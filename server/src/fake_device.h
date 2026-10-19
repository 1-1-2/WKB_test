/*
 * fake_device.h -- 供 Linux / CI 冒烟测试使用的设备后端（头文件内联实现）。
 *
 * 之所以单独成头文件：测试需要拿到这个具体类型（调用 drain() 模拟内核消费），
 * 而生产代码只通过 IDeviceBackend 抽象使用它，两者互不污染。
 *
 * 语义与真实驱动一一对应，便于在非 Windows 平台上验证驱动状态机：
 *   - 未 START                      -> WOMIC_ERR_NOT_READY
 *   - 帧字节数与 SET_FORMAT 不一致  -> WOMIC_ERR_FORMAT_MISMATCH（驱动绝不做重采样）
 *   - 环形缓冲满                    -> WOMIC_ERR_BUFFER_FULL，且一个样本都不接纳
 *   - GET_STATUS                    -> 返回真实的读写序号与统计
 * 唯一区别：数据由 drain() 手动取走，而不是被内核 DMA 中断取走。
 */
#ifndef WOMIC_SERVER_FAKE_DEVICE_H
#define WOMIC_SERVER_FAKE_DEVICE_H

#include <cstdint>
#include <cstring>
#include <string>

#include "injector.h"
#include "log.h"
#include "ring_adapter.h"
#include "womic/womic_ioctl.h"

namespace womic {

/*
 * 本头文件会被 injector.cpp 与测试同时包含，因此这里**不**定义
 * kWomicLogModule（会与 injector.cpp 里的同名常量冲突），直接把模块名
 * 以字面量传给 log_write()。
 */
class FakeDeviceBackend final : public IDeviceBackend {
public:
    explicit FakeDeviceBackend(uint32_t capacity_frames) : capacity_(capacity_frames) {}

    ~FakeDeviceBackend() override = default;

    const char *name() const override { return "fake"; }

    bool open(std::string *err) override
    {
        opened_ = true;
        if (err != nullptr) {
            err->clear();
        }
        ::womic::log_write(::womic::LogLevel::kInfo, "fake", "Fake 设备后端已打开（容量 %u 帧）",
                           capacity_);
        return true;
    }

    void close() override
    {
        opened_ = false;
        started_ = false;
    }

    bool get_version(WOMIC_VERSION *out, std::string *err) override
    {
        if (!opened_) {
            if (err != nullptr) {
                *err = "Fake 设备未打开";
            }
            return false;
        }
        if (out != nullptr) {
            out->major = 1u;
            out->minor = 0u;
            out->patch = 0u;
            out->build = 0u;
        }
        return true;
    }

    bool get_caps(WOMIC_CAPS *out, std::string *err) override
    {
        if (!opened_) {
            if (err != nullptr) {
                *err = "Fake 设备未打开";
            }
            return false;
        }
        if (out != nullptr) {
            std::memset(out, 0, sizeof(*out));
            out->ringCapacityFrames = capacity_;
            out->supportedRates[0] = 44100u;
            out->supportedRates[1] = 48000u; /* 后续为 0，表示列表结束 */
            out->supportedChannels = 0x1u;   /* bit0 = 单声道 */
            out->maxFrameBytesPerIoctl = 64u * 1024u;
            out->supportsSilenceFill = 1u;
        }
        return true;
    }

    bool set_format(const WOMIC_FORMAT &fmt, std::string *err) override
    {
        if (!opened_) {
            if (err != nullptr) {
                *err = "Fake 设备未打开";
            }
            return false;
        }
        const uint32_t frame_bytes = fmt.channels * (fmt.bitsPerSample / 8u);
        if ((fmt.channels == 0u) || (fmt.bitsPerSample != 16u) || (frame_bytes == 0u)) {
            if (err != nullptr) {
                *err = "Fake 设备只支持 16-bit PCM";
            }
            return false;
        }
        std::string ring_err;
        if (!ring_.init(capacity_, frame_bytes, &ring_err)) {
            if (err != nullptr) {
                *err = ring_err;
            }
            return false;
        }
        format_ = fmt;
        frame_bytes_ = frame_bytes;
        format_set_ = true;
        return true;
    }

    bool start(const WOMIC_START_IO &io, std::string *err) override
    {
        if (!opened_ || !format_set_) {
            if (err != nullptr) {
                *err = "Fake 设备：请先 SET_FORMAT";
            }
            return false;
        }
        if ((io.sampleRate != 0u) && (io.sampleRate != format_.sampleRate)) {
            if (err != nullptr) {
                *err = "Fake 设备：START 的采样率与 SET_FORMAT 不一致";
            }
            return false;
        }
        if ((io.channels != 0u) && (io.channels != format_.channels)) {
            if (err != nullptr) {
                *err = "Fake 设备：START 的声道数与 SET_FORMAT 不一致";
            }
            return false;
        }
        fill_silence_ = (io.fillSilence != 0u);
        started_ = true;
        state_ = WOMIC_STATE_RUNNING;
        ::womic::log_write(::womic::LogLevel::kInfo, "fake",
                           "Fake 设备已启动：%u Hz / %u ch / %u bit，静音填充=%d",
                           format_.sampleRate, format_.channels, format_.bitsPerSample,
                           static_cast<int>(fill_silence_));
        return true;
    }

