/*
 * injector.cpp -- 设备后端与注入器实现。
 *
 * Windows 分支（CreateFileW / DeviceIoControl）在 Linux 沙箱中无法编译验证；
 * 所有共享结构体均直接 include shared/core/include/womic/womic_ioctl.h，
 * 未做任何本地复制，因此字段布局由该头文件的静态断言保证一致。
 */
#include "injector.h"

#include <cstdio>
#include <cstring>

#include "fake_device.h"
#include "log.h"
#include "ring_adapter.h"
#include "threads_sync.h"
#include "womic/womic_ring.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace womic {

static constexpr char kWomicLogModule[] = "inject";

/* WRITE_PCM 遇到缓冲满时的退避上限：最多 8 次 × 1ms = 8ms。
   音频线程绝不允许在这里长时间阻塞（否则端点会欠载、听感上是咔哒）。 */
constexpr int kMaxWriteAttempts = 8;
constexpr uint32_t kBackoffMs = 1u;

const char *womic_error_string(uint32_t code)
{
    switch (code) {
        case WOMIC_OK:
            return "OK";
        case WOMIC_ERR_NOT_READY:
            return "NOT_READY";
        case WOMIC_ERR_BUFFER_FULL:
            return "BUFFER_FULL";
        case WOMIC_ERR_FORMAT_MISMATCH:
            return "FORMAT_MISMATCH";
        case WOMIC_ERR_INVALID_PARAM:
            return "INVALID_PARAM";
        case WOMIC_ERR_NO_STREAM:
            return "NO_STREAM";
        default:
            return "UNKNOWN";
    }
}

const char *womic_state_string(uint32_t state)
{
    switch (state) {
        case WOMIC_STATE_STOPPED:
            return "STOPPED";
        case WOMIC_STATE_STARTED:
            return "STARTED";
        case WOMIC_STATE_RUNNING:
            return "RUNNING";
        case WOMIC_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/* =================================================================== *
 * IoctlDeviceBackend：Windows 真实实现
 * =================================================================== */
class IoctlDeviceBackend final : public IDeviceBackend {
public:
    explicit IoctlDeviceBackend(std::string device_path)
        : path_(std::move(device_path))
    {
#ifdef _WIN32
        path_wide_ = utf8_to_wide(path_);
#endif
    }

    ~IoctlDeviceBackend() override { close(); }

    const char *name() const override { return "ioctl"; }

    bool open(std::string *err) override
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            return true;
        }
        /*
         *  \\.\WomicControl 是驱动创建的控制设备（IoCreateDeviceSecure +
         *  IoCreateSymbolicLink 到 \DosDevices\Global\WomicControl）。
         *  参数说明：
         *    dwShareMode = 0            —— 独占，避免第二个进程注入造成交错写；
         *    dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL —— 这里**不**用
         *        FILE_FLAG_OVERLAPPED：WRITE_PCM 的语义是"能写多少写多少"，
         *        同步调用即可，异步化只会把背压藏起来并增加 8 字节/帧的开销。
         */
        handle_ = CreateFileW(path_wide_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (err != nullptr) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "CreateFileW(%s) 失败，GetLastError=%lu",
                              path_.c_str(), static_cast<unsigned long>(e));
                *err = buf;
            }
            return false;
        }
        WOMIC_LOGI("已打开控制设备 %s", path_.c_str());
        return true;
#else
        if (err != nullptr) {
            *err = "IOCTL 设备后端只能在 Windows 上使用（当前平台无 \\\\.\\WomicControl）";
        }
        return false;
#endif
    }

    void close() override
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            WOMIC_LOGI("已关闭控制设备 %s", path_.c_str());
        }
