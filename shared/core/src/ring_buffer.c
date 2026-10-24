/*
 * ring_buffer.c -- SPSC 无锁环形缓冲实现（容量以帧为单位）。
 *
 * 见 miclink_ring.h 中的并发与内存序约定。
 *
 * 【内核侧 DPC 约束提醒】
 * miclink_ring_read / miclink_ring_read_fill_silence 会在 DISPATCH_LEVEL 且持有自旋锁的
 * DPC 中被调用，因此这两个函数内：
 *   - 不得阻塞、不得分配内存、不得获取任何锁、不得做浮点运算、不得调用分页代码；
 *   - 只允许 acquire/release 屏障与普通整数运算 + 样本拷贝；
 *   - 全部为 O(n) 的直线代码，无分支回溯，执行时间可预测。
 *
 * 无 libc 依赖：不使用 memcpy/memset/除法，取模一律用 mask 位与。
 */
#include "miclink/miclink_ring.h"

/* ------------------------------------------------------------------ *
 * 内部工具
 * ------------------------------------------------------------------ */

/** @brief 向下取整到最近的 2 的幂；输入 0 返回 0。 */
static uint32_t miclink_ring_floor_pow2(uint32_t v)
{
    uint32_t p = 1u;
    if (v == 0u) {
        return 0u;
    }
    while (p <= v / 2u) {
        p <<= 1;
    }
    return p;
}

