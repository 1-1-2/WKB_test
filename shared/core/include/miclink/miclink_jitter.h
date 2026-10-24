/*
 * miclink_jitter.h -- 自适应抖动缓冲（思路来自 WebRTC NetEq，实现大幅简化）。
 *
 * 职责：
 *   - 以 seq 为键做排序 / 去重 / 丢包检测（乱序与重复包在网络上很常见）；
 *   - 用 RFC3550 的包间抖动估计 J(i) = J + (|D(i-1,i)| - J)/16 平滑网络抖动；
 *   - 再对最近 64 个 |D| 取 95 分位，得到更稳健的 p95 抖动估计；
 *   - 目标延迟 target = clamp(min + k * p95, min, max)（k = 2）：
 *       上调要快（每次 put 立即生效），下调要慢（连续 N 帧无需等待且实测抖动
 *       确实下降时才 -1 帧），避免目标延迟在临界点来回震荡导致音质抖动；
 *   - 起播前必须缓冲到 target 深度；起播后按播放时钟输出，
 *     缺失帧用“占位帧”返回并通过 out_missing 告知调用方去跑 Opus PLC。
 *
 * 平台中立：纯 C99，不分配内存（storage 由调用方提供），不使用 libc/libm，
 * 可在 Windows 内核驱动（DISPATCH_LEVEL）中安全调用。
 *
 * 单位约定：本模块所有 put/get 的 nframes 参数表示“一帧的样本数”
 *   samples_per_frame = frame_ms * sample_rate / 1000 * channels
 * 每次 put 存入一帧、每次 get 取出一帧。
 */
#ifndef MICLINK_JITTER_H
#define MICLINK_JITTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MICLINK_JB_DEV_WINDOW      64u   /* 抖动样本窗口（用于 95 分位） */
#define MICLINK_JB_HARD_MAX_DELAY  120u  /* 目标延迟硬上限（毫秒） */
#define MICLINK_JB_DEFAULT_MIN     50u   /* 默认最小目标延迟 */
#define MICLINK_JB_DEFAULT_MAX     100u  /* 默认最大目标延迟 */
#define MICLINK_JB_DECREASE_STREAK 100u  /* 下调目标延迟所需的连续稳定帧数 */

typedef struct {
    uint32_t target_delay_ms;   /* 初始目标延迟（会随网络自适应变化） */
    uint32_t min_delay_ms;      /* 默认 50 */
    uint32_t max_delay_ms;      /* 默认 100，硬上限 120 */
    uint32_t frame_ms;          /* 每帧时长，10 或 20 */
    uint32_t capacity_frames;   /* 槽位数；0 表示由实现按 max_delay 推导默认值 */
    uint32_t sample_rate;
    uint16_t channels;
} miclink_jb_config;

typedef struct {
    /* ---- 配置与派生量 ---- */
    miclink_jb_config cfg;
    uint32_t samples_per_frame;  /* 每帧样本数 */
    uint32_t capacity;           /* 实际槽位数 */
    uint32_t initialized;

    /* ---- 存储（由调用方 storage 切分而来，不含动态分配） ---- */
    int16_t  *pcm;        /* capacity * samples_per_frame */
    uint32_t *slot_seq;   /* capacity：该槽位当前存放的 seq */
    uint8_t  *slot_used;  /* capacity：该槽位是否被占用 */

    /* ---- 播放游标 ---- */
    uint32_t head_seq;    /* 下一个待播出的 seq */
    uint32_t count;       /* 已占用槽位数 */

    /* ---- RFC3550 抖动估计 ---- */
    int      have_last;
    uint32_t last_seq;
    uint32_t last_arrival_ms;
    int64_t  jitter_x16;                        /* J，Q4 定点（单位 1/16 ms） */
    uint32_t dev_window[MICLINK_JB_DEV_WINDOW];   /* 最近 |D| 环形窗口 */
    uint32_t dev_count;
    uint32_t dev_pos;
    uint32_t p95_ms;          /* 最近窗口的 95 分位抖动（ms，向上取整） */

    /* ---- 自适应状态 ---- */
    int      playing;         /* 是否已起播 */
    uint32_t last_play_ms;    /* 上一次播出时刻 */
    uint32_t stable_frames;   /* 连续“无需等待”的帧数 */
    uint32_t target_delay_ms; /* 当前自适应目标延迟 */

    /* ---- 统计 ---- */
    uint64_t frames_inserted;
    uint64_t frames_emitted;
    uint64_t frames_missing;   /* PLC 占位帧数 */
    uint64_t frames_late;      /* 到得太晚被丢弃 */
    uint64_t frames_duplicate; /* 重复包 */
    uint64_t frames_evicted;   /* 缓冲满被驱逐 */
    uint32_t stall_count;      /* 起播后排空导致的重新缓冲次数（按事件计，非按毫秒计） */
    int      starving;         /* 当前是否处于“缓冲排空”态（用于把 stall 计成事件） */
    int32_t  last_sample;      /* 上一次播出的最后一个样本（供 PLC/去咔哒） */
} miclink_jitter;

/** @brief 计算给定配置所需的 storage 字节数（供调用方一次性分配）。 */
size_t miclink_jb_storage_bytes(const miclink_jb_config *cfg);

/**
 * @brief 初始化。
 * @return 0 成功；负值错误：-1 空指针，-2 frame_ms 非法，-3 sample_rate 非法，
 *         -4 channels 非法，-5 storage 不足。
 */
int miclink_jb_init(miclink_jitter *jb, const miclink_jb_config *cfg, void *storage, size_t storage_bytes);

/** @brief 复位（保留配置，清空所有缓冲与统计，目标延迟回到 min）。 */
void miclink_jb_reset(miclink_jitter *jb);

/**
 * @brief 入队一帧。
 * @return 0 已接收；1 重复包（忽略）；2 到达过晚（丢弃）；负值错误。
 */
int miclink_jb_put(miclink_jitter *jb, uint32_t seq, int16_t *frames, uint32_t nframes, uint32_t now_ms);

/**
 * @brief 出队一帧。
 *
 * @param out_missing 输出：1 表示该帧丢失，out_frames 中是占位帧（应交给
 *                    opus_decode(NULL, 0, ...) 做 PLC）；0 表示真实帧。
 * @return 1 产出一帧；0 还不到播的时候（仍在起播缓冲，或缓冲已排空）；
 *         -1 参数错误；-2 nframes 与 samples_per_frame 不符。
 */
int miclink_jb_get(miclink_jitter *jb, int16_t *out_frames, uint32_t nframes, int *out_missing, uint32_t now_ms);

/** @brief 当前自适应目标延迟（供测试与 UI 读取）。 */
uint32_t miclink_jb_target_delay_ms(const miclink_jitter *jb);

/** @brief 当前缓冲深度（毫秒，= 已占用槽位数 * frame_ms）。 */
uint32_t miclink_jb_depth_ms(const miclink_jitter *jb);

#ifdef __cplusplus
}
#endif

#endif /* MICLINK_JITTER_H */
