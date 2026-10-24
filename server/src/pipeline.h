/*
 * pipeline.h -- 音频流水线装配（平台中立）。
 *
 * 数据流（全部发生在 UDP 接收线程内，故 Opus 解码器无需加锁）：
 *
 *   UDP 包 -> miclink_packet_decode -> opus_decode / PLC -> JitterPipeline
 *          -> (服务端重采样：采集率 -> 端点率) -> 去咔哒 + 增益 + 电平测量
 *          -> PcmRing --(线程边界)--> 注入线程 -> DeviceIoControl(WRITE_PCM)
 *
 * 关键点：
 *   1) 解码发生在"入队前"，因此抖动缓冲里存的是 PCM 而不是 Opus 负载；
 *      代价是丢包时无法用带内 FEC 重解，收益是播放侧逻辑极简且与内核端点的
 *      帧定义一致。PLC 只在"出队发现缺失"时补，用的是同一个解码器状态。
 *   2) 重采样发生在"出队后、入环形缓冲前"，且只在这里发生一次
 *      （驱动侧绝不做重采样）。
 *   3) PcmRing 是唯一跨越线程边界的数据结构，SPSC 无锁。
 */
#ifndef MICLINK_SERVER_PIPELINE_H
#define MICLINK_SERVER_PIPELINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "jitter_pipeline.h"
#include "opus_decoder.h"
#include "resampler.h"
#include "ring_adapter.h"
#include "stats.h"
#include "miclink/miclink_dsp.h"

namespace miclink {

/** produce() 的产出描述。 */
struct ProducedFrame {
    uint32_t frames = 0u; /* 写入环形缓冲的帧数（端点采样率下的帧） */
    uint32_t seq = 0u;    /* 该帧对应的协议序列号（占位帧也是它"本应有"的 seq） */
    bool plc = false;     /* true = 该帧由 Opus PLC 补齐 */
};

class AudioPipeline {
public:
    /** feed() 的结果。 */
    enum class FeedResult : int {
        kOk = 0,           /* 已解码并入队 */
        kInvalidPacket,    /* 协议头/参数非法 */
        kDecodeError,      /* Opus 解码失败（已用 PLC 兜底或放弃） */
        kNotAccepted,      /* 抖动缓冲拒收（重复/过晚） */
    };

    AudioPipeline() = default;
    ~AudioPipeline() = default;

    AudioPipeline(const AudioPipeline &) = delete;
    AudioPipeline &operator=(const AudioPipeline &) = delete;

    /** 按配置初始化解码器、抖动缓冲、重采样器与环形缓冲。 */
    bool init(const ServerConfig &cfg, std::string *err);

    bool ready() const { return ready_; }

    /** 喂一个 UDP 数据报（含协议头）。 */
    FeedResult feed(const uint8_t *buf, size_t len, uint32_t now_ms, Stats &stats);

    /** 播放时钟推进：从抖动缓冲取一帧 -> PLC -> 重采样 -> DSP -> 环形缓冲。 */
    ProducedFrame produce(uint32_t now_ms, Stats &stats);

    /** 注入线程侧：从环形缓冲取最多 max_frames 帧。 */
    uint32_t consume(int16_t *dst, uint32_t max_frames);

    /** 流结束时把重采样器残留样本挤进环形缓冲（可选）。 */
    void flush_resampler(Stats &stats);

    uint32_t ring_available() const;
    uint32_t ring_free() const;

    /** 端点侧每帧样本数（= 端点声道数）。 */
    uint32_t out_samples_per_frame() const { return out_samples_per_frame_; }
    uint32_t capture_rate() const { return capture_rate_; }
    uint32_t endpoint_rate() const { return endpoint_rate_; }
    const char *resampler_backend_name() const { return resampler_.backend_name(); }
    bool resampling_active() const { return resampler_.needed(); }

    const JitterPipeline &jitter() const { return jitter_; }

private:
    bool ensure_capture_format(uint32_t rate, uint32_t channels, uint32_t frame_ms,
                               std::string *err);

    ServerConfig cfg_ {};
    bool ready_ = false;

    OpusDecoderWrapper decoder_;
    JitterPipeline jitter_;
    Resampler resampler_;
    PcmRing ring_;

    uint32_t capture_rate_ = 0u;
    uint32_t capture_channels_ = 0u;
    uint32_t capture_frame_ms_ = 0u;
    uint32_t endpoint_rate_ = 0u;
    uint32_t endpoint_channels_ = 0u;
    uint32_t out_samples_per_frame_ = 1u;

    float linear_gain_ = 1.0f;
    miclink_declck declck_ {};

    /* 解码暂存（采集率，最大 60ms × 48kHz × 2ch = 5760）与输出暂存（端点率）。 */
    std::vector<int16_t> decode_buf_;
    std::vector<int16_t> out_buf_;

    uint64_t produced_frames_ = 0u;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_PIPELINE_H */
