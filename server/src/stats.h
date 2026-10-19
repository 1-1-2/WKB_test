/*
 * stats.h -- 运行期统计。
 *
 * 【为什么全用原子量而不用互斥锁】
 * 统计会被 UDP 接收线程（最高优先级）与注入线程（次高）同时更新。若用互斥锁，
 * 低优先级的状态打印线程一旦持锁被抢占，高优先级线程就会阻塞在锁上 —— 这正是
 * 典型的优先级反转。这里所有计数器都是 std::atomic 的 relaxed 读改写：
 * 无锁、无阻塞、不会反转；统计只用于展示，不要求跨计数器的强一致性。
 */
#ifndef WOMIC_SERVER_STATS_H
#define WOMIC_SERVER_STATS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace womic {

struct StatsSnapshot {
    /* 网络 */
    uint64_t packets_received = 0;
    uint64_t bytes_received = 0;
    uint64_t packets_invalid = 0;    /* 协议头校验失败 */
    uint64_t packets_ignored = 0;    /* 来自非当前客户端 / 未知来源 */
    uint64_t client_connected_events = 0;

    /* 解码 */
    uint64_t frames_decoded = 0;
    uint64_t frames_plc = 0;         /* 丢包隐藏补齐的帧 */
    uint64_t decode_errors = 0;

    /* 产出与缓冲 */
    uint64_t frames_produced = 0;    /* 重采样后写入环形缓冲的帧 */
    uint64_t frames_dropped_ring = 0;/* 环形缓冲满导致的丢弃 */
    uint32_t ring_available = 0;
    uint32_t ring_free = 0;

    /* 抖动缓冲 */
    uint32_t jitter_target_ms = 0;
    uint32_t jitter_depth_ms = 0;
    uint64_t jitter_missing = 0;     /* 占位帧（交给 PLC） */
    uint64_t jitter_late = 0;        /* 到得太晚 */
    uint64_t jitter_duplicate = 0;
    uint64_t jitter_evicted = 0;     /* 缓冲满被驱逐 */
    uint32_t jitter_stalls = 0;      /* 重新缓冲次数 */

    /* 注入 */
    uint64_t frames_injected = 0;
    uint64_t device_full_events = 0; /* WRITE_PCM 返回 BUFFER_FULL 的次数 */
    uint64_t frames_dropped_device = 0;

    /* 电平 */
    float level_rms = 0.0f;
    float level_peak = 0.0f;

    /* 驱动侧状态（GET_STATUS） */
    uint32_t device_state = 0;
    uint64_t device_frames_written = 0;
    uint64_t device_frames_consumed = 0;
    uint64_t device_frames_dropped = 0;
    uint32_t device_underrun = 0;
    uint32_t device_overrun = 0;
    uint32_t device_high_watermark = 0;
    uint32_t device_ring_available = 0;
    uint32_t device_ring_free = 0;
};

class Stats {
public:
    Stats() = default;
    Stats(const Stats &) = delete;
    Stats &operator=(const Stats &) = delete;

