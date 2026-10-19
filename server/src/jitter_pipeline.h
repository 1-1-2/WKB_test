/*
 * jitter_pipeline.h -- 对 shared/core 的 womic_jitter 的服务端封装。
 *
 * 职责：
 *   - 管理 jitter 所需的 storage（womic_jitter 本身不分配内存，storage 由调用方给）；
 *   - 把 womic_jb_put / womic_jb_get 的返回码翻译成可读的统计量
 *     （重复、迟到、驱逐、PLC 占位、重缓冲次数）；
 *   - 支持运行期按"每包实际采样率/帧长"重建配置（手机换机、切采样率时）。
 *
 * 播放时钟由调用方传入的 now_ms 驱动（单调毫秒），本模块不自己取时间。
 */
#ifndef WOMIC_SERVER_JITTER_PIPELINE_H
#define WOMIC_SERVER_JITTER_PIPELINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "womic/womic_jitter.h"

namespace womic {

struct JitterCounters {
    uint64_t inserted = 0;   /* 正常入队帧数 */
    uint64_t emitted = 0;    /* 产出帧数（含占位帧） */
    uint64_t missing = 0;    /* 占位帧数（交给 Opus PLC） */
    uint64_t late = 0;       /* 到达过晚被丢弃 */
    uint64_t duplicate = 0;  /* 重复包 */
    uint64_t evicted = 0;    /* 缓冲满被驱逐 */
    uint32_t stalls = 0;     /* 起播后重新缓冲的事件次数 */
};

/** put() 的返回语义（与 womic_jb_put 一致，另加参数错误）。 */
enum class JitterPushResult : int {
    kAccepted = 0,
    kDuplicate = 1,
    kLate = 2,
    kInvalidArg = -1,
    kTooLarge = -2,
};

/** get() 的返回语义（与 womic_jb_get 一致）。 */
enum class JitterPullResult : int {
    kProduced = 1,
    kNotReady = 0,
    kInvalidArg = -1,
    kWrongFrameSize = -2,
};

class JitterPipeline {
public:
    JitterPipeline() = default;
    ~JitterPipeline() = default;

    JitterPipeline(const JitterPipeline &) = delete;
    JitterPipeline &operator=(const JitterPipeline &) = delete;

    /**
     * @brief 初始化抖动缓冲。
     * @param sample_rate      解码侧采样率（= 手机 Opus 采样率）
     * @param channels         声道数
     * @param frame_ms         每帧时长（10 或 20）
     * @param min_ms/max_ms    目标延迟自适应区间
     * @param target_ms        初始目标延迟（0 = 用 min）
     * @param capacity_frames  槽位数（0 = 由 max_ms 推导）
     */
    bool init(uint32_t sample_rate, uint16_t channels, uint32_t frame_ms, uint32_t min_ms,
              uint32_t max_ms, uint32_t target_ms, uint32_t capacity_frames, std::string *err);

    bool ready() const { return ready_; }

    /** 当前生效的每帧样本数（单声道即样本数）。 */
    uint32_t samples_per_frame() const { return samples_per_frame_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t frame_ms() const { return frame_ms_; }

    /** 入队一帧（nframes 必须等于 samples_per_frame()）。 */
    JitterPushResult push(uint32_t seq, const int16_t *frames, uint32_t nframes, uint32_t now_ms);

    /**
     * @brief seq 对应的帧当前是否处于"缺失"状态。
     *
     * 判据：seq 仍在播放游标之后（没播出去，还有补上的意义）**且**缓冲里没有它。
     *
     * 供带内 FEC 使用（缺陷 D-01）：冗余包是"它所标注 seq 那一帧"的第二份拷贝，
     * 只有在那一帧确实丢了的时候才值得解码注入；否则就是纯冗余，应当直接拒收，
     * 绝不能让它变成额外的一帧。
     */
    bool is_seq_missing(uint32_t seq) const;

    /**
     * @brief 出队一帧。
     * @param out_missing 输出：1 表示占位帧，调用方应跑 Opus PLC
     * @param out_seq     输出：该帧对应的 seq（占位帧也是它"本应有"的 seq）
     */
    JitterPullResult pull(int16_t *out_frames, uint32_t nframes, int *out_missing, uint32_t *out_seq,
                          uint32_t now_ms);

    /** 当前自适应目标延迟（ms）。 */
    uint32_t target_delay_ms() const;

    /** 当前缓冲深度（ms）。 */
    uint32_t depth_ms() const;

    /** 累计统计。 */
    JitterCounters counters() const;

    /** 复位（保留配置，回到未起播状态）。 */
    void reset();

private:
    womic_jitter jb_ {};
    std::vector<uint8_t> storage_;
    bool ready_ = false;
    uint32_t sample_rate_ = 0u;
    uint16_t channels_ = 0u;
    uint32_t frame_ms_ = 0u;
    uint32_t samples_per_frame_ = 0u;
};

}  // namespace womic

#endif  /* WOMIC_SERVER_JITTER_PIPELINE_H */
