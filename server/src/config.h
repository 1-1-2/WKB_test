/*
 * config.h -- 服务端配置：结构体 + 命令行解析 + INI/JSON 配置文件加载。
 *
 * 平台中立（不含任何 #ifdef）。默认值即"开箱可用"值。
 */
#ifndef MICLINK_SERVER_CONFIG_H
#define MICLINK_SERVER_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace miclink {

/** 日志级别值，避免 config.h 依赖 log.h 的枚举（两者一一对应）。 */
enum class LogLevelValue : int {
    kError = 0,
    kWarn = 1,
    kInfo = 2,
    kVerbose = 3,
};

/** 重采样后端选择。 */
enum class ResamplerBackend : int {
    kAuto = 0,   /* 有 soxr 用 soxr，其次 speex，都没有则 none */
    kSoxr = 1,   /* libsoxr：音质最好 */
    kSpeex = 2,  /* libspeexdsp：依赖更小 */
    kNone = 3,   /* 不做重采样（要求采集率 == 端点率，否则拒绝启动） */
};

struct ServerConfig {
    /* ---- 网络 ---- */
    uint16_t listen_port = 5004;                /* UDP 监听端口 */
    uint32_t bind_ipv4 = 0;                     /* 0 = 通配地址 */
    uint32_t udp_recv_buf_bytes = 262144;       /* SO_RCVBUF = 256KB，抗 Wi-Fi 突发 */
    uint32_t client_timeout_ms = 3000;          /* 超过此时间没收到包视为客户端断开 */

    /* ---- 音频格式 ---- */
    uint32_t endpoint_sample_rate = 48000;      /* 驱动端点采样率（44100 或 48000） */
    uint32_t endpoint_channels = 1;             /* 端点声道：内核驱动只支持单声道 */
    uint32_t endpoint_bits_per_sample = 16;     /* 端点位深 */
    uint32_t expected_opus_rate = 48000;        /* 期望的手机侧 Opus 采样率 */
    uint32_t expected_channels = 1;             /* 期望的手机侧声道 */
    uint32_t frame_ms = 20;                     /* 每包帧长（10 或 20），首包会自动校正 */

    /* ---- 抖动缓冲 ---- */
    uint32_t jitter_min_ms = 50;                /* 目标延迟下限 */
    uint32_t jitter_max_ms = 100;               /* 目标延迟上限（硬上限 120） */
    uint32_t jitter_target_ms = 50;             /* 初始目标延迟（0 = 用 min） */
    uint32_t jitter_capacity_frames = 0;        /* 0 = 由 max_delay 推导 */

    /* ---- 缓冲 ---- */
    uint32_t ring_capacity_frames = 8192;       /* 用户态生产者环形缓冲（喂注入线程） */
    uint32_t max_write_frames = 1024;           /* 单次 WRITE_PCM 最大帧数 */

    /* ---- 设备 ---- */
    std::string device_backend = "auto";        /* auto | ioctl | fake */
    std::string device_path = "\\\\.\\MicLinkControl";  /* 控制设备符号链接 */
    uint32_t fake_capacity_frames = 32768;      /* Fake 后端环形容量（仅测试用） */

    /* ---- DSP ---- */
    float gain_db = 0.0f;                       /* 输出增益（dB） */
    bool declick = true;                        /* 丢包/断流后淡入，消除咔哒 */

    /* ---- 重采样 ---- */
    ResamplerBackend resampler = ResamplerBackend::kAuto;
    uint32_t resampler_quality = 4;             /* soxr: VHQ 档；speex: quality 0..10 */

    /* ---- 日志与统计 ---- */
    LogLevelValue log_level = LogLevelValue::kInfo;
    bool log_timestamp = true;
    bool enable_level_log = false;              /* 是否打印 RMS/peak 电平 */
    uint32_t status_interval_ms = 1000;         /* 状态行打印间隔 */

    /* ---- 发现服务（可选 P1） ---- */
    bool discovery_enabled = false;
    uint16_t discovery_port = 5005;
    std::string discovery_name = "MicLink Virtual Microphone";
    uint32_t discovery_interval_ms = 0;         /* 0 = 只应答不主动广播 */

    /* ---- 运行模式 ---- */
    bool dry_run = false;                       /* 只解析配置并打印，不启动 */
    bool show_help = false;
    bool show_version = false;
    std::string config_file;                    /* --config 指定的文件路径 */
};

/**
 * @brief 解析命令行（同时按 --config 或在 argv 中出现的配置文件路径加载文件）。
 *
 * 优先级：命令行 > 配置文件 > 默认值。
 *
 * @param argc/argv 命令行
 * @param out       输出配置
 * @param err       失败原因
 * @return true 成功
 */
bool config_parse(int argc, char **argv, ServerConfig *out, std::string *err);

/** 加载 INI（key = value，# 注释）或扁平 JSON 文件。 */
bool config_load_file(const std::string &path, ServerConfig *out, std::string *err);

/**
 * @brief 校验配置语义（端口、采样率链路、抖动区间等）。
 * 内部会调用 shared/core 的 miclink_check_sample_rate_chain()，
 * 端点率 != 解码率时直接拒绝（驱动绝不做重采样）。
 */
bool config_validate(const ServerConfig &cfg, std::string *err);

/** 把配置格式化成人类可读文本（--dry-run / 启动日志用）。 */
std::string config_to_string(const ServerConfig &cfg);

/** 打印 --help。 */
void config_print_usage(const char *program);

/** 版本号字符串。 */
const char *config_version_string();

}  // namespace miclink

#endif  /* MICLINK_SERVER_CONFIG_H */