    /* ---- 网络侧（UDP 接收线程） ---- */
    void on_packet_received(uint64_t bytes)
    {
        packets_received_.fetch_add(1, std::memory_order_relaxed);
        bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void on_packet_invalid() { packets_invalid_.fetch_add(1, std::memory_order_relaxed); }
    void on_packet_ignored() { packets_ignored_.fetch_add(1, std::memory_order_relaxed); }
    void on_client_connected() { client_connected_events_.fetch_add(1, std::memory_order_relaxed); }

    /* ---- 解码 ---- */
    void on_frame_decoded() { frames_decoded_.fetch_add(1, std::memory_order_relaxed); }
    void on_frame_plc() { frames_plc_.fetch_add(1, std::memory_order_relaxed); }
    void on_decode_error() { decode_errors_.fetch_add(1, std::memory_order_relaxed); }

    /* ---- 抖动缓冲 ---- */
    void on_jitter_missing() { jitter_missing_.fetch_add(1, std::memory_order_relaxed); }
    void on_jitter_late() { jitter_late_.fetch_add(1, std::memory_order_relaxed); }
    void on_jitter_duplicate() { jitter_duplicate_.fetch_add(1, std::memory_order_relaxed); }
    void on_jitter_evicted() { jitter_evicted_.fetch_add(1, std::memory_order_relaxed); }
    void set_jitter(uint32_t target_ms, uint32_t depth_ms, uint32_t stalls)
    {
        jitter_target_ms_.store(target_ms, std::memory_order_relaxed);
        jitter_depth_ms_.store(depth_ms, std::memory_order_relaxed);
        jitter_stalls_.store(stalls, std::memory_order_relaxed);
    }

    /* ---- 环形缓冲（产出线程） ---- */
    void on_frame_produced(uint32_t frames)
    {
        frames_produced_.fetch_add(frames, std::memory_order_relaxed);
    }
    void on_ring_drop(uint32_t frames)
    {
        frames_dropped_ring_.fetch_add(frames, std::memory_order_relaxed);
    }
    void set_ring(uint32_t available, uint32_t free_frames)
    {
        ring_available_.store(available, std::memory_order_relaxed);
        ring_free_.store(free_frames, std::memory_order_relaxed);
    }

    /* ---- 注入线程 ---- */
    void on_frame_injected(uint32_t frames)
    {
        frames_injected_.fetch_add(frames, std::memory_order_relaxed);
    }
    void on_device_full() { device_full_events_.fetch_add(1, std::memory_order_relaxed); }
    void on_device_drop(uint32_t frames)
    {
        frames_dropped_device_.fetch_add(frames, std::memory_order_relaxed);
    }

    /* ---- 电平 ---- */
    void set_level(float rms, float peak)
    {
        level_rms_.store(rms, std::memory_order_relaxed);
        level_peak_.store(peak, std::memory_order_relaxed);
    }

    /* ---- 驱动状态 ---- */
    void set_device_status(uint32_t state, uint64_t written, uint64_t consumed, uint64_t dropped,
                           uint32_t underrun, uint32_t overrun, uint32_t high_watermark,
                           uint32_t ring_available, uint32_t ring_free)
    {
        device_state_.store(state, std::memory_order_relaxed);
        device_frames_written_.store(written, std::memory_order_relaxed);
        device_frames_consumed_.store(consumed, std::memory_order_relaxed);
        device_frames_dropped_.store(dropped, std::memory_order_relaxed);
        device_underrun_.store(underrun, std::memory_order_relaxed);
        device_overrun_.store(overrun, std::memory_order_relaxed);
        device_high_watermark_.store(high_watermark, std::memory_order_relaxed);
        device_ring_available_.store(ring_available, std::memory_order_relaxed);
        device_ring_free_.store(ring_free, std::memory_order_relaxed);
    }

    /** 取快照（各字段独立读取，不保证同瞬间一致，仅用于展示）。 */
    StatsSnapshot snapshot() const;

    /** 丢包率（%%，基于“应产出的总帧数”计算）。 */
    double loss_percent() const;

    /** 格式化成一行状态（供 --verbose 与每秒状态行使用）。 */
    std::string format_line(double cpu_load_percent) const;

    /** 格式化成多行汇总（退出时打印）。 */
    std::string format_summary() const;

private:
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> packets_invalid_{0};
    std::atomic<uint64_t> packets_ignored_{0};
    std::atomic<uint64_t> client_connected_events_{0};

    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_plc_{0};
    std::atomic<uint64_t> decode_errors_{0};

    std::atomic<uint64_t> frames_produced_{0};
    std::atomic<uint64_t> frames_dropped_ring_{0};
    std::atomic<uint32_t> ring_available_{0};
    std::atomic<uint32_t> ring_free_{0};

    std::atomic<uint32_t> jitter_target_ms_{0};
    std::atomic<uint32_t> jitter_depth_ms_{0};
    std::atomic<uint64_t> jitter_missing_{0};
    std::atomic<uint64_t> jitter_late_{0};
    std::atomic<uint64_t> jitter_duplicate_{0};
    std::atomic<uint64_t> jitter_evicted_{0};
    std::atomic<uint32_t> jitter_stalls_{0};

    std::atomic<uint64_t> frames_injected_{0};
    std::atomic<uint64_t> device_full_events_{0};
    std::atomic<uint64_t> frames_dropped_device_{0};

    std::atomic<float> level_rms_{0.0f};
    std::atomic<float> level_peak_{0.0f};

    std::atomic<uint32_t> device_state_{0};
    std::atomic<uint64_t> device_frames_written_{0};
    std::atomic<uint64_t> device_frames_consumed_{0};
    std::atomic<uint64_t> device_frames_dropped_{0};
    std::atomic<uint32_t> device_underrun_{0};
    std::atomic<uint32_t> device_overrun_{0};
    std::atomic<uint32_t> device_high_watermark_{0};
    std::atomic<uint32_t> device_ring_available_{0};
    std::atomic<uint32_t> device_ring_free_{0};
};

}  // namespace womic

#endif  /* WOMIC_SERVER_STATS_H */
