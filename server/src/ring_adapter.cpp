/*
 * ring_adapter.cpp -- womic_ring 的 C++ 薄封装。
 */
#include "ring_adapter.h"

#include <cstdio>

#include "log.h"

namespace womic {

static constexpr char kWomicLogModule[] = "ring";

bool PcmRing::init(uint32_t capacity_frames, uint32_t frame_bytes, std::string *err)
{
    if ((frame_bytes == 0u) || ((frame_bytes % 2u) != 0u)) {
        if (err != nullptr) {
            *err = "ring_adapter: frame_bytes 必须为正偶数";
        }
        ready_ = false;
        return false;
    }
    if (capacity_frames == 0u) {
        if (err != nullptr) {
            *err = "ring_adapter: capacity_frames 不能为 0";
        }
        ready_ = false;
        return false;
    }

    /* 向下取整到 2 的幂（womic_ring 的硬约束）。 */
    uint32_t cap = 1u;
    while ((cap << 1u) <= capacity_frames) {
        cap <<= 1u;
    }

    const size_t samples = static_cast<size_t>(cap) * static_cast<size_t>(frame_bytes / 2u);
    storage_.assign(samples, 0);

    int rc = womic_ring_init(&ring_, storage_.data(), cap, frame_bytes);
    if (rc != WOMIC_RING_OK) {
        if (err != nullptr) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "womic_ring_init 失败(%d)", rc);
            *err = buf;
        }
        ready_ = false;
        return false;
    }
    ready_ = true;
    return true;
}

uint32_t PcmRing::write(const int16_t *frames, uint32_t n)
{
    if (!ready_ || (frames == nullptr) || (n == 0u)) {
        return 0u;
    }
    return womic_ring_write(&ring_, frames, n);
}

uint32_t PcmRing::read(int16_t *frames, uint32_t n)
{
    if (!ready_ || (frames == nullptr) || (n == 0u)) {
        return 0u;
    }
    return womic_ring_read(&ring_, frames, n);
}

uint32_t PcmRing::available() const
{
    if (!ready_) {
        return 0u;
    }
    return womic_ring_available(const_cast<womic_ring *>(&ring_));
}

uint32_t PcmRing::free_frames() const
{
    if (!ready_) {
        return 0u;
    }
    return womic_ring_free(const_cast<womic_ring *>(&ring_));
}

uint32_t PcmRing::capacity() const
{
    if (!ready_) {
        return 0u;
    }
    return ring_.capacity;
}

void PcmRing::add_dropped(uint64_t n)
{
    if (!ready_ || (n == 0u)) {
        return;
    }
    womic_ring_add_dropped(&ring_, n);
}

void PcmRing::reset()
{
    if (!ready_) {
        return;
    }
    womic_ring_reset(&ring_);
}

}  // namespace womic
