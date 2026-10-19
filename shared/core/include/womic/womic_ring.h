/*
 * womic_ring.h -- SPSC（单生产者 / 单消费者）无锁环形缓冲，容量以“帧”为单位。
 *
 * 三方共用：Linux 单测、Windows 用户态服务端、Windows 内核驱动。
 *
 * 设计约束（重要，改动前必读）：
 *   1) 纯 C99，不使用 C++ 特性；不引用任何 libc 符号（内核态可编译）；
 *      唯一例外是 womic_atomic.h，它在 _KERNEL_MODE 下走 WDK 分支。
 *   2) 结构体内除 buf 之外不含指针（buf 由调用方在初始化时一次性注入，
 *      不跨 IOCTL 边界传递，故不受“共享结构体禁指针”约束）；
 *      序号全部为 uint32_t 自然对齐，32/64 位与内核/用户态布局一致。
 *   3) 容量必须是 2 的幂，用 mask 做取模，避免除法（内核 DPC 中除法代价高）。
 *
 * 并发约定（内核侧尤其重要）：
 *   - 写端（生产者）独占 head，只读 tail；
 *   - 读端（消费者）独占 tail，只读 head；
 *   - 统计计数器 written_frames / dropped_frames 由写端独占写，
 *     read_frames 由读端独占写，另一端只读；因此不需要原子 RMW。
 *
 * 【内核侧关键约束】
 *   womic_ring_read / womic_ring_read_fill_silence 会在 DISPATCH_LEVEL 且
 *   持有自旋锁的 DPC（音频驱动的 DMA 通知回调）中被调用。因此这两个读函数内：
 *      禁止任何阻塞操作、禁止内存分配（ExAllocatePool 等）、
 *      禁止获取任何锁或自旋锁、禁止调用任何可能分页/降 IRQL 的 API、
 *      禁止浮点运算；只允许 acquire/release 屏障与普通整数运算。
 *   写端运行在 PASSIVE_LEVEL 的服务端工作线程，无此限制。
 *
 * 内存序：
 *   写：填充样本 -> WOMIC_FENCE_FULL() -> STORE_RELEASE(head)
 *       （保证样本对读端可见先于 head 推进可见）
 *   读：LOAD_ACQUIRE(head) -> WOMIC_FENCE_FULL() -> 读取样本 -> STORE_RELEASE(tail)
 *       （保证读到新 head 之后一定能看到对应样本；释放槽位前样本已读完）
 */
#ifndef WOMIC_RING_H
#define WOMIC_RING_H

#include <stddef.h>
#include <stdint.h>

#include "womic_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct womic_ring {
    uint32_t capacity;        /* 帧容量，必须是 2 的幂 */
    uint32_t mask;            /* capacity - 1，取模用 */
    uint32_t frame_bytes;     /* 每帧字节数 = channels * sample_size(2) */
    volatile uint32_t head;   /* 写端序号，独占（单调递增，允许回绕） */
    volatile uint32_t tail;   /* 读端序号，独占（单调递增，允许回绕） */
    int16_t *buf;             /* 样本存储，由调用方提供，帧总数 = capacity */
    /* 统计：各端只写自己那一条，另一端只读 */
    volatile uint64_t written_frames;
    volatile uint64_t read_frames;
    volatile uint64_t dropped_frames;
} womic_ring;

#define WOMIC_RING_OK            0
#define WOMIC_RING_ERR_FULL       1
#define WOMIC_RING_ERR_TOO_LARGE  2

/* 返回码（负值为错误）*/
#define WOMIC_RING_ERR_NULL       (-1)
#define WOMIC_RING_ERR_NOT_POW2   (-2)
#define WOMIC_RING_ERR_BAD_FRAME  (-3)

/**
 * @brief 初始化环形缓冲。
 *
 * @param r              待初始化的对象，不可为 NULL。
 * @param storage        调用方提供的样本存储，至少 capacity_pow2 * frame_bytes/2 个
 *                       int16_t。可为 NULL（此时所有读写返回 0，便于“未挂载存储”的
 *                       降级路径，内核初始化早期会用到）。
 * @param capacity_pow2  期望帧容量；非 2 的幂时向下取整到最近的 2 的幂（最小 1）。
 * @param frame_bytes    每帧字节数，必须 > 0 且为偶数。
 * @return WOMIC_RING_OK(0) 成功；负值见 WOMIC_RING_ERR_*。
 */
int womic_ring_init(womic_ring *r, int16_t *storage, uint32_t capacity_pow2, uint32_t frame_bytes);

/**
 * @brief 写入最多 n 帧，返回实际写入帧数（不会丢弃调用方数据，写不下就少写）。
 *
 * 调用方可据此自行统计丢帧：dropped = requested - written。
 * 本函数不自动累加 dropped_frames，因为“重试后再写入”不应算作丢帧；
 * 请配合 womic_ring_add_dropped() 显式记账。
 */
uint32_t womic_ring_write(womic_ring *r, const int16_t *frames, uint32_t n);

/** @brief 读出最多 n 帧，返回实际读出帧数（0 表示缓冲为空）。DPC 安全。 */
uint32_t womic_ring_read(womic_ring *r, int16_t *dst, uint32_t n);

/** @brief 读 n 帧，不足部分补零（静音），返回其中“有效（真实）”帧数。DPC 安全。 */
uint32_t womic_ring_read_fill_silence(womic_ring *r, int16_t *dst, uint32_t n);

/** @brief 当前可读帧数。 */
uint32_t womic_ring_available(womic_ring *r);

/** @brief 当前可写帧数。 */
uint32_t womic_ring_free(womic_ring *r);

/** @brief 复位读写序号与统计（要求两端静默时调用）。 */
void womic_ring_reset(womic_ring *r);

/** @brief 写端显式累加丢帧计数。 */
void womic_ring_add_dropped(womic_ring *r, uint64_t n);

/** @brief 每帧样本数（= frame_bytes / 2）。 */
uint32_t womic_ring_frame_samples(const womic_ring *r);

#ifdef __cplusplus
}
#endif

#endif /* WOMIC_RING_H */
