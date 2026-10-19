/*
 * test_pipeline.cpp -- 端到端流水线测试（在 Linux 上真实运行）。
 *
 * 链路（全部是真东西，只有"内核驱动"被 Fake 后端替换）：
 *
 *   发送线程：合成扫频 -> libopus 编码 -> miclink_packet_encode -> 真实 UDP socket
 *             -> 127.0.0.1:<port>
 *   服务端  ：udp_receiver（真 socket）-> miclink_packet_decode -> opus 解码
 *             -> 抖动缓冲 -> (服务端重采样) -> 去咔哒/增益 -> PcmRing
 *             -> Injector -> FakeDeviceBackend（模拟内核消费）
 *
 * 场景矩阵：
 *   A  48k 采集 -> 48k 端点，20ms，0% 丢包   ：断言样本数精确、序列号连续、频率正确
 *   B  同上，注入 5% 丢包                     ：断言 PLC 补齐后样本数仍然连续、无崩溃
 *   C  16k 采集 -> 48k 端点，soxr 上采样      ：断言重采样后样本数正确
 *   D  16k 采集 -> 48k 端点，speex 上采样     ：同上（验证第二后端可用）
 *
 * 运行：./build/test_pipeline
 */
#include <opus.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "fake_device.h"
#include "injector.h"
#include "log.h"
#include "net_socket.h"
#include "session.h"
#include "threads_sync.h"
#include "miclink/miclink_protocol.h"

namespace {

/* ----------------------------- 极简断言 ----------------------------- */
int g_checks = 0;
int g_failures = 0;
const char *g_current_case = "";

void check_begin_case(const char *name)
{
    g_current_case = name;
    std::fprintf(stdout, "\n==== 场景 %s ====\n", name);
}

void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL][%s] %s\n", g_current_case, what);
    } else {
        std::fprintf(stdout, "  [ ok ][%s] %s\n", g_current_case, what);
    }
    std::fflush(stdout); /* 与写 stderr 的日志交错时保持可读 */
}

void check_fmt(bool ok, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    check(ok, buf);
}

/* --------------------------- 信号与发送端 --------------------------- */
constexpr double kPi = 3.14159265358979323846;
constexpr double kSweepStartHz = 200.0;
constexpr double kSweepEndHz = 3500.0;
constexpr double kAmplitude = 0.5;
constexpr int kMaxPayloadBytes = 4000;

/** 线性扫频发生器（相位连续，用于按帧校验频率）。 */
class SweepGenerator {
public:
    SweepGenerator(double fs, double duration_sec) : fs_(fs), duration_(duration_sec) {}

    int16_t next()
    {
        const double t = static_cast<double>(n_) / fs_;
        const double freq = kSweepStartHz + (kSweepEndHz - kSweepStartHz) * (t / duration_);
        phase_ += 2.0 * kPi * freq / fs_;
        if (phase_ > 2.0 * kPi) {
            phase_ -= 2.0 * kPi;
        }
        ++n_;
        return static_cast<int16_t>(std::sin(phase_) * kAmplitude * 32767.0);
    }

    /** 第 index 帧中心时刻的期望频率（ Hz）。 */
    double expected_freq_at_frame(uint32_t index, uint32_t frame_ms) const
    {
        const double t = (static_cast<double>(index) + 0.5) * static_cast<double>(frame_ms) / 1000.0;
        return kSweepStartHz + (kSweepEndHz - kSweepStartHz) * (t / duration_);
    }

private:
    double fs_ = 48000.0;
    double duration_ = 2.0;
    double phase_ = 0.0;
    uint64_t n_ = 0;
};

/** 确定性伪随机（用于可复现的丢包注入）。 */
class Lcg {
public:
    explicit Lcg(uint32_t seed) : state_(seed) {}
    uint32_t next()
    {
        state_ = static_cast<uint32_t>((static_cast<uint64_t>(state_) * 1103515245u + 12345u) &
                                       0x7FFFFFFFu);
        return state_;
    }

private:
    uint32_t state_ = 1u;
};

/**
 * 带内 FEC 的发送端语义（镜像 android/.../MicCaptureService.kt 的 FEC 分支）。
 *
 *   kOff            不发冗余包（场景 A~D）。
 *   kConformant     合规：冗余包**复用被保护帧的 seq**，不占用新序号。
 *                   这是协议约定（服务端据此判定"重复"或"补洞"），
 *                   也是缺陷 D-01 修复后 Android 侧的行为。
 *   kNonConformant  非合规：冗余包**占用一个新 seq**（D-01 修复前 Android 侧的行为）。
 *                   服务端会把它当成新帧注入 -> 每 16 帧多产出 20ms -> 播放快 6.25%。
 */
