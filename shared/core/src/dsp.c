/*
 * dsp.c -- 平台中立轻量 DSP。
 *
 * 无 libc / libm 依赖：sqrt 用 Newton 迭代自行实现，因此 Windows 内核驱动
 * （DISPATCH_LEVEL、不可调用分页代码、不可用 SSE 浮点上下文的场景）也可直接链接。
 */
#include <stddef.h>

#include "miclink/miclink_dsp.h"

/* ------------------------------------------------------------------ *
 * 内部：Newton 迭代求平方根（不使用 libm）
 * ------------------------------------------------------------------ */
static double miclink_sqrt_f64(double x)
{
    double g;
    double prev;
    int i;

    if (x <= 0.0) {
        return 0.0;
    }
    g = (x < 1.0) ? 1.0 : x;
    for (i = 0; i < 64; ++i) {
        prev = g;
        g = 0.5 * (g + x / g);
        /* 相对变化量足够小即收敛 */
        {
            double diff = g - prev;
            if (diff < 0.0) {
                diff = -diff;
            }
            if (diff < g * 1e-12) {
                break;
            }
        }
    }
    return g;
}

/** @brief 判断 float 是否为有限值（非 NaN、非 Inf），不依赖 math.h。 */
static int miclink_f32_is_finite(float v)
{
    return (v == v) && (v <= 3.402823466e+38f) && (v >= -3.402823466e+38f);
}

/* ------------------------------------------------------------------ *
 * 增益
 * ------------------------------------------------------------------ */

void miclink_apply_gain(int16_t *samples, uint32_t nframes, float linear_gain)
{
    uint32_t i;

    if ((samples == NULL) || (nframes == 0u)) {
        return;
    }
    if (!miclink_f32_is_finite(linear_gain)) {
        return; /* NaN/Inf：拒绝处理，绝不把未定义值写回样本 */
    }

    for (i = 0u; i < nframes; ++i) {
        float fv = (float)samples[i] * linear_gain;
        int32_t v;
        if (fv > 32767.0f) {
            fv = 32767.0f;
        } else if (fv < -32768.0f) {
            fv = -32768.0f;
        }
        /* 四舍五入（不用 lrintf，避免 libm 依赖与浮点环境依赖） */
        v = (int32_t)((fv >= 0.0f) ? (fv + 0.5f) : (fv - 0.5f));
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        samples[i] = (int16_t)v;
    }
}

/* ------------------------------------------------------------------ *
 * 电平测量
 * ------------------------------------------------------------------ */

void miclink_measure_level(const int16_t *s, uint32_t nframes, float *out_rms, float *out_peak)
{
    uint64_t sum_sq = 0u;
    uint32_t peak = 0u;
    uint32_t i;
    double mean_sq;
    double rms;

    if ((s == NULL) || (nframes == 0u)) {
        if (out_rms != NULL) {
            *out_rms = 0.0f;
        }
        if (out_peak != NULL) {
            *out_peak = 0.0f;
        }
        return;
    }

    for (i = 0u; i < nframes; ++i) {
        int32_t v = (int32_t)s[i];
        uint32_t av = (uint32_t)((v < 0) ? -v : v);
        sum_sq += (uint64_t)((uint64_t)av * (uint64_t)av);
        if (av > peak) {
            peak = av;
        }
    }

    mean_sq = (double)sum_sq / (double)nframes;
    rms = miclink_sqrt_f64(mean_sq) / 32767.0;

    if (out_rms != NULL) {
        float v = (float)rms;
        if (v > 1.0f) {
            v = 1.0f;
        }
        *out_rms = (v < 0.0f) ? 0.0f : v;
    }
    if (out_peak != NULL) {
        float v = (float)((double)peak / 32767.0);
        if (v > 1.0f) {
            v = 1.0f;
        }
        *out_peak = (v < 0.0f) ? 0.0f : v;
    }
}

/* ------------------------------------------------------------------ *
 * 静音填充
 * ------------------------------------------------------------------ */

