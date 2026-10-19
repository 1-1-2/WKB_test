/*
 * jitter_pipeline.cpp -- miclink_jitter 的服务端封装实现。
 */
#include "jitter_pipeline.h"

#include <cstdio>

#include "log.h"

namespace miclink {

static constexpr char kMicLinkLogModule[] = "jitter";

bool JitterPipeline::init(uint32_t sample_rate, uint16_t channels, uint32_t frame_ms,
                          uint32_t min_ms, uint32_t max_ms, uint32_t target_ms,
                          uint32_t capacity_frames, std::string *err)
{
    if ((sample_rate == 0u) || (channels == 0u) || (frame_ms == 0u)) {
        if (err != nullptr) {
            *err = "jitter: 采样率/声道/帧长不能为 0";
        }
        ready_ = false;
        return false;
    }

    miclink_jb_config cfg {};
    cfg.sample_rate = sample_rate;
    cfg.channels = channels;
    cfg.frame_ms = frame_ms;
    cfg.min_delay_ms = (min_ms == 0u) ? MICLINK_JB_DEFAULT_MIN : min_ms;
    cfg.max_delay_ms = (max_ms == 0u) ? MICLINK_JB_DEFAULT_MAX : max_ms;
    cfg.target_delay_ms = (target_ms == 0u) ? cfg.min_delay_ms : target_ms;
    cfg.capacity_frames = capacity_frames;

    const size_t need = miclink_jb_storage_bytes(&cfg);
    if (need == 0u) {
        if (err != nullptr) {
            *err = "jitter: storage_bytes 计算为 0（配置非法）";
        }
        ready_ = false;
        return false;
    }
    if (storage_.size() < need) {
        storage_.resize(need);
    }

    int rc = miclink_jb_init(&jb_, &cfg, storage_.data(), storage_.size());
    if (rc != 0) {
        if (err != nullptr) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "miclink_jb_init 失败(%d)，需要 %zu 字节 storage", rc, need);
            *err = buf;
        }
        ready_ = false;
        return false;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;
    frame_ms_ = frame_ms;
    samples_per_frame_ = jb_.samples_per_frame;
    ready_ = true;
    return true;
}

JitterPushResult JitterPipeline::push(uint32_t seq, const int16_t *frames, uint32_t nframes,
                                      uint32_t now_ms)
{
    if (!ready_) {
        return JitterPushResult::kInvalidArg;
    }
    if ((frames == nullptr) || (nframes != samples_per_frame_)) {
        return JitterPushResult::kInvalidArg;
    }
    int rc = miclink_jb_put(&jb_, seq, const_cast<int16_t *>(frames), nframes, now_ms);
    switch (rc) {
        case 0:
            return JitterPushResult::kAccepted;
        case 1:
            return JitterPushResult::kDuplicate;
        case 2:
            return JitterPushResult::kLate;
        default:
            /* miclink_jb_put 的负值：目前只有参数错误 / nframes 不符。 */
            return (rc == -2) ? JitterPushResult::kTooLarge : JitterPushResult::kInvalidArg;
    }
}

bool JitterPipeline::is_seq_missing(uint32_t seq) const
{
    if (!ready_) {
        return false;
    }
    /* 播放游标已经越过它：再补也没有意义（push 时会判成 late）。 */
    if (static_cast<int32_t>(seq - jb_.head_seq) < 0) {
        return false;
    }
    /* 缓冲里已经有这一帧：不是缺口。 */
    const uint32_t idx = seq % jb_.capacity;
    if ((jb_.slot_used[idx] != 0u) && (jb_.slot_seq[idx] == seq)) {
        return false;
    }
    return true;
}

JitterPullResult JitterPipeline::pull(int16_t *out_frames, uint32_t nframes, int *out_missing,
                                      uint32_t *out_seq, uint32_t now_ms)
{
    if (!ready_) {
        return JitterPullResult::kInvalidArg;
    }
    if (out_missing != nullptr) {
        *out_missing = 0;
    }

    int rc = miclink_jb_get(&jb_, out_frames, nframes, out_missing, now_ms);
    if (rc != 1) {
        if (rc == -2) {
            return JitterPullResult::kWrongFrameSize;
        }
        if (rc == -1) {
            return JitterPullResult::kInvalidArg;
        }
        return JitterPullResult::kNotReady;
    }

    /*
     * 成功产出一帧后，miclink_jitter.head_seq 已经 ++，因此"刚播出的那一帧"
     * 的 seq 就是 head_seq - 1（真实帧与占位帧都会推进 head_seq，见
     * shared/core/src/jitter_buffer.c 的 jb_emit_at_head 与占位分支）。
     * 直接读共享结构体字段（不复制定义），用于统计与测试的 seq 连续性校验。
     */
    if (out_seq != nullptr) {
        *out_seq = jb_.head_seq - 1u;
    }
    return JitterPullResult::kProduced;
}

uint32_t JitterPipeline::target_delay_ms() const
{
    if (!ready_) {
        return 0u;
    }
    return miclink_jb_target_delay_ms(&jb_);
}

uint32_t JitterPipeline::depth_ms() const
{
    if (!ready_) {
        return 0u;
    }
    return miclink_jb_depth_ms(&jb_);
}

JitterCounters JitterPipeline::counters() const
{
    JitterCounters c {};
    if (!ready_) {
        return c;
    }
    c.inserted = jb_.frames_inserted;
    c.emitted = jb_.frames_emitted;
    c.missing = jb_.frames_missing;
    c.late = jb_.frames_late;
    c.duplicate = jb_.frames_duplicate;
    c.evicted = jb_.frames_evicted;
    c.stalls = jb_.stall_count;
    return c;
}

void JitterPipeline::reset()
{
    if (!ready_) {
        return;
    }
    miclink_jb_reset(&jb_);
}

}  // namespace miclink