enum class FecMode {
    kOff,
    kConformant,
    kNonConformant,
};

/** 发送端：真 socket + 真 libopus 编码。 */
class UdpSender {
public:
    UdpSender(uint32_t rate, uint32_t frame_ms, uint32_t packets, double duration_sec, double loss,
              const miclink::NetEndpoint &dst, FecMode fec_mode = FecMode::kOff,
              uint32_t fec_period = 16u)
        : rate_(rate),
          frame_ms_(frame_ms),
          packets_(packets),
          duration_(duration_sec),
          loss_(loss),
          dst_(dst),
          fec_mode_(fec_mode),
          fec_period_(fec_period)
    {
    }

    bool start(std::string *err)
    {
        frames_per_packet_ = static_cast<size_t>(rate_) * static_cast<size_t>(frame_ms_) / 1000u;
        if (frames_per_packet_ == 0u) {
            if (err != nullptr) {
                *err = "帧长为 0（采样率/帧长组合非法）";
            }
            return false;
        }

        int opus_err = OPUS_OK;
        encoder_ = opus_encoder_create(static_cast<opus_int32>(rate_), 1, OPUS_APPLICATION_AUDIO,
                                       &opus_err);
        if ((encoder_ == nullptr) || (opus_err != OPUS_OK)) {
            if (err != nullptr) {
                *err = std::string("opus_encoder_create 失败: ") + opus_strerror(opus_err);
            }
            return false;
        }
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(64000));

        miclink::UdpOptions opts;
        opts.port = 0u; /* 让系统分配源端口 */
        opts.non_blocking = true;
        opts.reuse_addr = true;
        uint16_t bound = 0u;
        sock_ = miclink::udp_open(opts, &bound, err);
        if (sock_ == miclink::kInvalidSocket) {
            return false;
        }

        thread_ = std::thread([this]() { this->run(); });
        return true;
    }

    void join()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /** 发送是否已结束（非阻塞查询）。 */
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    uint32_t sent() const { return sent_; }
    uint32_t dropped() const { return dropped_; }

    /** 实际发出的 FEC 冗余包数量。 */
    uint32_t fec_sent() const { return fec_sent_; }

    /** 发送端一共消耗了多少个 seq（真实帧 + 非合规模式下冗余包占用的额外 seq）。 */
    uint32_t seq_used() const { return seq_used_; }