void miclink_fill_silence(int16_t *dst, uint32_t nframes)
{
    uint32_t i;

    if (dst == NULL) {
        return;
    }
    for (i = 0u; i < nframes; ++i) {
        dst[i] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * 去咔哒（declick）
 * ------------------------------------------------------------------ */

void miclink_declck_init(miclink_declck *d, uint32_t fade_len_samples)
{
    if (d == NULL) {
        return;
    }
    d->fade_len = (fade_len_samples == 0u) ? MICLINK_DECLCK_DEFAULT_FADE : fade_len_samples;
    if (d->fade_len > 4096u) {
        d->fade_len = 4096u; /* 上限保护：防止异常配置导致整段被淡入抹平 */
    }
    d->prev_sample = 0;
    /* 首段从 0 淡入，消除启动爆音 */
    d->fade_remaining = (int)d->fade_len;
}

void miclink_declck_reset(miclink_declck *d)
{
    if (d == NULL) {
        return;
    }
    /* 视为发生了一次断流：从静音重新开始淡入 */
    d->prev_sample = 0;
    d->fade_remaining = (int)((d->fade_len == 0u) ? MICLINK_DECLCK_DEFAULT_FADE : d->fade_len);
}

void miclink_declck_notify_loss(miclink_declck *d)
{
    if (d == NULL) {
        return;
    }
    /* 保留 prev_sample（它是断流前最后一个真实样本），仅重新武装淡入 */
    d->fade_remaining = (int)((d->fade_len == 0u) ? MICLINK_DECLCK_DEFAULT_FADE : d->fade_len);
}

void miclink_apply_declck(miclink_declck *d, int16_t *samples, uint32_t nframes)
{
    uint32_t i;
    uint32_t fade_n;

    if ((d == NULL) || (samples == NULL) || (nframes == 0u)) {
        return;
    }
    if (d->fade_len == 0u) {
        d->fade_len = MICLINK_DECLCK_DEFAULT_FADE;
    }
    if (d->fade_remaining <= 0) {
        /* 无需淡入：只更新 prev_sample */
        d->prev_sample = (int32_t)samples[nframes - 1u];
        return;
    }

    fade_n = (uint32_t)d->fade_remaining;
    if (fade_n > nframes) {
        fade_n = nframes;
    }

    /*
     * 交叉淡入：out[i] = prev + (s[i] - prev) * (j + 1) / fade_len
     *
     * 其中 j 是“整个淡入窗口”内的全局下标（j0 + i，j0 = fade_len - fade_remaining），
     * 而不是块内下标。这一点至关重要：淡入窗口可能横跨多个数据块，
     * 若用块内下标，每跨一次块就会把权值重新拉回小值，从而在块边界处
     * 制造一个新的能量跳变（反而引入二次咔哒）。
     *
     * 权值从 1/fade_len 线性升到 1，因此：
     *   - 第一个样本相对 prev 的落差只有原始阶跃的 1/fade_len（消除咔哒）；
     *   - 最后一个样本的权值恰好为 1，等于原始样本，与后续数据无缝衔接。
     * 用 int64 中间量避免 int32 溢出。
     */
    {
        uint32_t j0 = d->fade_len - (uint32_t)d->fade_remaining;
        for (i = 0u; i < fade_n; ++i) {
            int32_t s = (int32_t)samples[i];
            int64_t delta = (int64_t)s - (int64_t)d->prev_sample;
            int64_t scaled = delta * (int64_t)(j0 + i + 1u);
            int64_t out = (int64_t)d->prev_sample + scaled / (int64_t)d->fade_len;
            if (out > 32767) {
                out = 32767;
            } else if (out < -32768) {
                out = -32768;
            }
            samples[i] = (int16_t)out;
        }
    }

    d->fade_remaining = (int)((uint32_t)d->fade_remaining - fade_n);
    if (d->fade_remaining < 0) {
        d->fade_remaining = 0;
    }
    /*
     * 只有淡入彻底结束后才更新 prev_sample：淡入期间 prev 必须保持“冻结”，
     * 作为整段淡入的起点基准（否则淡入曲线会在块边界处折断）。
     */
    if (d->fade_remaining == 0) {
        d->prev_sample = (int32_t)samples[nframes - 1u];
    }
}
