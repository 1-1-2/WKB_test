/*
 * miclink_dsp.h -- 平台中立的轻量 DSP（增益 / 电平测量 / 静音填充 / 去咔哒）。
 *
 * 纯 C99、不依赖 libc 与 libm（内部自带 Newton 迭代实现的 sqrt）。
 *
 * 【内核态使用限制】
 *   - miclink_fill_silence / miclink_declck_* 为纯整数实现，可在 DISPATCH_LEVEL 调用；
 *   - miclink_apply_gain / miclink_measure_level 使用 float / double，
 *     Windows 内核（尤其 x64）默认不允许在内核态使用浮点上下文，
 *     因此这两个函数只能在 PASSIVE_LEVEL 的服务端 / 用户态调用，
 *     或者调用方自行用 KeSaveFloatingPointState / KeRestoreFloatingPointState 包裹。
 */
#ifndef MICLINK_DSP_H
#define MICLINK_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 对样本施加线性增益，带饱和 clamp，防止 int16 溢出回绕（爆音）。
 *
 * @param samples    原地处理的样本数组。
 * @param nframes    样本数（单声道即帧数）。
 * @param linear_gain 线性增益（1.0f 为原样）。NaN/Inf 被视为无效，直接返回。
 */
void miclink_apply_gain(int16_t *samples, uint32_t nframes, float linear_gain);

/**
 * @brief 测量电平。out_rms / out_peak 归一化到 0..1（1.0 对应满量程 32767）。
 * @param s        输入样本。
 * @param nframes  样本数；为 0 时输出 0。
 * @param out_rms  可为 NULL。
 * @param out_peak 可为 NULL。
 */
void miclink_measure_level(const int16_t *s, uint32_t nframes, float *out_rms, float *out_peak);

/** @brief 填充静音（全 0）。 */
void miclink_fill_silence(int16_t *dst, uint32_t nframes);

/*
 * 去咔哒（declick）：当一段数据之前发生过丢失（underrun / 丢包 / 缓冲排空）时，
 * 直接从“上一帧最后样本 prev”跳到“新数据第一样本”会产生一个阶跃，听感上就是“咔哒”。
 * 这里在开头 fade_len_samples 个样本上做线性淡入，把能量平滑地接回去。
 *
 * fade_len 默认 64 样本的依据：
 *   - 48 kHz 下 64 样本 ≈ 1.33 ms。人耳对 <3 ms 的瞬变不敏感为独立的“回声”，
 *     1.3 ms 足以把阶跃能量摊开到听感阈值以下；
 *   - 同时它足够短，不会在语音起始处产生可闻的“哇”声或拖尾（>5 ms 才会有），
 *     也不会引入可感知的回声（回声需要 >10 ms 的重复结构）；
 *   - 16 kHz 下 64 样本 = 4 ms，仍在安全区间内，因此 64 是一个跨采样率通用的取值。
 */
#define MICLINK_DECLCK_DEFAULT_FADE 64u

typedef struct {
    int32_t prev_sample;      /* 断流前最后一个样本；淡入期间保持冻结，作为淡入起点基准 */
    int fade_remaining;       /* 剩余需要做淡入的样本数，<=0 表示当前无需淡入 */
    uint32_t fade_len;        /* 淡入窗口长度（样本） */
} miclink_declck;

/**
 * @brief 初始化去咔哒状态。
 *
 * 初始化后 fade_remaining = fade_len：第一段数据会从 0 淡入，
 * 这正好消除“启动瞬间从未知电平跳变”的爆音。
 */
void miclink_declck_init(miclink_declck *d, uint32_t fade_len_samples);

/**
 * @brief 复位：视为发生了一次断流（prev 置 0，并重新武装淡入）。
 */
void miclink_declck_reset(miclink_declck *d);

/**
 * @brief 通知“此前有数据丢失”，下一次 apply 会从 prev_sample 淡入。
 *
 * 调用时机：jitter buffer 报 missing、环形缓冲 underrun 补静音之后、
 * 或驱动侧 DMA 断流恢复时。
 */
void miclink_declck_notify_loss(miclink_declck *d);

/**
 * @brief 原地处理一段数据：如处于淡入窗口，则用
 *        out[i] = prev + (s[i] - prev) * (j + 1) / fade_len,  j = fade_len - fade_remaining + i
 *        做交叉淡入（j 为整个淡入窗口内的全局下标，因此淡入横跨多个数据块时
 *        曲线不会在块边界折断）。两端都保证能量连续：
 *          - 第一个样本与 prev 的落差仅为 (s[0]-prev)/fade_len（原始阶跃的 1/fade_len）；
 *          - 淡入窗口最后一个样本权值恰好为 1，无缝接回原始样本序列。
 *        淡入结束后 prev_sample 更新为本段最后一个样本；淡入期间保持冻结。
 */
void miclink_apply_declck(miclink_declck *d, int16_t *samples, uint32_t nframes);

#ifdef __cplusplus
}
#endif

#endif /* MICLINK_DSP_H */