private:
    /** 组一个包并发出去；成功返回 true。 */
    bool emit(uint32_t seq, uint32_t timestamp_ms, const uint8_t *payload, int payload_len,
              uint16_t flags)
    {
        uint8_t packet[MICLINK_PROTO_MAX_HEADER + MICLINK_PROTO_MAX_PAYLOAD];
        miclink_packet_header header {};
        header.magic = MICLINK_PROTOCOL_MAGIC;
        header.version = static_cast<uint16_t>(MICLINK_PROTOCOL_VERSION);
        header.header_size = static_cast<uint16_t>(MICLINK_PACKET_HEADER_SIZE);
        header.seq = seq;
        header.timestamp_ms = timestamp_ms;
        header.sample_rate = rate_;
        header.channels = 1u;
        header.frames_per_packet = static_cast<uint16_t>(frames_per_packet_);
        header.payload_bytes = static_cast<uint16_t>(payload_len);
        header.flags = flags;
        header.reserved = 0u;

        const int total = miclink_packet_encode(&header, payload, packet, sizeof(packet));
        if (total <= 0) {
            return false;
        }
        std::string send_err;
        const int n = miclink::udp_sendto(sock_, packet, static_cast<size_t>(total), dst_, &send_err);
        if (n < 0) {
            std::fprintf(stderr, "  [warn] 发送失败: %s\n", send_err.c_str());
            return false;
        }
        return true;
    }

    void run()
    {
        SweepGenerator gen(static_cast<double>(rate_), duration_);
        Lcg rng(20240607u);
        std::vector<int16_t> pcm(frames_per_packet_);
        std::vector<uint8_t> payload(kMaxPayloadBytes);
        /* 上一帧的 payload：FEC 冗余包就是它的拷贝（对应 Android 侧 encoder.plc()）。 */
        std::vector<uint8_t> last_payload(kMaxPayloadBytes);
        int last_len = 0;

        uint32_t seq = 0u;
        const uint32_t start_ms = miclink::steady_now_ms();
        for (uint32_t i = 0u; i < packets_; ++i) {
            for (size_t s = 0u; s < pcm.size(); ++s) {
                pcm[s] = gen.next();
            }

            const bool drop = (loss_ > 0.0) &&
                              ((rng.next() % 10000u) < static_cast<uint32_t>(loss_ * 100.0));
            if (drop) {
                ++dropped_;
                ++seq; /* 序号按真实帧推进：丢包也要留空位，否则服务端看不出丢包 */
                pace(start_ms, i);
                continue;
            }

            const opus_int32 encoded =
                opus_encode(encoder_, pcm.data(), static_cast<int>(pcm.size()), payload.data(),
                            static_cast<opus_int32>(payload.size()));
            if (encoded <= 0) {
                ++dropped_;
                ++seq;
                pace(start_ms, i);
                continue;
            }

            /* ---- 真实帧：只有真实帧才消耗序号 ---- */
            const uint32_t frame_seq = seq;
            const uint32_t frame_ts = i * frame_ms_;
            if (emit(frame_seq, frame_ts, payload.data(), static_cast<int>(encoded), 0u)) {
                ++sent_;
            }
            ++seq;

            /* ---- 带内 FEC：每 fec_period_ 个真实帧附一份"刚刚那一帧"的冗余拷贝 ---- */
            if ((fec_mode_ != FecMode::kOff) && (fec_period_ != 0u) &&
                (((frame_seq + 1u) % fec_period_) == 0u) && (last_len > 0)) {
                uint32_t red_seq = 0u;
                if (fec_mode_ == FecMode::kConformant) {
                    /*
                     * 合规语义：冗余包复用被保护帧的 seq。服务端抖动缓冲据此判定
                     * "重复"（原包已到 -> 丢弃）或"补洞"（原包丢失 -> 用于恢复），
                     * 冗余包永远不会变成额外的一帧。
                     */
                    red_seq = frame_seq;
                } else {
                    /*
                     * D-01 修复前 Android 侧的行为：冗余包占用一个全新 seq。
                     * 服务端无法区分它与"真实的新帧"，只能把它当新帧注入 —— 这就是
                     * 修复前每 16 帧多产出 20ms（播放快 6.25%）的根因。
                     */
                    red_seq = seq;
                }
                if (emit(red_seq, frame_ts, last_payload.data(), last_len, MICLINK_FLAG_FEC)) {
                    ++fec_sent_;
                }
                if (fec_mode_ == FecMode::kNonConformant) {
                    ++seq; /* 非合规：冗余包也消耗序号（bug 本身） */
                }
            }

            std::memcpy(last_payload.data(), payload.data(), static_cast<size_t>(encoded));
            last_len = static_cast<int>(encoded);

            pace(start_ms, i);
        }
        seq_used_ = seq;
        finished_.store(true, std::memory_order_release);
    }

    /** 按真实时间节奏发送（20ms 一包），让抖动缓冲面对真实的到达间隔。 */
    void pace(uint32_t start_ms, uint32_t index)
    {
        const uint32_t target = start_ms + (index + 1u) * frame_ms_;
        const uint32_t now = miclink::steady_now_ms();
        if ((target > now) && ((target - now) < 1000u)) {
            miclink::sleep_ms(target - now);
        }
    }

    uint32_t rate_ = 48000u;
    uint32_t frame_ms_ = 20u;
    uint32_t packets_ = 100u;
    double duration_ = 2.0;
    double loss_ = 0.0;
    miclink::NetEndpoint dst_;
    FecMode fec_mode_ = FecMode::kOff;
    uint32_t fec_period_ = 16u;
    size_t frames_per_packet_ = 0u; /* = rate_ * frame_ms_ / 1000，构造时算好 */
    ::OpusEncoder *encoder_ = nullptr;
    miclink::SocketHandle sock_ = miclink::kInvalidSocket;
    std::thread thread_;
    uint32_t sent_ = 0u;
    uint32_t dropped_ = 0u;
    uint32_t fec_sent_ = 0u;
    uint32_t seq_used_ = 0u; /* 发送端一共消耗了多少个 seq（= 服务端能看到的帧槽数） */
    std::atomic<bool> finished_{false};
};

/* ----------------------------- 分析工具 ----------------------------- */

/** 用过零率估计一段信号的主频（Hz）。 */
double estimate_freq_zero_cross(const int16_t *s, uint32_t n, uint32_t sample_rate)
{
    if ((s == nullptr) || (n < 4u)) {
        return 0.0;
    }
    uint32_t crossings = 0u;
    for (uint32_t i = 1u; i < n; ++i) {
        const bool prev_positive = (s[i - 1u] >= 0);
        const bool cur_positive = (s[i] >= 0);
        if (prev_positive != cur_positive) {
            ++crossings;
        }
    }
    /* 一次完整周期会产生 2 次过零。 */
    return (static_cast<double>(crossings) / 2.0) * static_cast<double>(sample_rate) /
           static_cast<double>(n);
}