#endif
    }

    bool get_version(WOMIC_VERSION *out, std::string *err) override
    {
        return ioctl_call(WOMIC_IOCTL_GET_VERSION, nullptr, 0u, out,
                          (out != nullptr) ? sizeof(*out) : 0u, err) == WOMIC_OK;
    }

    bool get_caps(WOMIC_CAPS *out, std::string *err) override
    {
        return ioctl_call(WOMIC_IOCTL_GET_CAPS, nullptr, 0u, out,
                          (out != nullptr) ? sizeof(*out) : 0u, err) == WOMIC_OK;
    }

    bool set_format(const WOMIC_FORMAT &fmt, std::string *err) override
    {
        WOMIC_FORMAT in = fmt; /* 共享结构体，按值传，无指针字段 */
        return ioctl_call(WOMIC_IOCTL_SET_FORMAT, &in, sizeof(in), nullptr, 0u, err) == WOMIC_OK;
    }

    bool start(const WOMIC_START_IO &io, std::string *err) override
    {
        WOMIC_START_IO in = io;
        return ioctl_call(WOMIC_IOCTL_START, &in, sizeof(in), nullptr, 0u, err) == WOMIC_OK;
    }

    bool stop(std::string *err) override
    {
        return ioctl_call(WOMIC_IOCTL_STOP, nullptr, 0u, nullptr, 0u, err) == WOMIC_OK;
    }

    bool reset(const WOMIC_RESET_IO &io, std::string *err) override
    {
        WOMIC_RESET_IO in = io;
        return ioctl_call(WOMIC_IOCTL_RESET, &in, sizeof(in), nullptr, 0u, err) == WOMIC_OK;
    }

    bool get_status(WOMIC_STATUS *out, std::string *err) override
    {
        return ioctl_call(WOMIC_IOCTL_GET_STATUS, nullptr, 0u, out,
                          (out != nullptr) ? sizeof(*out) : 0u, err) == WOMIC_OK;
    }

    uint32_t write_pcm(const int16_t *pcm, uint32_t frames, uint32_t flags,
                       uint32_t *out_written_frames, std::string *err) override
    {
        if (out_written_frames != nullptr) {
            *out_written_frames = 0u;
        }
        if ((pcm == nullptr) || (frames == 0u)) {
            return WOMIC_ERR_INVALID_PARAM;
        }

        /*
         * METHOD_BUFFERED：输入缓冲区 = [WOMIC_WRITE_PCM_IO][PCM...]，
         * 由系统内核在用户态与内核态之间整体拷贝一次，驱动直接拿到连续内存。
         * 因此这里必须自己把头和数据拼成一个连续块（不能用两个指针）。
         */
        const uint32_t frame_bytes = frame_bytes_;
        if (frame_bytes == 0u) {
            return WOMIC_ERR_INVALID_PARAM;
        }
        const size_t payload_bytes = static_cast<size_t>(frames) * static_cast<size_t>(frame_bytes);
        const size_t total = sizeof(WOMIC_WRITE_PCM_IO) + payload_bytes;

        if (scratch_.size() < total) {
            scratch_.resize(total);
        }

        WOMIC_WRITE_PCM_IO header {};
        header.frameCount = frames;
        header.frameBytes = frame_bytes;
        header.flags = flags;
        header.reserved = 0u; /* 必须为 0：驱动侧会校验，非零直接拒绝 */
        std::memcpy(scratch_.data(), &header, sizeof(header));
        std::memcpy(scratch_.data() + sizeof(header), pcm, payload_bytes);

        WOMIC_STATUS status_out {};
        uint32_t rc = ioctl_call(WOMIC_IOCTL_WRITE_PCM, scratch_.data(), total, &status_out,
                                 sizeof(status_out), err);
        if (rc == WOMIC_OK) {
            if (out_written_frames != nullptr) {
                *out_written_frames = frames;
            }
            return WOMIC_OK;
        }
        if (rc == WOMIC_ERR_BUFFER_FULL) {
            /*
             * 缓冲满：驱动一个样本都没接纳。返回让上层退避重试。
             * 这里**不**睡眠 —— 睡眠属于调用方策略（Injector::write_frames），
             * 后端只负责如实报告状态。
             */
            if (err != nullptr) {
                err->clear();
            }
            return WOMIC_ERR_BUFFER_FULL;
        }
        return rc;
    }

    /** SET_FORMAT 成功后由 Injector 调用，后续 WRITE_PCM 用它算 payload 大小。 */
    void set_frame_bytes(uint32_t frame_bytes) { frame_bytes_ = frame_bytes; }

