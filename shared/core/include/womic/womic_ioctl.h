/*
 * womic_ioctl.h -- 内核驱动 / Windows 服务端 / Linux 单元测试 三方共用的 IOCTL 契约。
 *
 * 【强约束】此文件是唯一事实来源（single source of truth）。驱动、服务端、单测
 * 必须共用这一份头文件，禁止各自复制粘贴一份结构体定义，否则一旦字段或编码不一致，
 * 内核与用户态的二进制布局就会错位（此类 bug 极难定位）。
 *
 * 结构体设计规则：
 *   - 禁止任何指针字段（内核态指针在用户态无意义，且 32/64 位宽度不同）；
 *   - 全部使用定宽类型（uint32_t / uint64_t / int16_t）；
 *   - 字段按自然对齐排列，MSVC 与 GCC、32 位与 64 位布局一致（下方静态断言保证）；
 *   - 新增字段只能追加到结构体尾部并同步提升 version，禁止在中间插入。
 */
#ifndef WOMIC_IOCTL_H
#define WOMIC_IOCTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * IOCTL 控制码
 * ------------------------------------------------------------------ */
#define WOMIC_IOCTL_DEVICE_TYPE  0x00000022u   /* FILE_DEVICE_UNKNOWN */

/* 与 Windows CTL_CODE 宏完全一致：DeviceType<<16 | Access<<14 | Function<<2 | Method */
#define WOMIC_CTL_CODE(fn, method, access) \
    ((uint32_t)(((WOMIC_IOCTL_DEVICE_TYPE) << 16) | ((uint32_t)(access) << 14) | \
                ((uint32_t)(fn) << 2) | (uint32_t)(method)))

#define WOMIC_METHOD_BUFFERED 0u   /* METHOD_BUFFERED */
#define WOMIC_ACCESS_ANY      0u   /* FILE_ANY_ACCESS */
#define WOMIC_ACCESS_WRITE    1u   /* FILE_WRITE_ACCESS */

#define WOMIC_IOCTL_GET_VERSION WOMIC_CTL_CODE(0x900, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_ANY)
#define WOMIC_IOCTL_GET_CAPS    WOMIC_CTL_CODE(0x901, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_ANY)
#define WOMIC_IOCTL_GET_FORMAT  WOMIC_CTL_CODE(0x902, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_ANY)
#define WOMIC_IOCTL_SET_FORMAT  WOMIC_CTL_CODE(0x903, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_WRITE)
#define WOMIC_IOCTL_WRITE_PCM   WOMIC_CTL_CODE(0x904, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_WRITE)
#define WOMIC_IOCTL_GET_STATUS  WOMIC_CTL_CODE(0x905, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_ANY)
#define WOMIC_IOCTL_RESET       WOMIC_CTL_CODE(0x906, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_WRITE)
#define WOMIC_IOCTL_START       WOMIC_CTL_CODE(0x907, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_WRITE)
#define WOMIC_IOCTL_STOP        WOMIC_CTL_CODE(0x908, WOMIC_METHOD_BUFFERED, WOMIC_ACCESS_WRITE)

/* ------------------------------------------------------------------ *
 * 统一错误码（驱动 NTSTATUS 与服务端 Win32 Error 的映射见注释）
 * ------------------------------------------------------------------ */
#define WOMIC_OK                      0u
/* 设备未就绪：尚未 START，或格式未设置。
   驱动 -> STATUS_DEVICE_NOT_READY (0xC00000A3)；服务端 -> ERROR_NOT_READY (21) */
#define WOMIC_ERR_NOT_READY           1u
/* 环形缓冲已满，本次 WRITE_PCM 无法全部接纳。
   驱动 -> STATUS_BUFFER_TOO_SMALL (0xC0000023)；服务端 -> ERROR_INSUFFICIENT_BUFFER (122) */
#define WOMIC_ERR_BUFFER_FULL         2u
/* 提交的格式与端点当前格式不一致（驱动绝不做重采样）。
   驱动 -> STATUS_INVALID_DEVICE_REQUEST (0xC0000010)；服务端 -> ERROR_NOT_SUPPORTED (50) */
#define WOMIC_ERR_FORMAT_MISMATCH     3u
/* 参数非法（NULL、0 长度、越界帧数等）。
   驱动 -> STATUS_INVALID_PARAMETER (0xC000000D)；服务端 -> ERROR_INVALID_PARAMETER (87) */
#define WOMIC_ERR_INVALID_PARAM       4u
/* 无有效音频流（缓冲已排空且未开启静音填充）。
   驱动 -> STATUS_END_OF_FILE (0xC0000011)；服务端 -> ERROR_NO_DATA (232) */
#define WOMIC_ERR_NO_STREAM           5u

/* WRITE_PCM_IO.flags 位定义 */
#define WOMIC_WRITE_FLAG_SILENCE_FILL 0x00000001u  /* 不足时补静音而非返回 NO_STREAM */

/* 设备状态（WOMIC_STATUS.state）*/
#define WOMIC_STATE_STOPPED   0u
#define WOMIC_STATE_STARTED   1u
#define WOMIC_STATE_RUNNING   2u
#define WOMIC_STATE_ERROR     3u

