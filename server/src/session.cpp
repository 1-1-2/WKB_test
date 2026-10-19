/*
 * session.cpp -- 会话编排实现。
 */
#include "session.h"

#include <cstdio>

#include "log.h"

namespace womic {

static constexpr char kWomicLogModule[] = "session";

namespace {
constexpr uint32_t kRxPollTimeoutMs = 2u;      /* 收包等待上限：同时是播放时钟的推进节拍 */
constexpr int kMaxProducePerWake = 8;          /* 单次唤醒最多产出多少帧（防止长循环） */
constexpr uint32_t kInjectIdleSleepMs = 1u;    /* 环形缓冲空时的休眠 */
constexpr uint32_t kStatusPollIntervalMs = 500u;
constexpr size_t kInjectBufferFrames = 4096u;
}  // namespace

ServerSession::ServerSession()
    : rx_thread_("rx", ThreadPriority::kCritical),
      inject_thread_("inject", ThreadPriority::kHigh)
{
}

ServerSession::~ServerSession()
{
    request_stop();
    join();
}

bool ServerSession::init(const ServerConfig &cfg, std::string *err)
{
    cfg_ = cfg;

    std::string pipeline_err;
    if (!pipeline_.init(cfg, &pipeline_err)) {
        if (err != nullptr) {
            *err = pipeline_err;
        }
        return false;
    }

    if (!injector_.open(cfg, err)) {
        return false;
    }

    inject_buf_.assign(kInjectBufferFrames, 0);
    return true;
}

bool ServerSession::start(std::string *err)
{
    if (!receiver_.open(cfg_.listen_port, cfg_.bind_ipv4, cfg_.udp_recv_buf_bytes, err)) {
        return false;
    }

    /*
     * 配置里 listen_port 可以是 0（由系统分配）。发现服务必须广播**实际**端口，
     * 否则手机端会拿着 0 去连；这里把实际端口写回配置。
     */
    cfg_.listen_port = receiver_.bound_port();

    running_.store(true, std::memory_order_release);

    if (!rx_thread_.start([this](const std::atomic<bool> &stop) { this->rx_loop(stop); })) {
        running_.store(false, std::memory_order_release);
        if (err != nullptr) {
            *err = "rx 线程启动失败";
        }
        return false;
    }
    if (!inject_thread_.start([this](const std::atomic<bool> &stop) { this->inject_loop(stop); })) {
        running_.store(false, std::memory_order_release);
        rx_thread_.request_stop();
        rx_thread_.join(3000u);
        if (err != nullptr) {
            *err = "inject 线程启动失败";
        }
        return false;
    }

    if (cfg_.discovery_enabled) {
        std::string discovery_err;
        if (!discovery_.start(cfg_, &discovery_err)) {
            WOMIC_LOGW("发现服务启动失败（不影响音频）：%s", discovery_err.c_str());
        }
    }

    WOMIC_LOGI("会话已启动：UDP :%u -> 端点 %u Hz/%u ch（后端 %s）",
               static_cast<unsigned>(receiver_.bound_port()), cfg_.endpoint_sample_rate,
               cfg_.endpoint_channels, injector_.backend_name());
    return true;
}

void ServerSession::request_stop()
{
    running_.store(false, std::memory_order_release);
    rx_thread_.request_stop();
    inject_thread_.request_stop();
}

void ServerSession::join()
{
    rx_thread_.join(5000u);
    inject_thread_.join(5000u);

    /* 收尾：把重采样器里残留的样本挤进环形缓冲（仅重采样路径有意义）。 */
    pipeline_.flush_resampler(stats_);

    discovery_.stop();
    receiver_.close();
    injector_.close();
}

void ServerSession::rx_loop(const std::atomic<bool> &stop)
{
    WOMIC_LOGV("rx 线程进入循环");
    while (!stop.load(std::memory_order_acquire)) {
        UdpReceiver::Packet packet;
        const int got = receiver_.poll(kRxPollTimeoutMs, &packet, &stats_);
        if (got == 1) {
            const AudioPipeline::FeedResult fed =
                pipeline_.feed(packet.data, packet.size, packet.arrival_ms, stats_);
            if (fed == AudioPipeline::FeedResult::kInvalidPacket) {
                WOMIC_LOGV("丢弃一个非法包（%zu 字节，来自 %s）", packet.size,
                           packet.from.to_string().c_str());
            }
        } else if (got < 0) {
            /* socket 致命错误：稍等一下再试，避免忙等刷屏。 */
            sleep_ms(5);
        }

        /*
         * 播放时钟推进：不管有没有收到包都要调用 produce()，
         * 因为 womic_jb_get 是靠"被调用的时刻"推进播放时间轴的。
         */
        const uint32_t now = steady_now_ms();
        for (int i = 0; i < kMaxProducePerWake; ++i) {
            const ProducedFrame frame = pipeline_.produce(now, stats_);
            if (frame.frames == 0u) {
                break;
            }
            last_produced_seq_ = frame.seq;
            ++produced_frame_count_;
            if (frame.plc) {
                WOMIC_LOGV("seq %u 缺失，已用 PLC 补齐", frame.seq);
            }
        }

        receiver_.check_client_timeout(now, cfg_.client_timeout_ms, &stats_);
    }
    WOMIC_LOGV("rx 线程退出（累计产出 %u 帧）", produced_frame_count_);
}

void ServerSession::inject_loop(const std::atomic<bool> &stop)
{
    WOMIC_LOGV("inject 线程进入循环");
    uint32_t last_status_ms = steady_now_ms();

    while (!stop.load(std::memory_order_acquire)) {
        const uint32_t max_frames = static_cast<uint32_t>(inject_buf_.size()) /
                                    ((pipeline_.out_samples_per_frame() > 0u)
                                         ? pipeline_.out_samples_per_frame()
                                         : 1u);
        const uint32_t frames = pipeline_.consume(inject_buf_.data(), max_frames);
        if (frames > 0u) {
            const uint32_t written = injector_.write_frames(inject_buf_.data(), frames, &stats_);
            if (written == 0u) {
                /* 设备满了：稍等再取，绝不忙等。 */
                sleep_ms(kInjectIdleSleepMs);
            }
        } else {
            sleep_ms(kInjectIdleSleepMs);
        }

        const uint32_t now = steady_now_ms();
        if ((now - last_status_ms) >= kStatusPollIntervalMs) {
            last_status_ms = now;
            WOMIC_STATUS status {};
            if (injector_.poll_status(&stats_, &status)) {
                last_status_ = status;
            }
        }
    }
    WOMIC_LOGV("inject 线程退出");
}

double ServerSession::estimate_latency_ms(const StatsSnapshot &snap) const
{
    /*
     * 端到端延迟（估计）：
     *   抖动缓冲目标延迟 + 用户态环形缓冲深度 + 内核环形缓冲深度 + 固定算法延迟。
     * 端点采样率已知，因此帧数可以直接换算成毫秒。
     */
    const double rate = static_cast<double>(cfg_.endpoint_sample_rate);
    if (rate <= 0.0) {
        return 0.0;
    }
    const double jitter_ms = static_cast<double>(snap.jitter_depth_ms);
    const double user_ring_ms = 1000.0 * static_cast<double>(snap.ring_available) / rate;
    const double kernel_ring_ms = 1000.0 * static_cast<double>(snap.device_ring_available) / rate;
    const double codec_ms = 6.5; /* Opus lookahead 的典型 6.5ms，仅作常数估计 */
    return jitter_ms + user_ring_ms + kernel_ring_ms + codec_ms;
}

}  // namespace womic