private:
#ifdef _WIN32
    /** 统一的 DeviceIoControl 封装 + NTSTATUS->Win32 错误码映射（反向）。 */
    uint32_t ioctl_call(uint32_t code, const void *in, size_t in_size, void *out, size_t out_size,
                        std::string *err)
    {
        if (handle_ == INVALID_HANDLE_VALUE) {
            if (err != nullptr) {
                *err = "控制设备未打开";
            }
            return WOMIC_ERR_NOT_READY;
        }
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(handle_, static_cast<DWORD>(code),
                                  const_cast<LPVOID>(in), static_cast<DWORD>(in_size), out,
                                  static_cast<DWORD>(out_size), &returned, nullptr);
        if (ok) {
            if (err != nullptr) {
                err->clear();
            }
            return WOMIC_OK;
        }
        const DWORD e = GetLastError();
        uint32_t mapped = map_win32_error(e);
        if (err != nullptr) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "DeviceIoControl(0x%08X) 失败，GetLastError=%lu -> %s",
                          static_cast<unsigned>(code), static_cast<unsigned long>(e),
                          womic_error_string(mapped));
            *err = buf;
        }
        return mapped;
    }

    static uint32_t map_win32_error(DWORD e)
    {
        switch (e) {
            case ERROR_NOT_READY:             /* 21  */
                return WOMIC_ERR_NOT_READY;
            case ERROR_INSUFFICIENT_BUFFER:   /* 122 */
            case ERROR_MORE_DATA:             /* 234 */
                return WOMIC_ERR_BUFFER_FULL;
            case ERROR_NOT_SUPPORTED:         /* 50  */
            case ERROR_INVALID_FUNCTION:      /* 1   */
                return WOMIC_ERR_FORMAT_MISMATCH;
            case ERROR_INVALID_PARAMETER:     /* 87  */
                return WOMIC_ERR_INVALID_PARAM;
            case ERROR_NO_DATA:               /* 232 */
                return WOMIC_ERR_NO_STREAM;
            default:
                return WOMIC_ERR_INVALID_PARAM;
        }
    }

    static std::wstring utf8_to_wide(const std::string &utf8)
    {
        if (utf8.empty()) {
            return std::wstring();
        }
        int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (need <= 0) {
            return std::wstring();
        }
        std::wstring wide(static_cast<size_t>(need), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], need);
        while (!wide.empty() && (wide.back() == L'\0')) {
            wide.pop_back();
        }
        return wide;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_wide_;
#else
    uint32_t ioctl_call(uint32_t code, const void *in, size_t in_size, void *out, size_t out_size,
                        std::string *err)
    {
        (void)code;
        (void)in;
        (void)in_size;
        (void)out;
        (void)out_size;
        if (err != nullptr) {
            *err = "IOCTL 设备后端只能在 Windows 上使用";
        }
        return WOMIC_ERR_NOT_READY;
    }
#endif

    std::string path_;
    std::vector<uint8_t> scratch_;
    uint32_t frame_bytes_ = 0u;
};

