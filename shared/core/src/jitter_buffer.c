/*
 * jitter_buffer.c -- 自适应抖动缓冲（WebRTC NetEq 思路的简化实现）。
 *
 * ---------------------------------------------------------------------------
 * 算法说明
 * ---------------------------------------------------------------------------
 * 1) 网络抖动估计
 *    RFC3550 包间抖动（以 Q4 定点保存，避免浮点）：
 *        D(i-1,i) = (R_i - R_{i-1}) - (S_i - S_{i-1})
 *        J_i      = J_{i-1} + (|D(i-1,i)| - J_{i-1}) / 16
 *    其中 R 为到达时刻，S 为媒体时间戳（这里用 seq * frame_ms）。
 *    J 是一阶低通，对单次的突发抖动不敏感，但能稳定跟踪持续的抖动。
 *
 * 2) 目标延迟（95 分位 + 线性映射）
 *    单纯的 J 对“偶发大抖动 + 长期平稳”的网络反应过慢，所以额外维护最近
 *    WOMIC_JB_DEV_WINDOW(64) 个 |D| 的滑动窗口，取 95 分位 p95（对 64 个样本做
 *    插入排序，无内存分配、执行时间可预测，可在 DISPATCH_LEVEL 调用）：
 *        target = clamp(min_delay + k * p95, min_delay, max_delay),  k = 2
 *    k=2 的含义：目标延迟要能覆盖约 2 倍于“典型抖动上限”的偏差，
 *    使得绝大多数（>95%）迟到的包仍在 deadline 之前到达。
 *
 * 3) 衰老 / 回退（hysteresis）
 *    上调要快：每次 put 立即把 target 提升到位，网络一变差就马上加深缓冲；
 *    下调要慢：只有在“连续 WOMIC_JB_DECREASE_STREAK(100) 帧都无需因欠缓冲而等待”
 *    且“实测 p95 确实已经下降到支持更低的 target”时，才把 target 下调 1 帧。
 *    这样避免 target 在临界点附近来回震荡（震荡会同时带来延迟抖动与音质抖动）。
 *
 * 4) 起播与输出
 *    起播前必须缓冲到 >= target 深度；起播后按播放时钟（每 frame_ms 播一帧）输出。
 *    缺失帧：若后面还有数据（说明这一帧确实丢了/迟到了），返回占位帧并置
 *    out_missing=1，由调用方用 opus_decode(NULL, 0, ...) 做 PLC；
 *    若整个缓冲已排空，则回到“重新缓冲”态（返回 0），并计入 stall_count。
 *
 * 平台中立：纯 C99，无 libc/libm/动态内存，slot 表与 PCM 全部来自调用方 storage。
 */
#include "womic/womic_jitter.h"

/* ------------------------------------------------------------------ *
 * 内部工具
 * ------------------------------------------------------------------ */

/** @brief 计算配置对应的每帧样本数（0 表示参数非法）。 */
static uint32_t jb_samples_per_frame(const womic_jb_config *cfg)
{
    uint32_t per_ms;

    if (cfg == NULL) {
        return 0u;
    }
    if ((cfg->frame_ms != 5u) && (cfg->frame_ms != 10u) &&
        (cfg->frame_ms != 20u) && (cfg->frame_ms != 40u)) {
        return 0u;
    }
    if ((cfg->sample_rate != 16000u) && (cfg->sample_rate != 44100u) &&
        (cfg->sample_rate != 48000u)) {
        return 0u;
    }
    if ((cfg->channels != 1u) && (cfg->channels != 2u)) {
        return 0u;
    }
    if ((cfg->sample_rate * cfg->frame_ms) % 1000u != 0u) {
        return 0u; /* 44100 Hz 下 10ms 帧不是整数样本，直接拒绝 */
    }
    per_ms = (cfg->sample_rate * cfg->frame_ms) / 1000u;
    return per_ms * (uint32_t)cfg->channels;
}