    bool stop(std::string *err) override
    {
        started_ = false;
        state_ = WOMIC_STATE_STOPPED;
        if (err != nullptr) {
            err->clear();
        }
        return true;
    }

    bool reset(const WOMIC_RESET_IO &io, std::string *err) override
    {
        ring_.reset();
        if (io.clearStats != 0u) {
            frames_written_ = 0u;
            frames_dropped_ = 0u;
            underrun_ = 0u;
            overrun_ = 0u;
            high_watermark_ = 0u;
        }
        if (io.keepFormat == 0u) {
            format_set_ = false;
            format_ = WOMIC_FORMAT {};
        }
        started_ = false;
        state_ = WOMIC_STATE_STOPPED;
        if (err != nullptr) {
            err->clear();
        }
        return true;
    }

    bool get_status(WOMIC_STATUS *out, std::string *err) override
    {
        if (out == nullptr) {
            if (err != nullptr) {
                *err = "out 为空";
            }
            return false;
        }
        std::memset(out, 0, sizeof(*out));
        out->state = state_;
        out->ringAvailableFrames = ring_.available();
        out->ringFreeFrames = ring_.free_frames();
        out->underrunCount = underrun_;
        out->framesWritten = frames_written_;
        out->framesConsumed = frames_consumed_;
        out->droppedFrames = frames_dropped_;
        out->overrunCount = overrun_;
        out->ringHighWatermark = high_watermark_;
        out->currentFormat = format_;
        if (err != nullptr) {
            err->clear();
        }
        return true;
    }

    uint32_t write_pcm(const int16_t *pcm, uint32_t frames, uint32_t flags,
                       uint32_t *out_written_frames, std::string *err) override
    {
        (void)flags;
        if (out_written_frames != nullptr) {
            *out_written_frames = 0u;
        }
        if (!opened_ || !started_) {
            if (err != nullptr) {
                *err = "Fake 设备未启动";
            }
            return WOMIC_ERR_NOT_READY;
        }
        if ((pcm == nullptr) || (frames == 0u)) {
            return WOMIC_ERR_INVALID_PARAM;
        }
        const uint32_t frame_samples = frame_bytes_ / 2u;
        if (frame_samples == 0u) {
            return WOMIC_ERR_INVALID_PARAM;
        }
        if ((static_cast<uint64_t>(frames) * static_cast<uint64_t>(frame_bytes_)) >
            (64u * 1024u)) {
            return WOMIC_ERR_INVALID_PARAM; /* 超过 maxFrameBytesPerIoctl */
        }

        const uint32_t written = ring_.write(pcm, frames);
        if (written == 0u) {
            overrun_++;
            if (err != nullptr) {
                err->clear();
            }
            return WOMIC_ERR_BUFFER_FULL;
        }
        if (written < frames) {
            frames_dropped_ += static_cast<uint64_t>(frames - written);
            overrun_++;
        }
        frames_written_ += written;
        const uint32_t depth = ring_.available();
        if (depth > high_watermark_) {
            high_watermark_ = depth;
        }
        if (out_written_frames != nullptr) {
            *out_written_frames = written;
        }
        if (err != nullptr) {
            err->clear();
        }
        return WOMIC_OK;
    }

    /* ---- 测试辅助：模拟内核侧的 DMA 消费 ---- */

    /** 取走最多 max_frames 帧，返回实际取走帧数（0 表示缓冲已空）。 */
    uint32_t drain(int16_t *dst, uint32_t max_frames)
    {
        const uint32_t n = ring_.read(dst, max_frames);
        if (n == 0u) {
            underrun_++;
            return 0u;
        }
        frames_consumed_ += n;
        return n;
    }

    uint32_t available_frames() const { return ring_.available(); }
    uint64_t frames_written_total() const { return frames_written_; }
    uint64_t frames_consumed_total() const { return frames_consumed_; }
    uint64_t frames_dropped_total() const { return frames_dropped_; }
    uint32_t underrun_count() const { return underrun_; }
    uint32_t overrun_count() const { return overrun_; }
    uint32_t high_watermark() const { return high_watermark_; }
    uint32_t frame_bytes() const { return frame_bytes_; }

private:
    uint32_t capacity_ = 32768u;
    bool opened_ = false;
    bool format_set_ = false;
    bool started_ = false;
    bool fill_silence_ = true;
    uint32_t state_ = WOMIC_STATE_STOPPED;
    uint32_t frame_bytes_ = 0u;
    WOMIC_FORMAT format_ {};
    PcmRing ring_;
    uint64_t frames_written_ = 0u;
    uint64_t frames_consumed_ = 0u;
    uint64_t frames_dropped_ = 0u;
    uint32_t underrun_ = 0u;
    uint32_t overrun_ = 0u;
    uint32_t high_watermark_ = 0u;
};

}  // namespace womic

#endif  /* WOMIC_SERVER_FAKE_DEVICE_H */
