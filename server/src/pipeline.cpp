/*
 * pipeline.cpp -- 音频流水线实现。
 */
#include "pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "miclink/miclink_protocol.h"

namespace miclink {

static constexpr char kMicLinkLogModule[] = "pipe";

namespace {
/* 60ms @ 48kHz @ 2ch = 5760 样本，是本流水线能接受的单个最大帧。 */
constexpr size_t kMaxFrameSamples = 8192u;
}  // namespace

bool AudioPipeline::init(const ServerConfig &cfg, std::string *err)
{
    cfg_ = cfg;
    endpoint_rate_ = cfg.endpoint_sample_rate;
    endpoint_channels_ = (cfg.endpoint_channels == 0u) ? 1u : cfg.endpoint_channels;
    out_samples_per_frame_ = endpoint_channels_;

    capture_rate_ = cfg.expected_opus_rate;
    capture_channels_ = (cfg.expected_channels == 0u) ? 1u : cfg.expected_channels;
    capture_frame_ms_ = cfg.frame_ms;

    std::string decoder_err;
    if (!decoder_.init(capture_rate_, capture_channels_, &decoder_err)) {
        if (err != nullptr) {
            *err = std::string("Opus 解码器初始化失败: ") + decoder_err;
        }
        return false;
    }
    if ((cfg.gain_db != 0.0f) && !decoder_.set_gain_db(cfg.gain_db, &decoder_err)) {
        MICLINK_LOGW("设置 Opus 解码增益失败（改用 miclink_apply_gain 兜底）：%s",
                   decoder_err.c_str());
    }
    /* 输出侧增益统一由 miclink_apply_gain 施加（带饱和 clamp），
       Opus 侧的 SET_GAIN 只作为额外调节手段，两者互不冲突。 */
    linear_gain_ = std::pow(10.0f, cfg.gain_db / 20.0f);

    std::string jitter_err;
    if (!jitter_.init(capture_rate_, static_cast<uint16_t>(capture_channels_), capture_frame_ms_,
                      cfg.jitter_min_ms, cfg.jitter_max_ms, cfg.jitter_target_ms,
                      cfg.jitter_capacity_frames, &jitter_err)) {
        if (err != nullptr) {
            *err = std::string("抖动缓冲初始化失败: ") + jitter_err;
        }
        return false;
    }

    Resampler::Backend preferred = Resampler::Backend::kSoxr;
    switch (cfg.resampler) {
        case ResamplerBackend::kAuto:
            preferred = Resampler::Backend::kSoxr; /* init 内部会自动降级 */
            break;
        case ResamplerBackend::kSoxr:
            preferred = Resampler::Backend::kSoxr;
            break;
        case ResamplerBackend::kSpeex:
            preferred = Resampler::Backend::kSpeex;
            break;
        case ResamplerBackend::kNone:
            preferred = Resampler::Backend::kNone;
            break;
    }
    Resampler::Backend actual = Resampler::Backend::kNone;
    std::string resampler_err;
    if (!resampler_.init(preferred, capture_rate_, endpoint_rate_, endpoint_channels_,
                         cfg.resampler_quality, &actual, &resampler_err)) {
        if (err != nullptr) {
            *err = std::string("重采样器初始化失败: ") + resampler_err;
        }
        return false;
    }
    MICLINK_LOGI("重采样：%u Hz -> %u Hz，后端 %s", capture_rate_, endpoint_rate_,
               resampler_.backend_name());

    const uint32_t frame_bytes = endpoint_channels_ * (cfg.endpoint_bits_per_sample / 8u);
    std::string ring_err;
    if (!ring_.init(cfg.ring_capacity_frames, frame_bytes, &ring_err)) {
        if (err != nullptr) {
            *err = std::string("环形缓冲初始化失败: ") + ring_err;
        }
        return false;
    }

    decode_buf_.assign(kMaxFrameSamples, 0);
    out_buf_.assign(kMaxFrameSamples, 0);

    miclink_declck_init(&declck_, MICLINK_DECLCK_DEFAULT_FADE);

    ready_ = true;
    return true;
}

bool AudioPipeline::ensure_capture_format(uint32_t rate, uint32_t channels, uint32_t frame_ms,
                                         std::string *err)
{
    if ((rate == capture_rate_) && (channels == capture_channels_) &&
        (frame_ms == capture_frame_ms_)) {
        return true;
    }

    MICLINK_LOGI("采集格式切换：%u Hz/%u ch/%u ms -> %u Hz/%u ch/%u ms（重建解码器与抖动缓冲）",
               capture_rate_, capture_channels_, capture_frame_ms_, rate, channels, frame_ms);

    capture_rate_ = rate;
    capture_channels_ = channels;
    capture_frame_ms_ = frame_ms;

    std::string decoder_err;
    if (!decoder_.init(rate, channels, &decoder_err)) {
        if (err != nullptr) {
            *err = std::string("切换采集格式后重建解码器失败: ") + decoder_err;
        }
        return false;
    }
    if (cfg_.gain_db != 0.0f) {
        std::string gain_err;
        if (!decoder_.set_gain_db(cfg_.gain_db, &gain_err)) {
            MICLINK_LOGW("重建解码器后设置增益失败：%s", gain_err.c_str());
        }
    }

    std::string jitter_err;
    if (!jitter_.init(rate, static_cast<uint16_t>(channels), frame_ms, cfg_.jitter_min_ms,
                      cfg_.jitter_max_ms, cfg_.jitter_target_ms, cfg_.jitter_capacity_frames,
                      &jitter_err)) {
        if (err != nullptr) {
            *err = std::string("切换采集格式后重建抖动缓冲失败: ") + jitter_err;
        }
        return false;
    }

    Resampler::Backend preferred = Resampler::Backend::kSoxr;
    if (cfg_.resampler == ResamplerBackend::kSpeex) {
        preferred = Resampler::Backend::kSpeex;
    } else if (cfg_.resampler == ResamplerBackend::kNone) {
        preferred = Resampler::Backend::kNone;
    }
    Resampler::Backend actual = Resampler::Backend::kNone;
    std::string resampler_err;
    if (!resampler_.init(preferred, rate, endpoint_rate_, endpoint_channels_,
                         cfg_.resampler_quality, &actual, &resampler_err)) {
        if (err != nullptr) {
            *err = std::string("切换采集格式后重建重采样器失败: ") + resampler_err;
        }
        return false;
    }
    MICLINK_LOGI("重采样已重建：%u Hz -> %u Hz，后端 %s", rate, endpoint_rate_,
               resampler_.backend_name());
    return true;
}

AudioPipeline::FeedResult AudioPipeline::feed(const uint8_t *buf, size_t len, uint32_t now_ms,
                                              Stats &stats)
{
    if (!ready_) {
        return FeedResult::kInvalidPacket;
    }

    miclink_packet_header header {};
    const uint8_t *payload = nullptr;
    int consumed = miclink_packet_decode(buf, len, &header, &payload);
    if (consumed < 0) {
        stats.on_packet_invalid();
        MICLINK_LOGV("协议头校验失败（%d），丢弃 %zu 字节", consumed, len);
        return FeedResult::kInvalidPacket;
    }

    if ((header.sample_rate == 0u) || (header.channels == 0u) ||
        (header.frames_per_packet == 0u)) {
        stats.on_packet_invalid();
        return FeedResult::kInvalidPacket;
    }
    if (!OpusDecoderWrapper::is_valid_frame_size(header.sample_rate, header.frames_per_packet)) {
        stats.on_packet_invalid();
        MICLINK_LOGV("非法 Opus 帧长 %u（采样率 %u），丢弃 seq=%u", header.frames_per_packet,
                   header.sample_rate, header.seq);
        return FeedResult::kInvalidPacket;
    }
    if ((payload == nullptr) && (header.payload_bytes > 0u)) {
        stats.on_packet_invalid();
        return FeedResult::kInvalidPacket;
    }

    /* 由帧样本数反推帧长（ms2 = ms*2，避免 2.5ms 的浮点）。 */
    const uint32_t ms2 = static_cast<uint32_t>(static_cast<uint64_t>(header.frames_per_packet) *
                                               2000u / static_cast<uint64_t>(header.sample_rate));
    uint32_t frame_ms = 0u;
    switch (ms2) {
        case 10u:
            frame_ms = 5u;
            break;
        case 20u:
            frame_ms = 10u;
            break;
        case 40u:
            frame_ms = 20u;
            break;
        case 80u:
            frame_ms = 40u;
            break;
        case 120u:
            frame_ms = 60u;
            break;
        default:
            /* 2.5ms(ms2=5) 与 60ms 之外的非整数/超长帧：抖动缓冲要求整数 ms，拒收。 */
            stats.on_packet_invalid();
            return FeedResult::kInvalidPacket;
    }

    std::string err;
    if (!ensure_capture_format(header.sample_rate, header.channels, frame_ms, &err)) {
        MICLINK_LOGW("采集格式切换失败：%s", err.c_str());
        stats.on_packet_invalid();
        return FeedResult::kInvalidPacket;
    }

    const uint32_t samples_per_frame = jitter_.samples_per_frame();
    if (samples_per_frame == 0u) {
        return FeedResult::kInvalidPacket;
    }
    const uint32_t needed = samples_per_frame * header.channels;
    if (needed > kMaxFrameSamples) {
        stats.on_packet_invalid();
        return FeedResult::kInvalidPacket;
    }

    /*
     * ---- 带内 FEC（MICLINK_FLAG_FEC）--------------------------------------
     * 协议约定：带 FEC 标志的包是"它所标注 seq 那一帧"的**冗余副本**。发送端
     * 必须复用被保护帧的 seq、且不占用新序号（见 android/.../MicCaptureService.kt）。
     *
     * 服务端不无条件信任发送端，按两条规则处理：
     *   1) 那一帧已经收到（或早已播出）-> 纯冗余：直接拒收，**不解码、不入队**。
     *      它绝不能变成额外的一帧 —— 否则每 16 帧多产出 20ms，播放速率快
     *      6.25%，抖动缓冲被持续积压直至驱逐丢帧（缺陷 D-01）。
     *   2) 那一帧确实缺失 -> 用它补洞：走下面正常的解码入队路径。
     *
     * 修复前这里从不读 header.flags，每个冗余包都要先做一次 Opus 解码再交给
     * 抖动缓冲判重：既白烧 CPU，也让"解码帧数"统计比真实帧数多出冗余包数量。
     *
     * 已知的能力边界：如果发送端**违反**约定给冗余包分配了新 seq（旧版本的
     * bug），服务端在协议层面无法把它与"真实的新帧"区分开，只能照单全收；
     * 那一部分膨胀只能由发送端修复消除（见 tests/test_pipeline.cpp 场景 F）。
     */
    if ((header.flags & MICLINK_FLAG_FEC) != 0u) {
        if (!jitter_.is_seq_missing(header.seq)) {
            MICLINK_LOGV("FEC 冗余包 seq=%u 无缺口可补，拒收（不解码、不入队）", header.seq);
            return FeedResult::kNotAccepted;
        }
        MICLINK_LOGV("FEC 冗余包 seq=%u 用于补洞", header.seq);
    }

    int decoded = 0;
    if ((header.payload_bytes > 0u) && (payload != nullptr)) {
        decoded = decoder_.decode(payload, static_cast<int>(header.payload_bytes),
                                  decode_buf_.data(), static_cast<int>(samples_per_frame));
    } else {
        /* DTX / 空负载：直接按丢包处理，交 PLC。 */
        decoded = decoder_.decode_plc(decode_buf_.data(), static_cast<int>(samples_per_frame));
    }
    if (decoded < 0) {
        stats.on_decode_error();
        return FeedResult::kDecodeError;
    }
    if (static_cast<uint32_t>(decoded) < samples_per_frame) {
        /* Opus 在极少数情况下会少给几个样本（如最后一次 PLC），补零保证帧长恒定。 */
        for (uint32_t i = static_cast<uint32_t>(decoded) * header.channels; i < needed; ++i) {
            decode_buf_[i] = 0;
        }
    }
    stats.on_frame_decoded();

    const JitterPushResult push_result =
        jitter_.push(header.seq, decode_buf_.data(), samples_per_frame, now_ms);
    switch (push_result) {
        case JitterPushResult::kAccepted:
            return FeedResult::kOk;
        case JitterPushResult::kDuplicate:
            stats.on_jitter_duplicate();
            return FeedResult::kNotAccepted;
        case JitterPushResult::kLate:
            stats.on_jitter_late();
            return FeedResult::kNotAccepted;
        default:
            stats.on_packet_invalid();
            return FeedResult::kInvalidPacket;
    }
}

ProducedFrame AudioPipeline::produce(uint32_t now_ms, Stats &stats)
{
    ProducedFrame result {};
    if (!ready_) {
        return result;
    }

    const uint32_t samples_per_frame = jitter_.samples_per_frame();
    if (samples_per_frame == 0u) {
        return result;
    }

    int missing = 0;
    uint32_t seq = 0u;
    const JitterPullResult pull_result =
        jitter_.pull(decode_buf_.data(), samples_per_frame, &missing, &seq, now_ms);
    if (pull_result != JitterPullResult::kProduced) {
        /* 还不到播的时候（起播缓冲中）或缓冲已排空：本轮不产出。 */
        stats.set_jitter(jitter_.target_delay_ms(), jitter_.depth_ms(), jitter_.counters().stalls);
        return result;
    }
    result.seq = seq;

    if (missing != 0) {
        /*
         * 该 seq 的包丢了：用 Opus PLC 补一帧（共享同一解码器状态，
         * 因此外推出来的波形与前一帧连续，听感远好于填静音）。
         */
        const int plc_samples =
            decoder_.decode_plc(decode_buf_.data(), static_cast<int>(samples_per_frame));
        if (plc_samples < 0) {
            for (uint32_t i = 0u; i < samples_per_frame * capture_channels_; ++i) {
                decode_buf_[i] = 0;
            }
        }
        result.plc = true;
        stats.on_frame_plc();
        stats.on_jitter_missing();
        /* 通知去咔哒：接下来要做淡入，避免从上一帧末尾直接跳变。 */
        if (cfg_.declick) {
            miclink_declck_notify_loss(&declck_);
        }
    }

    /* ---- 服务端重采样：采集率 -> 端点率（驱动侧绝不做重采样） ---- */
    const uint32_t out_capacity = static_cast<uint32_t>(out_buf_.size()) / endpoint_channels_;
    int out_frames = resampler_.process(decode_buf_.data(), samples_per_frame, out_buf_.data(),
                                        out_capacity);
    if (out_frames < 0) {
        MICLINK_LOGW("重采样失败(%d)，本帧丢弃", out_frames);
        stats.on_ring_drop(1u);
        return result;
    }
    if (out_frames == 0) {
        /*
         * 这是**正常**情况而不是错误：libsoxr 按内部块处理（实测 16k->48k 时
         * 每 4 个输入帧产出一次 3300 样本），前几帧 odone 为 0，数据只是还留在
         * 重采样器内部，样本总数严格守恒（见 resampler 的 flush()）。
         * 因此这里既不计数丢帧，也不告警。
         */
        MICLINK_LOGV("重采样器本轮未产出（内部滤波仍在填充，样本未丢失）");
        return result;
    }
    const uint32_t produced = static_cast<uint32_t>(out_frames);
    const uint32_t produced_samples = produced * endpoint_channels_;

    /* ---- DSP：去咔哒 -> 增益（带饱和）-> 电平测量 ---- */
    if (cfg_.declick) {
        miclink_apply_declck(&declck_, out_buf_.data(), produced_samples);
    }
    if (cfg_.gain_db != 0.0f) {
        miclink_apply_gain(out_buf_.data(), produced_samples, linear_gain_);
    }
    float rms = 0.0f;
    float peak = 0.0f;
    miclink_measure_level(out_buf_.data(), produced_samples, &rms, &peak);
    stats.set_level(rms, peak);

    const uint32_t written = ring_.write(out_buf_.data(), produced);
    if (written < produced) {
        /* 环形缓冲满：注入线程跟不上。记丢帧，绝不阻塞接收线程。 */
        ring_.add_dropped(static_cast<uint64_t>(produced - written));
        stats.on_ring_drop(produced - written);
    }
    stats.on_frame_produced(written);
    produced_frames_ += written;

    stats.set_jitter(jitter_.target_delay_ms(), jitter_.depth_ms(), jitter_.counters().stalls);
    stats.set_ring(ring_.available(), ring_.free_frames());

    result.frames = written;
    return result;
}

uint32_t AudioPipeline::consume(int16_t *dst, uint32_t max_frames)
{
    if (!ready_ || (dst == nullptr) || (max_frames == 0u)) {
        return 0u;
    }
    return ring_.read(dst, max_frames);
}

void AudioPipeline::flush_resampler(Stats &stats)
{
    if (!ready_ || !resampler_.needed()) {
        return;
    }
    const uint32_t out_capacity = static_cast<uint32_t>(out_buf_.size()) / endpoint_channels_;
    const int flushed = resampler_.flush(out_buf_.data(), out_capacity);
    if (flushed <= 0) {
        return;
    }
    const uint32_t produced = static_cast<uint32_t>(flushed);
    const uint32_t written = ring_.write(out_buf_.data(), produced);
    if (written < produced) {
        ring_.add_dropped(static_cast<uint64_t>(produced - written));
        stats.on_ring_drop(produced - written);
    }
    stats.on_frame_produced(written);
    MICLINK_LOGI("重采样器收尾：挤出 %u 帧写入 %u 帧", produced, written);
}

uint32_t AudioPipeline::ring_available() const
{
    return ready_ ? ring_.available() : 0u;
}

uint32_t AudioPipeline::ring_free() const
{
    return ready_ ? ring_.free_frames() : 0u;
}

}  // namespace miclink