/** @brief 归一化配置：填默认值、夹紧上下限，并返回应使用的槽位容量。 */
static uint32_t jb_capacity_for(const womic_jb_config *cfg)
{
    uint32_t max_delay;
    uint32_t min_frames;
    uint32_t cap;

    max_delay = cfg->max_delay_ms;
    if (max_delay == 0u) {
        max_delay = WOMIC_JB_DEFAULT_MAX;
    }
    if (max_delay > WOMIC_JB_HARD_MAX_DELAY) {
        max_delay = WOMIC_JB_HARD_MAX_DELAY; /* 硬上限 120ms */
    }
    min_frames = max_delay / cfg->frame_ms + 2u;
    cap = cfg->capacity_frames;
    if (cap < min_frames) {
        cap = min_frames; /* 至少要能装下最大目标延迟 */
    }
    if (cap > 4096u) {
        cap = 4096u;
    }
    return cap;
}

/** @brief 对最近的 |D| 窗口取 95 分位（插入排序，无分配）。 */
static uint32_t jb_percentile95(const womic_jitter *jb)
{
    uint32_t tmp[WOMIC_JB_DEV_WINDOW];
    uint32_t n = jb->dev_count;
    uint32_t i;
    uint32_t j;
    uint32_t idx;

    if (n == 0u) {
        return 0u;
    }
    for (i = 0u; i < n; ++i) {
        tmp[i] = jb->dev_window[i];
    }
    for (i = 1u; i < n; ++i) {
        uint32_t key = tmp[i];
        j = i;
        while ((j > 0u) && (tmp[j - 1u] > key)) {
            tmp[j] = tmp[j - 1u];
            --j;
        }
        tmp[j] = key;
    }
    /* 向上取整取第 95 分位 */
    idx = (95u * (n - 1u) + 99u) / 100u;
    if (idx >= n) {
        idx = n - 1u;
    }
    return tmp[idx];
}

/** @brief 拷贝一帧样本（不依赖 libc memcpy）。 */
static void jb_copy_frame(int16_t *dst, const int16_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

static void jb_zero_frame(int16_t *dst, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        dst[i] = 0;
    }
}

/** @brief RFC3550 抖动更新 + 窗口维护。 */
static void jb_update_jitter(womic_jitter *jb, uint32_t seq, uint32_t now_ms)
{
    if (jb->have_last && (seq > jb->last_seq)) {
        int32_t d_ts = (int32_t)((seq - jb->last_seq) * jb->cfg.frame_ms);
        int32_t d_arr = (int32_t)(now_ms - jb->last_arrival_ms);
        int32_t d = d_arr - d_ts;
        int64_t j16;
        int64_t diff;
        uint32_t abs_d;

        if (d < 0) {
            d = -d;
        }
        if (d > 10000) {
            d = 10000; /* 限幅：防止时钟跳变/长时间断流污染估计 */
        }
        abs_d = (uint32_t)d;

        /* J += (|D| - J) / 16，J 用 Q4 定点（单位 1/16 ms） */
        j16 = jb->jitter_x16;
        diff = ((int64_t)abs_d << 4) - j16;
        if (diff >= 0) {
            j16 += (diff + 8) / 16;
        } else {
            j16 -= ((-diff) + 8) / 16;
        }
        if (j16 < 0) {
            j16 = 0;
        }
        jb->jitter_x16 = j16;

        jb->dev_window[jb->dev_pos] = abs_d;
        jb->dev_pos = (jb->dev_pos + 1u) % (uint32_t)WOMIC_JB_DEV_WINDOW;
        if (jb->dev_count < (uint32_t)WOMIC_JB_DEV_WINDOW) {
            jb->dev_count++;
        }
    }
    jb->last_seq = seq;
    jb->last_arrival_ms = now_ms;
    jb->have_last = 1;
}