/* ------------------------------------------------------------------ *
 * 共享结构体（无指针、定宽、自然对齐）
 * ------------------------------------------------------------------ */
typedef struct _WOMIC_VERSION {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t build;
} WOMIC_VERSION;

typedef struct _WOMIC_FORMAT {
    uint32_t sampleRate;     /* 16000 / 44100 / 48000 */
    uint32_t channels;       /* 1 / 2 */
    uint32_t bitsPerSample;  /* 16 */
} WOMIC_FORMAT;

typedef struct _WOMIC_CAPS {
    uint32_t ringCapacityFrames;     /* 环形缓冲帧容量（2 的幂） */
    uint32_t supportedRates[8];      /* 支持的采样率列表，0 表示结尾 */
    uint32_t supportedChannels;      /* 位掩码：bit0=1ch, bit1=2ch */
    uint32_t maxFrameBytesPerIoctl;  /* 单次 WRITE_PCM 最大字节数 */
    uint32_t supportsSilenceFill;    /* 0/1：是否支持静音填充 */
} WOMIC_CAPS;

/* WRITE_PCM 的输入头：紧随其后的是 frameCount * frameBytes 字节的 PCM 数据 */
typedef struct _WOMIC_WRITE_PCM_IO {
    uint32_t frameCount;   /* 本次写入的帧数 */
    uint32_t frameBytes;   /* 每帧字节数 = channels * bitsPerSample / 8 */
    uint32_t flags;        /* WOMIC_WRITE_FLAG_* */
    uint32_t reserved;     /* 必须为 0，保证 8 字节对齐与未来扩展 */
} WOMIC_WRITE_PCM_IO;

typedef struct _WOMIC_STATUS {
    uint32_t state;                /* WOMIC_STATE_* */
    uint32_t ringAvailableFrames;  /* 可读帧数 */
    uint32_t ringFreeFrames;       /* 可写帧数 */
    uint32_t underrunCount;        /* 读取侧欠载次数（补静音或返回 NO_STREAM） */
    uint64_t framesWritten;        /* 累计写入帧数 */
    uint64_t framesConsumed;       /* 累计消费帧数 */
    uint64_t droppedFrames;        /* 累计丢帧（缓冲满） */
    uint32_t overrunCount;         /* 写入侧溢出次数 */
    uint32_t ringHighWatermark;    /* 缓冲深度历史峰值（帧） */
    WOMIC_FORMAT currentFormat;    /* 当前端点格式 */
} WOMIC_STATUS;

typedef struct _WOMIC_RESET_IO {
    uint32_t keepFormat;   /* 1: 保留当前格式；0: 复位为默认格式 */
    uint32_t clearStats;   /* 1: 同时清零统计计数器 */
    uint32_t reserved[2];
} WOMIC_RESET_IO;

typedef struct _WOMIC_START_IO {
    uint32_t sampleRate;      /* 期望端点采样率，0 表示沿用当前格式 */
    uint32_t channels;        /* 期望声道数，0 表示沿用当前格式 */
    uint32_t fillSilence;     /* 1: 缓冲空时补静音；0: 返回 NO_STREAM */
    uint32_t reserved;
} WOMIC_START_IO;

/* ------------------------------------------------------------------ *
 * 布局静态断言：任何一处被意外改动都会导致编译失败，从而杜绝
 * “驱动和服务端结构体不一致”这类隐蔽错误。
 * ------------------------------------------------------------------ */
#define WOMIC_STATIC_ASSERT(name, cond) \
    typedef char womic_static_assert_##name[(cond) ? 1 : -1]

WOMIC_STATIC_ASSERT(womic_version_size,  sizeof(WOMIC_VERSION)  == 16);
WOMIC_STATIC_ASSERT(womic_format_size,   sizeof(WOMIC_FORMAT)   == 12);
WOMIC_STATIC_ASSERT(womic_caps_size,     sizeof(WOMIC_CAPS)     == 48);
WOMIC_STATIC_ASSERT(womic_write_io_size, sizeof(WOMIC_WRITE_PCM_IO) == 16);
WOMIC_STATIC_ASSERT(womic_reset_io_size, sizeof(WOMIC_RESET_IO) == 16);
WOMIC_STATIC_ASSERT(womic_start_io_size, sizeof(WOMIC_START_IO) == 16);
/*
 * WOMIC_STATUS 布局（MSVC/GCC、32/64 位一致）：
 *   state@0, ringAvailableFrames@4, ringFreeFrames@8, underrunCount@12,
 *   [4 字节对齐填充] framesWritten@16, framesConsumed@24, droppedFrames@32,
 *   overrunCount@40, ringHighWatermark@44, currentFormat@48 -> 总长 60 -> 对齐到 64
 */
WOMIC_STATIC_ASSERT(womic_status_size, sizeof(WOMIC_STATUS) == 64);

#ifdef __cplusplus
}
#endif

#endif /* WOMIC_IOCTL_H */