/** 统计峰值电平与削波（|s| == 32767）样本数。 */
void analyze_level(const int16_t *s, size_t n, float *peak, size_t *clipped)
{
    int max_abs = 0;
    size_t clipped_count = 0u;
    for (size_t i = 0u; i < n; ++i) {
        const int v = (s[i] < 0) ? -static_cast<int>(s[i]) : static_cast<int>(s[i]);
        if (v > max_abs) {
            max_abs = v;
        }
        if (v >= 32767) {
            ++clipped_count;
        }
    }
    *peak = static_cast<float>(max_abs) / 32767.0f;
    *clipped = clipped_count;
}

/* ----------------------------- 场景定义 ----------------------------- */
struct Scenario {
    const char *name;
    uint32_t opus_rate;
    uint32_t endpoint_rate;
    uint32_t frame_ms;
    double duration_sec;
    double loss_percent;
    miclink::ResamplerBackend resampler;
    bool check_frequency;
    FecMode fec_mode = FecMode::kOff;
};

bool run_scenario(const Scenario &sc)
{
    check_begin_case(sc.name);

    std::string err;
    if (!miclink::net_startup(&err)) {
        check(false, "net_startup 失败");
        return false;
    }

    miclink::ServerConfig cfg;
    cfg.listen_port = 0u; /* 让系统分配端口，避免与并行测试冲突 */
    cfg.endpoint_sample_rate = sc.endpoint_rate;
    cfg.endpoint_channels = 1u;
    cfg.endpoint_bits_per_sample = 16u;
    cfg.expected_opus_rate = sc.opus_rate;
    cfg.expected_channels = 1u;
    cfg.frame_ms = sc.frame_ms;
    cfg.jitter_min_ms = 50u;
    cfg.jitter_max_ms = 100u;
    cfg.jitter_target_ms = 50u;
    cfg.ring_capacity_frames = 8192u;
    cfg.max_write_frames = 1024u;
    cfg.device_backend = "fake";
    cfg.fake_capacity_frames = 32768u;
    cfg.resampler = sc.resampler;
    cfg.resampler_quality = 4u;
    cfg.gain_db = 0.0f;
    cfg.declick = true;
    cfg.log_level = miclink::LogLevelValue::kWarn;
    cfg.enable_level_log = true;
    cfg.status_interval_ms = 0u;
    cfg.client_timeout_ms = 5000u;

    if (!miclink::config_validate(cfg, &err)) {
        check_fmt(false, "配置校验失败: %s", err.c_str());
        return false;
    }

    miclink::ServerSession session;
    if (!session.init(cfg, &err)) {
        check_fmt(false, "会话初始化失败: %s", err.c_str());
        return false;
    }
    if (!session.start(&err)) {
        check_fmt(false, "会话启动失败: %s", err.c_str());
        return false;
    }

    miclink::FakeDeviceBackend *fake =
        dynamic_cast<miclink::FakeDeviceBackend *>(session.injector().backend());
    if (fake == nullptr) {
        check(false, "设备后端不是 FakeDeviceBackend");
        session.request_stop();
        session.join();
        return false;
    }

    const uint16_t port = session.receiver().bound_port();
    const miclink::NetEndpoint dst = miclink::net_endpoint_from_ipv4(0x7F000001u, port);
    std::fprintf(stdout, "  服务端监听 127.0.0.1:%u（后端 %s，重采样 %s）\n",
                 static_cast<unsigned>(port), session.injector().backend_name(),
                 session.pipeline().resampler_backend_name());

    const uint32_t packets = static_cast<uint32_t>(sc.duration_sec * 1000.0 / sc.frame_ms);
    const uint32_t out_frames_per_packet = sc.endpoint_rate * sc.frame_ms / 1000u;
    uint64_t expected_frames = static_cast<uint64_t>(packets) * out_frames_per_packet;

    UdpSender sender(sc.opus_rate, sc.frame_ms, packets, sc.duration_sec, sc.loss_percent, dst,
                     sc.fec_mode);
    if (!sender.start(&err)) {
        check_fmt(false, "发送端启动失败: %s", err.c_str());
        session.request_stop();
        session.join();
        return false;
    }

    /* ---- 主循环：一边模拟内核消费（drain），一边等产出追平 ---- */
    std::vector<int16_t> captured;
    captured.reserve(static_cast<size_t>(expected_frames) + 8192u);
    std::vector<int16_t> drain_buf(8192u);

    auto drain_all = [&]() {
        for (;;) {
            const uint32_t n = fake->drain(drain_buf.data(), static_cast<uint32_t>(drain_buf.size()));
            if (n == 0u) {
                break;
            }
            captured.insert(captured.end(), drain_buf.begin(), drain_buf.begin() + n);
        }
    };

    const uint32_t deadline = miclink::steady_now_ms() + 20000u;
    uint64_t last_produced = 0u;
    uint32_t last_progress_ms = miclink::steady_now_ms();

    /*
     * 结束条件：发送端已结束 **且** 产出计数连续 800ms 不再增长 **且**
     * 注入已经把环形缓冲取空。这样能确保抖动缓冲里最后那 target_delay 毫秒
     * 的音频也真的被产出并注入（而不是靠容差提前收工）。
     */
    while (miclink::steady_now_ms() < deadline) {
        drain_all();
        const miclink::StatsSnapshot snap = session.stats().snapshot();
        const uint32_t now = miclink::steady_now_ms();
        if (snap.frames_produced != last_produced) {
            last_produced = snap.frames_produced;
            last_progress_ms = now;
        }
        if (sender.finished() && ((now - last_progress_ms) > 800u) &&
            (snap.frames_injected >= snap.frames_produced)) {
            break;
        }
        miclink::sleep_ms(1u);
    }

    sender.join();

    /* 退出前先取一次快照：session.join() 会 RESET(clearStats=1) 清零驱动侧计数。 */
    const uint64_t fake_frames_at_stop = fake->frames_written_total();
    const miclink::JitterCounters jc = session.pipeline().jitter().counters();

    session.request_stop();
    session.join();

    /* join() 内部 flush 了重采样残留，这里把它们也收进来（不再经过注入线程）。 */
    drain_all();
    uint64_t leftover = 0u;
    for (;;) {
        const uint32_t n = session.pipeline().consume(drain_buf.data(),
                                                      static_cast<uint32_t>(drain_buf.size()));
        if (n == 0u) {
            break;
        }
        captured.insert(captured.end(), drain_buf.begin(), drain_buf.begin() + n);
        leftover += n;
    }

    const miclink::StatsSnapshot snap = session.stats().snapshot();
    session.pipeline();
    std::fprintf(stdout,
                 "  发送 %u 包（主动丢弃 %u），收到 %llu 包，解码 %llu 帧，PLC %llu 帧，"
                 "产出 %llu 帧，注入 %llu 帧，Fake 收到 %llu 帧，收尾残留 %llu 帧\n"
                 "  抖动缓冲：入队 %llu / 产出 %llu / 缺失 %llu / 重复 %llu / 迟到 %llu / 驱逐 %llu\n",
                 packets, sender.dropped(),
                 static_cast<unsigned long long>(snap.packets_received),
                 static_cast<unsigned long long>(snap.frames_decoded),
                 static_cast<unsigned long long>(snap.frames_plc),
                 static_cast<unsigned long long>(snap.frames_produced),
                 static_cast<unsigned long long>(snap.frames_injected),
                 static_cast<unsigned long long>(fake_frames_at_stop),
                 static_cast<unsigned long long>(leftover),
                 static_cast<unsigned long long>(jc.inserted),
                 static_cast<unsigned long long>(jc.emitted),
                 static_cast<unsigned long long>(jc.missing),
                 static_cast<unsigned long long>(jc.duplicate),
                 static_cast<unsigned long long>(jc.late),
                 static_cast<unsigned long long>(jc.evicted));

    /* ---------------------------- 断言 ---------------------------- */
    bool ok = true;

    if (sc.fec_mode == FecMode::kNonConformant) {
        /*
         * 非合规发送端（修复前 Android 的行为）每发一个冗余包就多消耗一个 seq，
         * 于是"每 16 个真实帧多塞 1 帧"，产出速率比实时快 6.25%。服务端**无法**
         * 消掉这部分膨胀（冗余包的 seq 与"真实的新帧"在协议层面完全等价），
         * 结果就是抖动缓冲被持续积压、 oldest 帧被驱逐（见断言 7 的 evicted）。
         * 因此这里不能用"真实帧数"当期望值，改为按抖动缓冲的实际产出对账。
         *
         * 缺陷 D-01 的**主修在 Android 侧**（冗余包复用上一帧 seq，见场景 E）；
         * 服务端这一侧的加固只保证"冗余包不会被额外解码成新帧"。
         */
        expected_frames = static_cast<uint64_t>(jc.emitted) * out_frames_per_packet;
    }

    /* 1) 样本数：Fake 后端收到的 PCM 样本总数必须正确。 */
    const uint64_t tolerance = (sc.opus_rate == sc.endpoint_rate) ? 0u
                                                                  : (2u * out_frames_per_packet);
    const uint64_t diff = (captured.size() > expected_frames)
                              ? (captured.size() - expected_frames)
                              : (expected_frames - captured.size());
    ok = (diff <= tolerance);
    check_fmt(ok, "样本数：期望 %llu，实际 %llu（容差 %llu）",
              static_cast<unsigned long long>(expected_frames),
              static_cast<unsigned long long>(captured.size()),
              static_cast<unsigned long long>(tolerance));
    if (!ok) {
        return false;
    }

    /* 2) 注入路径没有丢帧（0% 丢包场景下必须一帧不落）。 */
    check_fmt(fake->frames_dropped_total() == 0u, "Fake 后端未发生丢帧（dropped=%llu）",
              static_cast<unsigned long long>(fake->frames_dropped_total()));
    check_fmt(snap.frames_dropped_ring == 0u, "用户态环形缓冲未丢帧（dropped=%llu）",
              static_cast<unsigned long long>(snap.frames_dropped_ring));

    /* 3) 序列号：0% 丢包时不得有任何缺失 / 重复 / 迟到。 */
    if (sc.loss_percent <= 0.0) {
        check_fmt(snap.jitter_missing == 0u, "无缺失帧（missing=%llu）",
                  static_cast<unsigned long long>(snap.jitter_missing));
        check_fmt(snap.jitter_duplicate == 0u, "无重复包（duplicate=%llu）",
                  static_cast<unsigned long long>(snap.jitter_duplicate));
        check_fmt(snap.jitter_late == 0u, "无迟到包（late=%llu）",
                  static_cast<unsigned long long>(snap.jitter_late));
    } else {
        const double plc_ratio = 100.0 * static_cast<double>(snap.frames_plc) /
                                 static_cast<double>((snap.frames_plc + snap.frames_decoded) > 0
                                                         ? (snap.frames_plc + snap.frames_decoded)
                                                         : 1);
        check_fmt((plc_ratio > 1.0) && (plc_ratio < 15.0),
                  "PLC 比例 %.2f%% 落在注入丢包 %.1f%% 的合理区间", plc_ratio, sc.loss_percent);
    }

    /* 4) 电平：峰值合理、无削波溢出、无 NaN（int16 下 NaN 表现为非法值，这里查溢出）。 */
    float peak = 0.0f;
    size_t clipped = 0u;
    analyze_level(captured.data(), captured.size(), &peak, &clipped);
    check_fmt((peak > 0.05f) && (peak <= 1.0f), "峰值电平 %.3f 落在 (0.05, 1.0]",
              static_cast<double>(peak));
    const double clip_ratio =
        100.0 * static_cast<double>(clipped) / static_cast<double>(captured.size() > 0u
                                                                       ? captured.size()
                                                                       : 1u);
    check_fmt(clip_ratio < 1.0, "削波样本占比 %.3f%% < 1%%（无溢出）", clip_ratio);

    /* 5) 逐帧频率校验：证明第 i 帧装的确实是第 i 帧的音频（无乱序/重复/错位）。 */
    if (sc.check_frequency && (sc.opus_rate == sc.endpoint_rate)) {
        SweepGenerator reference(static_cast<double>(sc.endpoint_rate), sc.duration_sec);
        uint32_t bad_frames = 0u;
        uint32_t checked = 0u;
        const size_t frame_samples = out_frames_per_packet;
        for (size_t offset = 3u * frame_samples; offset + frame_samples <= captured.size();
             offset += frame_samples) {
            const uint32_t index = static_cast<uint32_t>(offset / frame_samples);
            if (index >= packets) {
                break;
            }
            const double expected_hz = reference.expected_freq_at_frame(index, sc.frame_ms);
            const double measured_hz = estimate_freq_zero_cross(
                captured.data() + offset, static_cast<uint32_t>(frame_samples), sc.endpoint_rate);
            const double rel = std::fabs(measured_hz - expected_hz) / expected_hz;
            ++checked;
            if (rel > 0.20) {
                ++bad_frames;
                if (bad_frames <= 3u) {
                    std::fprintf(stderr, "    帧 %u 频率偏差 %.1f%%（期望 %.1f Hz，实测 %.1f Hz）\n",
                                 index, rel * 100.0, expected_hz, measured_hz);
                }
            }
        }
        check_fmt((checked > 10u) && (bad_frames == 0u),
                  "逐帧频率校验：%u 帧全部匹配（越界帧 %u）", checked, bad_frames);
    }

    /* 6) 抖动缓冲目标延迟必须在配置区间内自适应。 */
    check_fmt((snap.jitter_target_ms >= 50u) && (snap.jitter_target_ms <= 100u),
              "抖动目标延迟 %ums 在 [50,100] 区间", snap.jitter_target_ms);

    /* 7) FEC 专项（缺陷 D-01）：带内 FEC 冗余包绝不能被当成"新的一帧"。 */
    if (sc.fec_mode != FecMode::kOff) {
        const uint32_t fec_packets = sender.fec_sent();
        std::fprintf(stdout, "  FEC：模式=%s，冗余包 %u 个，发送端共消耗 seq %u 个，"
                             "解码 %llu 帧，重复 %llu 帧\n",
                     (sc.fec_mode == FecMode::kConformant) ? "合规（复用上一帧 seq）"
                                                           : "非合规（冗余包占用新 seq）",
                     fec_packets, sender.seq_used(),
                     static_cast<unsigned long long>(snap.frames_decoded),
                     static_cast<unsigned long long>(snap.jitter_duplicate));

        if (sc.fec_mode == FecMode::kConformant) {
            /*
             * 合规语义下，冗余包与被保护帧同 seq，服务端必须认出它是冗余副本：
             *   - 原包已到 -> 拒收，且**不解码**（decoded 只统计真实帧）；
             *   - 原包丢失 -> 用它补洞（这条路径由抖动缓冲的 PLC 覆盖）。
             * 修复前服务端从不看 header.flags，会把每个冗余包都解码一遍再交给
             * 抖动缓冲判重 —— 于是 frames_decoded 比真实帧数多出冗余包的数量。
             */
            check_fmt(snap.frames_decoded == static_cast<uint64_t>(packets),
                      "冗余包未被当成新帧解码：解码 %llu 帧 == 真实帧 %u",
                      static_cast<unsigned long long>(snap.frames_decoded), packets);
            check_fmt(sender.seq_used() == packets,
                      "合规发送端不为冗余包消耗 seq：seq 槽位 %u == 真实帧 %u",
                      sender.seq_used(), packets);
            check_fmt(fec_packets > 0u, "本场景确实发出了 FEC 冗余包（%u 个）", fec_packets);
        } else {
            /*
             * 非合规发送端（修复前 Android 的行为）的能力边界刻画：
             *  1) 冗余包占用新 seq -> 产出速率比实时快 fec/packets（本场景 6/100）；
             *  2) 抖动缓冲（默认容量 = max_delay/frame_ms + 2 = 7 槽）被持续积压，
             *     oldest 帧被驱逐 —— 这就是 D-01 描述的"缓冲溢出丢帧"；
             *  3) 服务端**无法**把冗余包与"真实的新帧"区分开（协议层面等价），
             *     只能靠发送端修复（场景 E）来消除。
             * 这两条断言把上述行为钉死：既不放大、也不假装已经消掉。
             */
            check_fmt(jc.evicted > 0u,
                      "非合规 FEC 语义把抖动缓冲挤爆：驱逐 %llu 帧（真实帧 %u + 冗余包 %u = %u 个 "
                      "seq 槽，最终只产出 %llu 帧）",
                      static_cast<unsigned long long>(jc.evicted), packets, fec_packets,
                      sender.seq_used(), static_cast<unsigned long long>(jc.emitted));
            check_fmt(captured.size() != static_cast<uint64_t>(packets) * out_frames_per_packet,
                      "非合规语义下产出量必然偏离真实帧数（产出 %zu，真实帧应为 %llu）",
                      captured.size(),
                      static_cast<unsigned long long>(static_cast<uint64_t>(packets) *
                                                     out_frames_per_packet));
        }
    }

    return true;
}