/** @brief 根据当前 p95 重算目标延迟（只做“快上调”，下调另走慢路径）。 */
static void jb_update_target_fast(womic_jitter *jb)
{
    uint32_t desired;
    uint32_t max_delay = jb->cfg.max_delay_ms;

    if (max_delay == 0u) {
        max_delay = WOMIC_JB_DEFAULT_MAX;
    }
    if (max_delay > WOMIC_JB_HARD_MAX_DELAY) {
        max_delay = WOMIC_JB_HARD_MAX_DELAY;
    }

    jb->p95_ms = jb_percentile95(jb);
    desired = jb->cfg.min_delay_ms + 2u * jb->p95_ms; /* k = 2 */
    if (desired < jb->cfg.min_delay_ms) {
        desired = jb->cfg.min_delay_ms;
    }
    if (desired > max_delay) {
        desired = max_delay;
    }
    if (desired > jb->target_delay_ms) {
        jb->target_delay_ms = desired; /* 上调要快 */
    }
}

/** @brief 慢速下调：连续稳定且实测抖动支持时，每次只降 1 帧。 */
static void jb_maybe_lower_target(womic_jitter *jb)
{
    uint32_t desired;
    uint32_t max_delay = jb->cfg.max_delay_ms;

    if (max_delay == 0u) {
        max_delay = WOMIC_JB_DEFAULT_MAX;
    }
    if (max_delay > WOMIC_JB_HARD_MAX_DELAY) {
        max_delay = WOMIC_JB_HARD_MAX_DELAY;
    }

    if (jb->stable_frames < (uint32_t)WOMIC_JB_DECREASE_STREAK) {
        return;
    }
    if (jb->target_delay_ms <= jb->cfg.min_delay_ms) {
        jb->stable_frames = 0u;
        return;
    }

    desired = jb->cfg.min_delay_ms + 2u * jb->p95_ms;
    if (desired > max_delay) {
        desired = max_delay;
    }
    if (desired + jb->cfg.frame_ms <= jb->target_delay_ms) {
        jb->target_delay_ms -= jb->cfg.frame_ms; /* 下调要慢：一次 1 帧 */
        if (jb->target_delay_ms < jb->cfg.min_delay_ms) {
            jb->target_delay_ms = jb->cfg.min_delay_ms;
        }
    }
    jb->stable_frames = 0u;
}

/** @brief 找到当前缓冲中最小的 seq。 */
static int jb_find_min_seq(const womic_jitter *jb, uint32_t *out_seq)
{
    uint32_t i;
    int found = 0;
    uint32_t min_seq = 0u;

    for (i = 0u; i < jb->capacity; ++i) {
        if (jb->slot_used[i] != 0u) {
            if (!found || (jb->slot_seq[i] < min_seq)) {
                min_seq = jb->slot_seq[i];
                found = 1;
            }
        }
    }
    if (found && (out_seq != NULL)) {
        *out_seq = min_seq;
    }
    return found;
}

/** @brief 清空所有槽位。 */
static void jb_clear_slots(womic_jitter *jb)
{
    uint32_t i;
    for (i = 0u; i < jb->capacity; ++i) {
        jb->slot_used[i] = 0u;
        jb->slot_seq[i] = 0u;
    }
    jb->count = 0u;
}

/** @brief 若出现超过 max_delay 的巨大断裂，直接跳到最早可用帧（避免无界追赶）。 */
static void jb_resync_huge_gap(womic_jitter *jb)
{
    uint32_t min_seq = 0u;
    uint32_t tolerance;

    if (jb->count == 0u) {
        return;
    }
    if (!jb_find_min_seq(jb, &min_seq)) {
        return;
    }
    tolerance = jb->cfg.max_delay_ms / jb->cfg.frame_ms;
    if (tolerance == 0u) {
        tolerance = 1u;
    }
    if (min_seq > jb->head_seq + tolerance) {
        jb->frames_missing += (uint64_t)(min_seq - jb->head_seq);
        jb->head_seq = min_seq;
        jb->stable_frames = 0u;
    }
}

