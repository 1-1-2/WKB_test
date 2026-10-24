/*
 * stats.cpp -- 统计实现。所有计数都是 relaxed 原子操作（见 stats.h 注释）。
 */
#include "stats.h"

#include <cstdio>

#include "miclink/miclink_ioctl.h"

namespace miclink {
namespace {

const char *device_state_name(uint32_t state)
{
    switch (state) {
        case MICLINK_STATE_STOPPED:
            return "STOPPED";
        case MICLINK_STATE_STARTED:
            return "STARTED";
        case MICLINK_STATE_RUNNING:
            return "RUNNING";
        case MICLINK_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

}  // namespace

StatsSnapshot Stats::snapshot() const
{
    StatsSnapshot s;
    s.packets_received = packets_received_.load(std::memory_order_relaxed);
    s.bytes_received = bytes_received_.load(std::memory_order_relaxed);
    s.packets_invalid = packets_invalid_.load(std::memory_order_relaxed);
    s.packets_ignored = packets_ignored_.load(std::memory_order_relaxed);
    s.client_connected_events = client_connected_events_.load(std::memory_order_relaxed);

    s.frames_decoded = frames_decoded_.load(std::memory_order_relaxed);
    s.frames_plc = frames_plc_.load(std::memory_order_relaxed);
    s.decode_errors = decode_errors_.load(std::memory_order_relaxed);

    s.frames_produced = frames_produced_.load(std::memory_order_relaxed);
    s.frames_dropped_ring = frames_dropped_ring_.load(std::memory_order_relaxed);
    s.ring_available = ring_available_.load(std::memory_order_relaxed);
    s.ring_free = ring_free_.load(std::memory_order_relaxed);

    s.jitter_target_ms = jitter_target_ms_.load(std::memory_order_relaxed);
    s.jitter_depth_ms = jitter_depth_ms_.load(std::memory_order_relaxed);
    s.jitter_missing = jitter_missing_.load(std::memory_order_relaxed);
    s.jitter_late = jitter_late_.load(std::memory_order_relaxed);
    s.jitter_duplicate = jitter_duplicate_.load(std::memory_order_relaxed);
    s.jitter_evicted = jitter_evicted_.load(std::memory_order_relaxed);
    s.jitter_stalls = jitter_stalls_.load(std::memory_order_relaxed);

    s.frames_injected = frames_injected_.load(std::memory_order_relaxed);
    s.device_full_events = device_full_events_.load(std::memory_order_relaxed);
    s.frames_dropped_device = frames_dropped_device_.load(std::memory_order_relaxed);

    s.level_rms = level_rms_.load(std::memory_order_relaxed);
    s.level_peak = level_peak_.load(std::memory_order_relaxed);

    s.device_state = device_state_.load(std::memory_order_relaxed);
    s.device_frames_written = device_frames_written_.load(std::memory_order_relaxed);
    s.device_frames_consumed = device_frames_consumed_.load(std::memory_order_relaxed);
    s.device_frames_dropped = device_frames_dropped_.load(std::memory_order_relaxed);
    s.device_underrun = device_underrun_.load(std::memory_order_relaxed);
    s.device_overrun = device_overrun_.load(std::memory_order_relaxed);
    s.device_high_watermark = device_high_watermark_.load(std::memory_order_relaxed);
    s.device_ring_available = device_ring_available_.load(std::memory_order_relaxed);
    s.device_ring_free = device_ring_free_.load(std::memory_order_relaxed);
    return s;
}

double Stats::loss_percent() const
{
    uint64_t missing = jitter_missing_.load(std::memory_order_relaxed);
    uint64_t real = frames_decoded_.load(std::memory_order_relaxed);
    uint64_t total = missing + real;
    if (total == 0u) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(missing) / static_cast<double>(total);
}

std::string Stats::format_line(double cpu_load_percent) const
{
    StatsSnapshot s = snapshot();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "pkt=%llu loss=%.2f%% plc=%llu | jit tgt=%ums depth=%ums stalls=%u | "
                  "ring=%u/%u | inj=%llu full=%llu | dev=%s w=%llu c=%llu und=%u ovr=%u "
                  "hwm=%u | rms=%.3f peak=%.3f | cpu=%.0f%%",
                  static_cast<unsigned long long>(s.packets_received), loss_percent(),
                  static_cast<unsigned long long>(s.frames_plc), s.jitter_target_ms,
                  s.jitter_depth_ms, s.jitter_stalls, s.ring_available, s.ring_free,
                  static_cast<unsigned long long>(s.frames_injected),
                  static_cast<unsigned long long>(s.device_full_events),
                  device_state_name(s.device_state),
                  static_cast<unsigned long long>(s.device_frames_written),
                  static_cast<unsigned long long>(s.device_frames_consumed), s.device_underrun,
                  s.device_overrun, s.device_high_watermark,
                  static_cast<double>(s.level_rms), static_cast<double>(s.level_peak),
                  cpu_load_percent);
    return std::string(buf);
}

std::string Stats::format_summary() const
{
    StatsSnapshot s = snapshot();
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "收包 %llu 包 / %llu 字节（无效 %llu，忽略 %llu，连接事件 %llu）\n"
                  "解码 %llu 帧，PLC 补齐 %llu 帧（丢包率 %.2f%%），解码错误 %llu\n"
                  "抖动缓冲：目标 %ums，重缓冲 %u 次，迟到 %llu，重复 %llu，驱逐 %llu\n"
                  "环形缓冲：产出 %llu 帧，丢弃 %llu 帧\n"
                  "注入驱动：%llu 帧，缓冲满 %llu 次，丢弃 %llu 帧\n"
                  "驱动状态：%s，写入 %llu / 消费 %llu / 丢弃 %llu，欠载 %u 次，溢出 %u 次，"
                  "峰值深度 %u\n"
                  "电平：RMS %.3f，峰值 %.3f",
                  static_cast<unsigned long long>(s.packets_received),
                  static_cast<unsigned long long>(s.bytes_received),
                  static_cast<unsigned long long>(s.packets_invalid),
                  static_cast<unsigned long long>(s.packets_ignored),
                  static_cast<unsigned long long>(s.client_connected_events),
                  static_cast<unsigned long long>(s.frames_decoded),
                  static_cast<unsigned long long>(s.frames_plc), loss_percent(),
                  static_cast<unsigned long long>(s.decode_errors), s.jitter_target_ms,
                  s.jitter_stalls, static_cast<unsigned long long>(s.jitter_late),
                  static_cast<unsigned long long>(s.jitter_duplicate),
                  static_cast<unsigned long long>(s.jitter_evicted),
                  static_cast<unsigned long long>(s.frames_produced),
                  static_cast<unsigned long long>(s.frames_dropped_ring),
                  static_cast<unsigned long long>(s.frames_injected),
                  static_cast<unsigned long long>(s.device_full_events),
                  static_cast<unsigned long long>(s.frames_dropped_device),
                  device_state_name(s.device_state),
                  static_cast<unsigned long long>(s.device_frames_written),
                  static_cast<unsigned long long>(s.device_frames_consumed),
                  static_cast<unsigned long long>(s.device_frames_dropped), s.device_underrun,
                  s.device_overrun, s.device_high_watermark, static_cast<double>(s.level_rms),
                  static_cast<double>(s.level_peak));
    return std::string(buf);
}

}  // namespace miclink