/* --------------------- 场景 E：发现服务往返 --------------------- */
bool run_discovery_check()
{
    check_begin_case("E 发现服务往返");

    std::string err;
    if (!miclink::net_startup(&err)) {
        check(false, "net_startup 失败");
        return false;
    }

    miclink::ServerConfig cfg;
    cfg.listen_port = 0u;
    cfg.discovery_enabled = true;
    cfg.discovery_port = 0u; /* 由系统分配，避免端口冲突 */
    cfg.device_backend = "fake";
    cfg.log_level = miclink::LogLevelValue::kWarn;
    cfg.status_interval_ms = 0u;

    miclink::ServerSession session;
    if (!session.init(cfg, &err) || !session.start(&err)) {
        check_fmt(false, "会话启动失败: %s", err.c_str());
        return false;
    }
    const uint16_t discovery_port = session.discovery().bound_port();
    check_fmt(discovery_port != 0u, "发现服务已绑定端口 %u", static_cast<unsigned>(discovery_port));
    if (discovery_port == 0u) {
        session.request_stop();
        session.join();
        return false;
    }

    miclink::UdpOptions opts;
    opts.port = 0u;
    opts.non_blocking = true;
    opts.enable_broadcast = true;
    uint16_t client_port = 0u;
    const miclink::SocketHandle sock = miclink::udp_open(opts, &client_port, &err);
    if (sock == miclink::kInvalidSocket) {
        check_fmt(false, "查询端 socket 打开失败: %s", err.c_str());
        session.request_stop();
        session.join();
        return false;
    }

    const std::string query = "MICLINK-DISCOVER 1";
    const miclink::NetEndpoint dst = miclink::net_endpoint_from_ipv4(0x7F000001u, discovery_port);
    const int sent = miclink::udp_sendto(sock, reinterpret_cast<const uint8_t *>(query.data()),
                                       query.size(), dst, &err);
    check_fmt(sent == static_cast<int>(query.size()), "已发送发现查询（%d 字节）", sent);

    std::string reply;
    const uint32_t deadline = miclink::steady_now_ms() + 3000u;
    while (miclink::steady_now_ms() < deadline) {
        std::string wait_err;
        if (miclink::udp_wait_readable(sock, 100u, &wait_err) <= 0) {
            continue;
        }
        uint8_t buf[512];
        miclink::NetEndpoint from;
        const int n = miclink::udp_recvfrom(sock, buf, sizeof(buf), &from, &err);
        if (n > 0) {
            reply.assign(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
            break;
        }
    }

    check_fmt(reply.rfind("MICLINK-HERE", 0) == 0, "收到发现应答：%s",
              reply.empty() ? std::string("(空)").c_str() : reply.c_str());

    /* 应答里必须带上音频端口与端点格式，手机端据此直连。 */
    const bool has_port = reply.find(std::to_string(session.receiver().bound_port())) !=
                          std::string::npos;
    check_fmt(has_port, "应答中包含音频端口 %u",
              static_cast<unsigned>(session.receiver().bound_port()));
    check_fmt(reply.find("48000") != std::string::npos, "应答中包含端点采样率 48000");

    miclink::udp_close(sock);
    session.request_stop();
    session.join();
    return true;
}

}  // namespace