/** @brief 若 head_seq 处有数据则取出一帧。返回 1 表示已取出。 */
static int jb_emit_at_head(womic_jitter *jb, int16_t *out_frames, uint32_t nframes)
{
    uint32_t idx = jb->head_seq % jb->capacity;

    if ((jb->slot_used[idx] == 0u) || (jb->slot_seq[idx] != jb->head_seq)) {
        return 0;
    }
    jb_copy_frame(out_frames, &jb->pcm[idx * jb->samples_per_frame], nframes);
    jb->slot_used[idx] = 0u;
    jb->count--;
    jb->head_seq++;
    jb->frames_emitted++;
    jb->last_sample = (int32_t)out_frames[nframes - 1u];
    jb->starving = 0;
    return 1;
}

/** @brief 播放时钟推进（带漂移校正：长时间停顿后直接对齐当前时刻）。 */
static void jb_advance_clock(womic_jitter *jb, uint32_t now_ms)
{
    if ((now_ms - jb->last_play_ms) > (2u * jb->cfg.frame_ms)) {
        jb->last_play_ms = now_ms;
    } else {
        jb->last_play_ms += jb->cfg.frame_ms;
    }
}

/* ------------------------------------------------------------------ *
 * 公开 API
 * ------------------------------------------------------------------ */

size_t womic_jb_storage_bytes(const womic_jb_config *cfg)
{
    uint32_t spf;
    uint32_t cap;
    size_t pcm_bytes;
    size_t seq_off;
    size_t total;

    if (cfg == NULL) {
        return 0u;
    }
    spf = jb_samples_per_frame(cfg);
    if (spf == 0u) {
        return 0u;
    }
    cap = jb_capacity_for(cfg);

    pcm_bytes = (size_t)cap * (size_t)spf * sizeof(int16_t);
    seq_off = (pcm_bytes + 3u) & ~(size_t)3u; /* 4 字节对齐后放 uint32_t slot_seq */
    total = seq_off + (size_t)cap * sizeof(uint32_t) + (size_t)cap * sizeof(uint8_t);
    return total;
}

