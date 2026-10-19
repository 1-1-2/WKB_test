/*
 * ring_adapter.h -- 用户态生产者环形缓冲。
 *
 * 作用：把"产出线程"（UDP 接收 + 解码 + 抖动缓冲 + 重采样）与"注入线程"
 * （DeviceIoControl WRITE_PCM）解耦。产出侧受网络抖动影响是突发式的，
 * 注入侧受内核端点时钟影响是匀速的，中间必须有一个 SPSC 缓冲吸收两者差异。
 *
 * 底层直接用 shared/core 的 miclink_ring（与内核驱动同一份实现、同一份语义），
 * 因此用户态与内核态的"帧"定义完全一致，且天然满足：
 *   - 单生产者 / 单消费者 -> 无锁（只有 acquire/release 屏障），
 *     注入线程永远不会因为产出线程持锁而阻塞（无优先级反转）；
 *   - 容量必须是 2 的幂，用 mask 取模，无除法。
 */
#ifndef MICLINK_SERVER_RING_ADAPTER_H
#define MICLINK_SERVER_RING_ADAPTER_H

#include <cstdint>
#include <string>
#include <vector>

#include "miclink/miclink_ring.h"

namespace miclink {

class PcmRing {
public:
    PcmRing() = default;
    ~PcmRing() = default;

    PcmRing(const PcmRing &) = delete;
    PcmRing &operator=(const PcmRing &) = delete;

    /**
     * @brief 初始化。
     * @param capacity_frames 期望帧容量（会向下取整到 2 的幂）
     * @param frame_bytes     每帧字节数（= channels * bits / 8）
     */
    bool init(uint32_t capacity_frames, uint32_t frame_bytes, std::string *err);

    bool ready() const { return ready_; }

    /** 写入最多 n 帧，返回实际写入帧数。 */
    uint32_t write(const int16_t *frames, uint32_t n);

    /** 读出最多 n 帧，返回实际读出帧数。 */
    uint32_t read(int16_t *frames, uint32_t n);

    /** 当前可读帧数。 */
    uint32_t available() const;

    /** 当前可写帧数。 */
    uint32_t free_frames() const;

    /** 容量（帧）。 */
    uint32_t capacity() const;

    /** 显式记账丢帧（由调用方在确认丢弃后调用）。 */
    void add_dropped(uint64_t n);

    /** 复位读写序号（要求两端静默时调用）。 */
    void reset();

private:
    miclink_ring ring_ {};
    std::vector<int16_t> storage_;
    bool ready_ = false;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_RING_ADAPTER_H */
