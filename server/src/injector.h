/*
 * injector.h -- 平台适配层 3/3：把 PCM 写进内核虚拟麦克风驱动。
 *
 * 抽象：IDeviceBackend
 *   - IoctlDeviceBackend：Windows 真实实现（CreateFileW + DeviceIoControl）
 *   - FakeDeviceBackend ：Linux / 冒烟测试用的等价实现（同一套状态机 + womic_ring）
 *
 * 只有 injector.cpp 里出现 Win32 特有代码，且全部包在 #ifdef _WIN32 中；
 * Linux 分支给出语义等价的实现，因此除 IOCTL 调用本身之外的所有逻辑
 * （启动时序、错误分类、背压退避、状态轮询）都在 Linux 上被真实执行过。
 *
 * 启动时序（严格按驱动契约）：
 *   CreateFileW(\\.\WomicControl)
 *     -> GET_VERSION   （比对驱动 ABI）
 *     -> GET_CAPS      （取 ringCapacityFrames / 支持格式 / 单次写入上限）
 *     -> SET_FORMAT    （端点采样率 / 声道 / 位深；驱动**绝不**重采样）
 *     -> START         （开始向端点供给数据）
 * 主循环：WRITE_PCM（METHOD_BUFFERED，非阻塞）
 * 退出：STOP -> RESET -> CloseHandle
 */
#ifndef WOMIC_SERVER_INJECTOR_H
#define WOMIC_SERVER_INJECTOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "womic/womic_ioctl.h"

#include "config.h"
#include "stats.h"

namespace womic {

/** 驱动/设备后端的统一接口（便于在 Linux 上用 Fake 实现跑端到端测试）。 */
class IDeviceBackend {
public:
    virtual ~IDeviceBackend() = default;

    /** 后端名（日志用）。 */
    virtual const char *name() const = 0;

    /** 打开设备。 */
    virtual bool open(std::string *err) = 0;

    /** 关闭设备（幂等）。 */
    virtual void close() = 0;

    virtual bool get_version(WOMIC_VERSION *out, std::string *err) = 0;
    virtual bool get_caps(WOMIC_CAPS *out, std::string *err) = 0;
    virtual bool set_format(const WOMIC_FORMAT &fmt, std::string *err) = 0;
    virtual bool start(const WOMIC_START_IO &io, std::string *err) = 0;
    virtual bool stop(std::string *err) = 0;
    virtual bool reset(const WOMIC_RESET_IO &io, std::string *err) = 0;
    virtual bool get_status(WOMIC_STATUS *out, std::string *err) = 0;

    /**
     * @brief 写 PCM。
     * @param pcm                 帧数据（frameCount * channels 个 int16）
     * @param frames              帧数
     * @param flags               WOMIC_WRITE_FLAG_*
     * @param out_written_frames  实际接纳的帧数（BUFFER_FULL 时可能为 0）
     * @param err                 失败原因
     * @return WOMIC_OK 或 WOMIC_ERR_*（见 womic_ioctl.h）
     */
    virtual uint32_t write_pcm(const int16_t *pcm, uint32_t frames, uint32_t flags,
                               uint32_t *out_written_frames, std::string *err) = 0;
};

/**
 * @brief 创建设备后端。
 * @param name "ioctl" | "fake" | "auto"
 *             auto：Windows 上使用 IOCTL；否则（或编译了 WOMIC_WITH_FAKE_DEVICE）用 Fake。
 */
std::unique_ptr<IDeviceBackend> create_device_backend(const std::string &name,
                                                      uint32_t fake_capacity_frames,
                                                      std::string *err);

/** WOMIC_ERR_* -> 文本。 */
const char *womic_error_string(uint32_t code);

/** WOMIC_STATE_* -> 文本。 */
const char *womic_state_string(uint32_t state);

/**
 * @brief 注入器：封装"打开 -> 设格式 -> 启动 -> 写 -> 停止"的完整生命周期。
 */
class Injector {
public:
    Injector() = default;
    ~Injector();

    Injector(const Injector &) = delete;
    Injector &operator=(const Injector &) = delete;

    /** 打开设备并完成启动时序（GET_VERSION/GET_CAPS/SET_FORMAT/START）。 */
    bool open(const ServerConfig &cfg, std::string *err);

    /** 停止并关闭（STOP -> RESET -> close）。 */
    void close();

    bool is_open() const { return opened_; }

    /**
     * @brief 写若干帧到设备；遇到 BUFFER_FULL 短暂退避重试，绝不长时间阻塞。
     * @return 实际写入的帧数
     */
    uint32_t write_frames(const int16_t *pcm, uint32_t frames, Stats *stats);

    /** 周期调用：GET_STATUS 更新统计。 */
    bool poll_status(Stats *stats, WOMIC_STATUS *out);

    const WOMIC_CAPS &caps() const { return caps_; }
    const WOMIC_FORMAT &format() const { return format_; }
    uint32_t frame_bytes() const { return frame_bytes_; }
    IDeviceBackend *backend() { return backend_.get(); }
    const char *backend_name() const { return (backend_ != nullptr) ? backend_->name() : "none"; }

private:
    std::unique_ptr<IDeviceBackend> backend_;
    WOMIC_CAPS caps_ {};
    WOMIC_FORMAT format_ {};
    uint32_t frame_bytes_ = 0u;
    uint32_t max_chunk_frames_ = 1024u;
    bool opened_ = false;
};

}  // namespace womic

#endif  /* WOMIC_SERVER_INJECTOR_H */