int womic_jb_init(womic_jitter *jb, const womic_jb_config *cfg, void *storage, size_t storage_bytes)
{
    uint32_t spf;
    uint32_t cap;
    size_t pcm_bytes;
    size_t seq_off;
    size_t need_bytes;
    size_t i;
    uint8_t *base;

    if ((jb == NULL) || (cfg == NULL)) {
        return -1;
    }
    spf = jb_samples_per_frame(cfg);
    if (spf == 0u) {
        /* 细分错误码 */
        if ((cfg->frame_ms != 5u) && (cfg->frame_ms != 10u) &&
            (cfg->frame_ms != 20u) && (cfg->frame_ms != 40u)) {
            return -2;
        }
        if ((cfg->sample_rate != 16000u) && (cfg->sample_rate != 44100u) &&
            (cfg->sample_rate != 48000u)) {
            return -3;
        }
        if ((cfg->channels != 1u) && (cfg->channels != 2u)) {
            return -4;
        }
        return -2;
    }

    cap = jb_capacity_for(cfg);
    pcm_bytes = (size_t)cap * (size_t)spf * sizeof(int16_t);
    seq_off = (pcm_bytes + 3u) & ~(size_t)3u;
    need_bytes = seq_off + (size_t)cap * sizeof(uint32_t) + (size_t)cap * sizeof(uint8_t);

    if ((storage == NULL) || (storage_bytes < need_bytes)) {
        return -5;
    }

    /* 结构体清零，保证所有状态有确定初值 */
    for (i = 0u; i < sizeof(*jb); ++i) {
        ((uint8_t *)jb)[i] = 0u;
    }

    base = (uint8_t *)storage;
    jb->pcm = (int16_t *)(void *)base;                          /* 2 字节对齐天然满足 */
    jb->slot_seq = (uint32_t *)(void *)(base + seq_off);        /* 4 字节对齐已处理 */
    jb->slot_used = (uint8_t *)(void *)(base + seq_off + (size_t)cap * sizeof(uint32_t));

    jb->cfg = *cfg;
    if (jb->cfg.min_delay_ms == 0u) {
        jb->cfg.min_delay_ms = WOMIC_JB_DEFAULT_MIN;
    }
    if (jb->cfg.max_delay_ms == 0u) {
        jb->cfg.max_delay_ms = WOMIC_JB_DEFAULT_MAX;
    }
    if (jb->cfg.max_delay_ms > WOMIC_JB_HARD_MAX_DELAY) {
        jb->cfg.max_delay_ms = WOMIC_JB_HARD_MAX_DELAY;
    }
    if (jb->cfg.max_delay_ms < jb->cfg.min_delay_ms) {
        jb->cfg.max_delay_ms = jb->cfg.min_delay_ms;
    }
    if (jb->cfg.target_delay_ms == 0u) {
        jb->cfg.target_delay_ms = jb->cfg.min_delay_ms;
    }

    jb->samples_per_frame = spf;
    jb->capacity = cap;
    jb->head_seq = 0u;
    jb->count = 0u;
    jb->have_last = 0;
    jb->jitter_x16 = 0;
    jb->p95_ms = 0u;
    jb->playing = 0;
    jb->target_delay_ms = jb->cfg.target_delay_ms;
    if (jb->target_delay_ms < jb->cfg.min_delay_ms) {
        jb->target_delay_ms = jb->cfg.min_delay_ms;
    }
    if (jb->target_delay_ms > jb->cfg.max_delay_ms) {
        jb->target_delay_ms = jb->cfg.max_delay_ms;
    }
    jb_clear_slots(jb);
    jb->initialized = 1u;
    return 0;
}

void womic_jb_reset(womic_jitter *jb)
{
    if ((jb == NULL) || (!jb->initialized)) {
        return;
    }
    jb_clear_slots(jb);
    jb->head_seq = 0u;
    jb->have_last = 0;
    jb->jitter_x16 = 0;
    jb->dev_count = 0u;
    jb->dev_pos = 0u;
    jb->p95_ms = 0u;
    jb->playing = 0;
    jb->last_play_ms = 0u;
    jb->stable_frames = 0u;
    jb->target_delay_ms = jb->cfg.min_delay_ms;
    jb->frames_inserted = 0u;
    jb->frames_emitted = 0u;
    jb->frames_missing = 0u;
    jb->frames_late = 0u;
    jb->frames_duplicate = 0u;
    jb->frames_evicted = 0u;
    jb->stall_count = 0u;
    jb->starving = 0;
    jb->last_sample = 0;
}