std::unique_ptr<IDeviceBackend> create_device_backend(const std::string &name,
                                                      uint32_t fake_capacity_frames,
                                                      std::string *err)
{
    std::string lower = name;
    for (char &c : lower) {
        if ((c >= 'A') && (c <= 'Z')) {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    bool want_fake = (lower == "fake");
    if (lower == "auto") {
#if defined(WOMIC_WITH_FAKE_DEVICE)
        /* 该开关打开时（Linux CI / 冒烟测试默认打开）auto 优先解析为 Fake。 */
        want_fake = true;
#elif !defined(_WIN32)
        want_fake = true;
#else
        want_fake = false;
#endif
    }

    if (want_fake) {
        return std::unique_ptr<IDeviceBackend>(new FakeDeviceBackend(fake_capacity_frames));
    }
    if ((lower == "ioctl") || (lower == "auto")) {
        return std::unique_ptr<IDeviceBackend>(new IoctlDeviceBackend("\\\\.\\WomicControl"));
    }
    if (err != nullptr) {
        *err = "未知设备后端 '" + name + "'（可选 auto/ioctl/fake）";
    }
    return nullptr;
}

/* =================================================================== *
 * Injector
 * =================================================================== */

Injector::~Injector()
{
    close();
}

bool Injector::open(const ServerConfig &cfg, std::string *err)
{
    backend_ = create_device_backend(cfg.device_backend, cfg.fake_capacity_frames, err);
    if (backend_ == nullptr) {
        return false;
    }

    /* 命令行里显式给了设备路径时，用它覆盖默认路径。 */
    if ((cfg.device_backend == "ioctl") && !cfg.device_path.empty()) {
        backend_ = std::unique_ptr<IDeviceBackend>(new IoctlDeviceBackend(cfg.device_path));
    }

    if (!backend_->open(err)) {
        return false;
    }

    WOMIC_VERSION version {};
    if (!backend_->get_version(&version, err)) {
        return false;
    }
    WOMIC_LOGI("驱动版本 %u.%u.%u.%u", version.major, version.minor, version.patch, version.build);

    if (!backend_->get_caps(&caps_, err)) {
        return false;
    }
    WOMIC_LOGI("驱动能力：环形容量 %u 帧，单次写入上限 %u 字节，静音填充=%u，声道掩码=0x%x",
               caps_.ringCapacityFrames, caps_.maxFrameBytesPerIoctl, caps_.supportsSilenceFill,
               caps_.supportedChannels);
    WOMIC_LOGI("驱动支持采样率：%u %u %u %u %u %u %u %u", caps_.supportedRates[0],
               caps_.supportedRates[1], caps_.supportedRates[2], caps_.supportedRates[3],
               caps_.supportedRates[4], caps_.supportedRates[5], caps_.supportedRates[6],
               caps_.supportedRates[7]);

    /* 端点格式校验：驱动支持的采样率列表里必须能找到目标采样率。 */
    bool rate_supported = false;
    for (uint32_t i = 0u; i < 8u; ++i) {
        if (caps_.supportedRates[i] == cfg.endpoint_sample_rate) {
            rate_supported = true;
            break;
        }
        if (caps_.supportedRates[i] == 0u) {
            break;
        }
    }
    if (!rate_supported) {
        if (err != nullptr) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "驱动不支持端点采样率 %u Hz", cfg.endpoint_sample_rate);
            *err = buf;
        }
        return false;
    }
    if ((caps_.supportedChannels & (1u << (cfg.endpoint_channels - 1u))) == 0u) {
        if (err != nullptr) {
            *err = "驱动不支持该声道数（当前端点只支持单声道）";
        }
        return false;
    }

    format_.sampleRate = cfg.endpoint_sample_rate;
    format_.channels = cfg.endpoint_channels;
    format_.bitsPerSample = cfg.endpoint_bits_per_sample;
    frame_bytes_ = format_.channels * (format_.bitsPerSample / 8u);
    if (frame_bytes_ == 0u) {
        if (err != nullptr) {
            *err = "位深非法（必须是 16）";
        }
        return false;
    }

    if (!backend_->set_format(format_, err)) {
        return false;
    }
    WOMIC_LOGI("已设置端点格式：%u Hz / %u ch / %u bit（帧 %u 字节）", format_.sampleRate,
               format_.channels, format_.bitsPerSample, frame_bytes_);

    /* 让派生后端知道帧大小（真实 IOCTL 后端需要它拼 WRITE_PCM 的包体）。 */
    {
        IoctlDeviceBackend *ioctl_backend = dynamic_cast<IoctlDeviceBackend *>(backend_.get());
        if (ioctl_backend != nullptr) {
            ioctl_backend->set_frame_bytes(frame_bytes_);
        }
    }

    /* 单次写入上限：受 maxFrameBytesPerIoctl 与配置双重约束。 */
    uint32_t by_ioctl = caps_.maxFrameBytesPerIoctl / frame_bytes_;
    max_chunk_frames_ = cfg.max_write_frames;
    if ((by_ioctl > 0u) && (by_ioctl < max_chunk_frames_)) {
        max_chunk_frames_ = by_ioctl;
    }
    if (max_chunk_frames_ == 0u) {
        max_chunk_frames_ = 1u;
    }

    WOMIC_START_IO start_io {};
    start_io.sampleRate = cfg.endpoint_sample_rate;
    start_io.channels = cfg.endpoint_channels;
    start_io.fillSilence = 1u; /* 缓冲排空时补静音，避免端点返回 NO_STREAM */
    start_io.reserved = 0u;
    if (!backend_->start(start_io, err)) {
        return false;
    }

    opened_ = true;
    WOMIC_LOGI("端点已启动（后端 %s），等待 PCM 注入", backend_->name());
    return true;
}