/** @brief 判断是否为 2 的幂。 */
static int miclink_ring_is_pow2(uint32_t v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

/** @brief 拷贝 n 个 int16 样本（不依赖 libc 的 memcpy）。 */
static void miclink_ring_copy_i16(int16_t *dst, const int16_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

/** @brief 把 n 个 int16 置零（不依赖 libc 的 memset）。 */
static void miclink_ring_zero_i16(int16_t *dst, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        dst[i] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * 公开 API
 * ------------------------------------------------------------------ */

int miclink_ring_init(miclink_ring *r, int16_t *storage, uint32_t capacity_pow2, uint32_t frame_bytes)
{
    uint32_t cap;

    if (r == NULL) {
        return MICLINK_RING_ERR_NULL;
    }
    if ((frame_bytes == 0u) || ((frame_bytes % 2u) != 0u)) {
        return MICLINK_RING_ERR_BAD_FRAME;
    }
    if (!miclink_ring_is_pow2(capacity_pow2)) {
        /* 非 2 的幂：向下取整到最近的 2 的幂，保证 mask 取模始终成立。
           调用方可用 miclink_ring_capacity() 读回实际容量。 */
        uint32_t floored = miclink_ring_floor_pow2(capacity_pow2);
        if (floored == 0u) {
            return MICLINK_RING_ERR_NOT_POW2;
        }
        cap = floored;
    } else {
        cap = capacity_pow2;
    }

    r->capacity = cap;
    r->mask = cap - 1u;
    r->frame_bytes = frame_bytes;
    r->buf = storage;
    r->head = 0u;
    r->tail = 0u;
    r->written_frames = 0u;
    r->read_frames = 0u;
    r->dropped_frames = 0u;

    /* 初始化结果必须对另一端可见后才能被观察到 head/tail */
    MICLINK_FENCE_FULL();
    return MICLINK_RING_OK;
}

uint32_t miclink_ring_write(miclink_ring *r, const int16_t *frames, uint32_t n)
{
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t free_frames;
    uint32_t to_write;
    uint32_t samples_per_frame;
    uint32_t i;
    uint32_t idx;

    if ((r == NULL) || (frames == NULL) || (n == 0u)) {
        return 0u;
    }
    if (r->buf == NULL) {
        return 0u;
    }

    /* 写端独占 head，但仍需 acquire 读 tail（读端 release 写入） */
    head = MICLINK_LOAD_ACQUIRE_U32(&r->head);
    tail = MICLINK_LOAD_ACQUIRE_U32(&r->tail);

    used = head - tail; /* 无符号回绕安全 */
    if (used >= r->capacity) {
        return 0u; /* 已满：一个帧都不写，由调用方决定是否记账为丢帧 */
    }
    free_frames = r->capacity - used;
    to_write = (n > free_frames) ? free_frames : n;
    if (to_write == 0u) {
        return 0u;
    }

    samples_per_frame = r->frame_bytes / 2u;
    if (samples_per_frame == 0u) {
        return 0u;
    }

    for (i = 0u; i < to_write; ++i) {
        idx = (head + i) & r->mask;
        miclink_ring_copy_i16(&r->buf[idx * samples_per_frame],
                            &frames[i * samples_per_frame],
                            samples_per_frame);
    }

    /* 释放栅栏：样本写入必须先于 head 推进被读端看到 */
    MICLINK_FENCE_FULL();
    MICLINK_STORE_RELEASE_U32(&r->head, head + to_write);

    r->written_frames += (uint64_t)to_write;
    return to_write;
}

uint32_t miclink_ring_read(miclink_ring *r, int16_t *dst, uint32_t n)
{
    uint32_t head;
    uint32_t tail;
    uint32_t available;
    uint32_t to_read;
    uint32_t samples_per_frame;
    uint32_t i;
    uint32_t idx;

    if ((r == NULL) || (dst == NULL) || (n == 0u)) {
        return 0u;
    }
    if (r->buf == NULL) {
        return 0u;
    }

    /* 获取栅栏：先看到写端 release 的 head，再读样本 */
    head = MICLINK_LOAD_ACQUIRE_U32(&r->head);
    MICLINK_FENCE_FULL();

    tail = MICLINK_LOAD_ACQUIRE_U32(&r->tail);
    available = head - tail; /* 无符号回绕安全 */
    if (available == 0u) {
        return 0u;
    }
    to_read = (n > available) ? available : n;

    samples_per_frame = r->frame_bytes / 2u;
    if (samples_per_frame == 0u) {
        return 0u;
    }

    for (i = 0u; i < to_read; ++i) {
        idx = (tail + i) & r->mask;
        miclink_ring_copy_i16(&dst[i * samples_per_frame],
                            &r->buf[idx * samples_per_frame],
                            samples_per_frame);
    }

    /* 释放栅栏：样本必须读完之后，槽位才能通过 tail 释放给写端复用 */
    MICLINK_FENCE_FULL();
    MICLINK_STORE_RELEASE_U32(&r->tail, tail + to_read);

    r->read_frames += (uint64_t)to_read;
    return to_read;
}

uint32_t miclink_ring_read_fill_silence(miclink_ring *r, int16_t *dst, uint32_t n)
{
    uint32_t real;
    uint32_t samples_per_frame;
    uint32_t missing;

    if ((r == NULL) || (dst == NULL) || (n == 0u)) {
        return 0u;
    }

    real = miclink_ring_read(r, dst, n);
    if (real >= n) {
        return real;
    }

    /* underrun：剩余部分补静音（DPC 安全，纯整数循环） */
    samples_per_frame = r->frame_bytes / 2u;
    missing = n - real;
    miclink_ring_zero_i16(&dst[real * samples_per_frame], missing * samples_per_frame);
    return real;
}

uint32_t miclink_ring_available(miclink_ring *r)
{
    uint32_t head;
    uint32_t tail;

    if (r == NULL) {
        return 0u;
    }
    head = MICLINK_LOAD_ACQUIRE_U32(&r->head);
    MICLINK_FENCE_FULL();
    tail = MICLINK_LOAD_ACQUIRE_U32(&r->tail);
    return head - tail;
}

uint32_t miclink_ring_free(miclink_ring *r)
{
    uint32_t used;

    if (r == NULL) {
        return 0u;
    }
    used = miclink_ring_available(r);
    if (used >= r->capacity) {
        return 0u;
    }
    return r->capacity - used;
}

void miclink_ring_reset(miclink_ring *r)
{
    if (r == NULL) {
        return;
    }
    r->head = 0u;
    r->tail = 0u;
    r->written_frames = 0u;
    r->read_frames = 0u;
    r->dropped_frames = 0u;
    MICLINK_FENCE_FULL();
}

void miclink_ring_add_dropped(miclink_ring *r, uint64_t n)
{
    if (r == NULL) {
        return;
    }
    r->dropped_frames += n;
}

uint32_t miclink_ring_frame_samples(const miclink_ring *r)
{
    if ((r == NULL) || (r->frame_bytes == 0u)) {
        return 0u;
    }
    return r->frame_bytes / 2u;
}