int womic_jb_put(womic_jitter *jb, uint32_t seq, int16_t *frames, uint32_t nframes, uint32_t now_ms)
{
    uint32_t idx;

    if ((jb == NULL) || (!jb->initialized)) {
        return -1;
    }
    if (frames == NULL) {
        return -1;
    }
    if (nframes != jb->samples_per_frame) {
        return -1;
    }

    /* 到得太晚（播放游标已经越过它），直接丢弃 */
    if ((int32_t)(seq - jb->head_seq) < 0) {
        jb->frames_late++;
        return 2;
    }

    idx = seq % jb->capacity;
    if ((jb->slot_used[idx] != 0u) && (jb->slot_seq[idx] == seq)) {
        jb->frames_duplicate++;
        return 1; /* 重复包：忽略 */
    }

    /* 巨大 seq 跳跃（断流后重连 / 发送端重启）：整体重同步 */
    if ((seq - jb->head_seq) >= jb->capacity) {
        jb->frames_evicted += (uint64_t)jb->count;
        jb_clear_slots(jb);
        jb->head_seq = seq;
        jb->have_last = 0;    /* 断裂点不参与抖动估计 */
        jb->stable_frames = 0u;
        idx = seq % jb->capacity;
    }

    /* 缓冲满：驱逐最旧的帧以腾出槽位（新帧总是比旧帧更有价值） */
    while (jb->count >= jb->capacity) {
        uint32_t hidx = jb->head_seq % jb->capacity;
        if ((jb->slot_used[hidx] != 0u) && (jb->slot_seq[hidx] == jb->head_seq)) {
            jb->slot_used[hidx] = 0u;
            jb->count--;
        }
        jb->frames_evicted++;
        jb->head_seq++;
    }
    idx = seq % jb->capacity;

    /* 抖动估计先做：即使随后被驱逐，到达时刻信息也是有效的网络观测 */
    jb_update_jitter(jb, seq, now_ms);
    jb_update_target_fast(jb);

    jb_copy_frame(&jb->pcm[idx * jb->samples_per_frame], frames, jb->samples_per_frame);
    jb->slot_used[idx] = 1u;
    jb->slot_seq[idx] = seq;
    jb->count++;
    jb->frames_inserted++;
    return 0;
}

int womic_jb_get(womic_jitter *jb, int16_t *out_frames, uint32_t nframes, int *out_missing, uint32_t now_ms)
{
    uint32_t depth_ms;

    if ((jb == NULL) || (!jb->initialized) || (out_frames == NULL)) {
        return -1;
    }
    if (nframes != jb->samples_per_frame) {
        return -2;
    }
    if (out_missing != NULL) {
        *out_missing = 0;
    }

    depth_ms = jb->count * jb->cfg.frame_ms;

    /* 起播缓冲：缓冲深度必须达到目标延迟才开播 */
    if (!jb->playing) {
        if (depth_ms >= jb->target_delay_ms) {
            jb->playing = 1;
            jb->last_play_ms = now_ms;
        } else {
            return 0; /* 还不到播的时候 */
        }
    }

    /* 播放时钟：每 frame_ms 只允许播出一帧 */
    if ((uint32_t)(now_ms - jb->last_play_ms) < jb->cfg.frame_ms) {
        return 0;
    }

    /* 处理“长时间断裂”导致的无界追赶 */
    jb_resync_huge_gap(jb);

    if (jb_emit_at_head(jb, out_frames, nframes)) {
        jb_advance_clock(jb, now_ms);
        jb->stable_frames++;
        jb_maybe_lower_target(jb);
        if (out_missing != NULL) {
            *out_missing = 0;
        }
        return 1;
    }

    /* head_seq 处没有数据 */
    if (jb->count == 0u) {
        /*
         * 整个缓冲排空：进入“重新缓冲”态。
         * stall_count 统计的是“事件次数”而不是“挨饿的毫秒数”，
         * 因此只在刚进入挨饿状态时 +1（starving 由下一次成功产出清除）。
         */
        if (!jb->starving) {
            jb->starving = 1;
            jb->stall_count++;
        }
        jb->stable_frames = 0u;
        return 0;
    }

    /* 后面还有数据 -> 这一帧确实丢了/迟到太久：产出占位帧，交给上层做 PLC */
    jb_zero_frame(out_frames, nframes);
    jb->frames_missing++;
    jb->starving = 0;
    jb->head_seq++;
    jb->stable_frames = 0u;
    jb_advance_clock(jb, now_ms);
    if (out_missing != NULL) {
        *out_missing = 1;
    }
    return 1;
}

uint32_t womic_jb_target_delay_ms(const womic_jitter *jb)
{
    if ((jb == NULL) || (!jb->initialized)) {
        return 0u;
    }
    return jb->target_delay_ms;
}

uint32_t womic_jb_depth_ms(const womic_jitter *jb)
{
    if ((jb == NULL) || (!jb->initialized)) {
        return 0u;
    }
    return jb->count * jb->cfg.frame_ms;
}