int main()
{
    std::fprintf(stdout, "miclinkserver 端到端流水线测试（Linux，Fake 设备后端 + 真实 UDP/opus）\n");

    const Scenario scenarios[] = {
        {"A 48k->48k 直通 0% 丢包", 48000u, 48000u, 20u, 2.0, 0.0,
         miclink::ResamplerBackend::kAuto, true},
        {"B 48k->48k 直通 5% 丢包", 48000u, 48000u, 20u, 2.0, 5.0,
         miclink::ResamplerBackend::kAuto, false},
        {"C 16k->48k soxr 上采样", 16000u, 48000u, 20u, 1.5, 0.0,
         miclink::ResamplerBackend::kSoxr, false},
        {"D 16k->48k speex 上采样", 16000u, 48000u, 20u, 1.5, 0.0,
         miclink::ResamplerBackend::kSpeex, false},
        /* E/F：缺陷 D-01 的回归场景。E 镜像"修复后"的 Android 语义（冗余包复用
           上一帧 seq），F 镜像"修复前"的非合规语义（冗余包占用新 seq）。 */
        {"E FEC 开启（合规语义：冗余包复用上一帧 seq）", 48000u, 48000u, 20u, 2.0, 0.0,
         miclink::ResamplerBackend::kAuto, true, FecMode::kConformant},
        {"F FEC 开启（非合规语义：冗余包占用新 seq）", 48000u, 48000u, 20u, 2.0, 0.0,
         miclink::ResamplerBackend::kAuto, false, FecMode::kNonConformant},
    };

    for (const Scenario &sc : scenarios) {
        run_scenario(sc);
    }
    run_discovery_check();

    std::fprintf(stdout, "\n==== 汇总：%d 项断言，%d 项失败 ====\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "测试失败\n");
        return 1;
    }
    std::fprintf(stdout, "测试通过\n");
    return 0;
}