void Injector::close()
{
    if ((backend_ == nullptr) || !opened_) {
        if (backend_ != nullptr) {
            backend_->close();
        }
        opened_ = false;
        return;
    }

    std::string err;
    if (!backend_->stop(&err)) {
        WOMIC_LOGW("STOP 失败：%s", err.c_str());
    }
    WOMIC_RESET_IO reset_io {};
    reset_io.keepFormat = 0u;
    reset_io.clearStats = 1u;
    reset_io.reserved[0] = 0u;
    reset_io.reserved[1] = 0u;
    if (!backend_->reset(reset_io, &err)) {
        WOMIC_LOGW("RESET 失败：%s", err.c_str());
    }
    backend_->close();
    /*
     * 刻意**不** reset backend_：退出后仍允许读取 Fake 后端的最终计数
     * （测试与 --dry-run 诊断会用到）；下一次 open() 会直接替换它。
     */
    opened_ = false;
    WOMIC_LOGI("端点已停止并复位");
}

uint32_t Injector::write_frames(const int16_t *pcm, uint32_t frames, Stats *stats)
{
    if ((backend_ == nullptr) || !opened_ || (pcm == nullptr) || (frames == 0u)) {
        return 0u;
    }

    const uint32_t frame_samples = frame_bytes_ / 2u;
    uint32_t total_written = 0u;
    int attempts = 0;

    while (total_written < frames) {
        uint32_t remaining = frames - total_written;
        uint32_t chunk = (remaining > max_chunk_frames_) ? max_chunk_frames_ : remaining;

        uint32_t written = 0u;
        std::string err;
        uint32_t rc = backend_->write_pcm(pcm + static_cast<size_t>(total_written) *
                                                    static_cast<size_t>(frame_samples),
                                          chunk, 0u, &written, &err);
        if (rc == WOMIC_OK) {
            if (written == 0u) {
                /* 后端报告成功但没接纳：视为满，按退避处理。 */
                rc = WOMIC_ERR_BUFFER_FULL;
            } else {
                total_written += written;
                if (stats != nullptr) {
                    stats->on_frame_injected(written);
                }
                continue;
            }
        }

        if (rc == WOMIC_ERR_BUFFER_FULL) {
            if (stats != nullptr) {
                stats->on_device_full();
            }
            ++attempts;
            if (attempts >= kMaxWriteAttempts) {
                /* 放弃剩余数据：绝不为了追平而长时间阻塞音频线程。 */
                if (stats != nullptr) {
                    stats->on_device_drop(frames - total_written);
                }
                WOMIC_LOGV("WRITE_PCM 连续 %d 次缓冲满，丢弃剩余 %u 帧", kMaxWriteAttempts,
                           frames - total_written);
                break;
            }
            sleep_ms(kBackoffMs);
            continue;
        }

        /* 其余错误（NOT_READY / FORMAT_MISMATCH / INVALID_PARAM / NO_STREAM）：
           不是"等一会儿就好"，继续重试无意义，直接放弃本次并对严重者告警。 */
        if (rc != WOMIC_ERR_NO_STREAM) {
            WOMIC_LOGW("WRITE_PCM 失败：%s（%s）", womic_error_string(rc), err.c_str());
        }
        if (stats != nullptr) {
            stats->on_device_drop(frames - total_written);
        }
        break;
    }

    return total_written;
}

bool Injector::poll_status(Stats *stats, WOMIC_STATUS *out)
{
    if ((backend_ == nullptr) || !opened_) {
        return false;
    }
    WOMIC_STATUS status {};
    std::string err;
    if (!backend_->get_status(&status, &err)) {
        WOMIC_LOGW("GET_STATUS 失败：%s", err.c_str());
        return false;
    }
    if (out != nullptr) {
        *out = status;
    }
    if (stats != nullptr) {
        stats->set_device_status(status.state, status.framesWritten, status.framesConsumed,
                                 status.droppedFrames, status.underrunCount, status.overrunCount,
                                 status.ringHighWatermark, status.ringAvailableFrames,
                                 status.ringFreeFrames);
    }
    return true;
}

}  // namespace womic
