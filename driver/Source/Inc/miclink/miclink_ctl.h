/*++

Copyright (c) MicLink-open contributors. Licensed under the terms in NOTICE.md.

Module Name:

    miclink_ctl.h

Abstract:

    MICLINK 控制设备（\\.\MicLinkControl）的内核侧契约与共享状态。

    用户态（miclink 服务端）通过
        CreateFileW(L"\\\\.\\MicLinkControl", ...) + DeviceIoControl(...)
    把真实 PCM 注入到本驱动的 SPSC 环形缓冲；音频 miniport 的 DMA 通知 DPC
    再从同一个环形缓冲里取数据写进 WaveRT DMA 缓冲，从而让 Zoom / OBS / 录音机
    在输入设备列表里看到一枚名叫 "MicLink Virtual Microphone" 的真实麦克风。

    【为什么环形缓冲必须放在控制设备扩展里】
    消费端（miniport）跑在 DISPATCH_LEVEL 且持有自旋锁的 DPC 中，因此它只能通过
    一个"读全局指针 + 验签"的极简函数拿到 ring，绝不能做查找、加锁或 IO。
    所以 ring 作为控制设备扩展的一个内联字段存在，miniport 通过
    MicLinkGetControlExt() 直接取址。

--*/

#ifndef _MICLINK_CTL_H_
#define _MICLINK_CTL_H_

//
// 本头文件会被 C++（驱动）与 C（共享 core）两种翻译单元包含，
// 因此这里只使用 WDK 的基本类型，并且不依赖任何 C++ 特性。
//
//
// wdm.h 必须先于下面的 _KERNEL_MODE 兜底：
// 若某个 WDK 版本是在 wdm.h 里定义 _KERNEL_MODE 的，那么这里先包含它，
// 就能让兜底的 #ifndef 正确地"跳过"，绝不会与 WDK 的定义冲突（C4005）。
//
#include <wdm.h>

#if !defined(_KERNEL_MODE)
//
// 兜底：shared/core 的 miclink_atomic.h 用 `#if defined(_WIN32) && defined(_KERNEL_MODE)`
// 选择 WDK（ReadAcquire / WriteRelease / MemoryBarrier）分支。某些 WDK 版本不在
// 命令行上预定义 _KERNEL_MODE，这里补上，确保共享头一定走内核分支；
// 若构建环境已经定义过（命令行 /D 或 WDK 头），#ifndef 会跳过，不会产生 C4005。
//
#define _KERNEL_MODE 1
#endif

//
// 三方共享的 IOCTL 契约与无锁环形缓冲。
// 这两个头在 <repo>/shared/core/include/miclink/ 下，由 vcxproj 的
// $(SolutionDir)..\..\..\shared\core\include 搜索路径提供。
// 严禁在驱动里复制一份 —— 必须直接 include 原文件，否则二进制布局一旦漂移，
// 内核与用户态会静默错位。
//
#include "miclink/miclink_ioctl.h"
#include "miclink/miclink_ring.h"
#include "miclink/miclink_dsp.h"

//=============================================================================
// 常量
//=============================================================================

//
// 'WOMC' —— 必须放在 MICLINK_CONTROL_EXT 的第一个字段。
//
// 存在的理由（坑 1 / N-3 的另一半）：adapter.cpp 的 PnpHandler 会把"任何"收到 PNP IRP
// 的设备的 DeviceExtension 强转成 PortClassDeviceContext*。两段式判定的第二段
// （MicLinkIsControlDevice）正是靠这个 magic 来确认"这块 extension 真的是我们的"，
// 从而在设备已经被释放、指针失效的情况下也不会误判。
//
#define MICLINK_CONTROL_SIGNATURE         0x574F4D43UL

// 池 tag：'CmoW'（小端可读为 WomC）
#define MICLINK_POOLTAG                   'CmoW'

// 控制设备名（内核）/ 符号链接（用户态 \\.\MicLinkControl）
#define MICLINK_CONTROL_NT_NAME           L"\\Device\\MicLinkControl"

//
// \DosDevices\Global\ 保证符号链接落在全局 DOS 设备命名空间，
// 这样无论服务端跑在哪个 session（含服务 session 0）都能 \\.\MicLinkControl 打开。
//
#define MICLINK_CONTROL_SYMLINK           L"\\DosDevices\\Global\\MicLinkControl"

//
// 控制设备的 device type。必须与 miclink_ioctl.h 里的
// MICLINK_IOCTL_DEVICE_TYPE (FILE_DEVICE_UNKNOWN == 0x22) 保持一致，
// 否则 CTL_CODE 算出来的控制码与驱动期望的不匹配。
//
#define MICLINK_CONTROL_DEVICE_TYPE       FILE_DEVICE_UNKNOWN

//
// 端点只支持 16-bit 单声道，因此一帧恒为 2 字节。
// 驱动侧禁止重采样：44.1k 与 48k 只是"帧的消耗速率"不同，帧格式完全相同。
//
#define MICLINK_FRAME_BYTES               2u

// 环形缓冲帧容量（必须是 2 的幂）。32768 帧 * 2 字节 = 64 KiB，
// 48 kHz 下约 0.68 s、44.1 kHz 下约 0.74 s 的抗抖动深度。
#define MICLINK_RING_CAPACITY_FRAMES      (1u << 15)

// 单次 WRITE_PCM 允许的最大帧数 / 字节数（与 MICLINK_CAPS 上报一致）
#define MICLINK_MAX_FRAMES_PER_IOCTL      (MICLINK_RING_CAPACITY_FRAMES / 2u)
#define MICLINK_MAX_FRAME_BYTES_PER_IOCTL (MICLINK_MAX_FRAMES_PER_IOCTL * MICLINK_FRAME_BYTES)

// 默认注入格式：48 kHz / 单声道 / 16-bit
#define MICLINK_DEFAULT_SAMPLE_RATE       48000u
#define MICLINK_DEFAULT_CHANNELS          1u
#define MICLINK_DEFAULT_BITS_PER_SAMPLE   16u

// 支持的采样率（与 MICLINK_CAPS.supportedRates 一致）
#define MICLINK_SUPPORTED_RATE_COUNT      2u

// 控制设备安全描述符：SYSTEM / Administrators 完全控制，Everyone 读写执行。
// 服务端通常以普通用户权限运行，必须能 CreateFile(GENERIC_WRITE)。
#define MICLINK_CONTROL_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;WD)"

//
// {7C2E5E71-9E0A-4F5E-9E5B-3A2E5D8C1B40} —— 控制设备的 device class GUID
// （IoCreateDeviceSecure 需要；不注册设备接口，仅用于安全描述符归类）
//
// 定义放在 miclinkctl.cpp，不放头文件：这里若写 `static const GUID ...`，
// 每个包含本头但不使用它的 TU 都会持有一份未使用的文件作用域常量，
// 在 /W4 + TreatWarningAsError 下属于可避免的噪音。
//
extern const GUID MICLINK_CONTROL_CLASS_GUID;

//=============================================================================
// 控制设备扩展
//=============================================================================
typedef struct _MICLINK_CONTROL_EXT
{
    //
    // 必须是第一个字段：两段式判定的第二段直接解引用 offset 0 处的 ULONG。
    //
    ULONG                   Signature;

    PDEVICE_OBJECT          Self;

    // 打开的句柄计数（PASSIVE 侧维护，用于卸载时等待归零）
    volatile LONG           OpenHandleCount;

    //
    // 是否把环形缓冲里的数据交给 DMA（START=1 / STOP=0）。
    // 消费端在 DPC 中只读这一个字段。
    //
    volatile LONG           Injecting;

    // 用户态 SET_FORMAT / START 指定的注入格式
    MICLINK_FORMAT            InjectFormat;

    // miniport 捕获流实际协商到的格式（驱动不做重采样，两者必须相等）
    MICLINK_FORMAT            StreamFormat;

    // 是否有捕获流处于打开状态（CMiniportWaveRTStream::Init / ~ 中维护）
    volatile LONG           StreamOpen;

    //
    // SPSC 环形缓冲：生产者 = WRITE_PCM（PASSIVE），消费者 = DMA 通知 DPC。
    // Ring.buf 指向 RingStorage（NonPagedPool）。
    //
    miclink_ring              Ring;
    PVOID                   RingStorage;
    SIZE_T                  RingStorageBytes;

    //
    // 统计：写端独占写 FramesWritten / DroppedFrames / OverrunCount / RingHighWatermark，
    //       读端（DPC）独占写 FramesConsumed / UnderrunCount，另一端只读。
    // 计数一律用 LONG（而非 ULONG），以便直接与 InterlockedIncrement /
    // InterlockedExchange 的签名匹配，避免整段代码里到处做指针强转。
    //
    volatile LONG64         FramesWritten;
    volatile LONG64         FramesConsumed;
    volatile LONG           UnderrunCount;
    volatile LONG           OverrunCount;
    volatile LONG64         DroppedFrames;
    volatile LONG           RingHighWatermark;

    // 消费端写入 DMA 缓冲的最后一个样本（供 GET_STATUS 观察波形是否活着）
    volatile LONG           LastSample;

    // 环形缓冲是否已被填充过（RESET 会清零；消费端据此触发去咔哒淡入）
    volatile LONG           Primed;
} MICLINK_CONTROL_EXT, *PMICLINK_CONTROL_EXT;

//=============================================================================
// 全局声明
//=============================================================================

//
// 控制设备对象指针。NULL 表示尚未创建或已销毁。
// 消费端（DPC）与 dispatch 路由都只读这一个指针。
//
extern PDEVICE_OBJECT g_pMicLinkControlDevice;

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// 极简访问器：可被 DPC 在 DISPATCH_LEVEL 直接调用。
//
// 约束：只读一个全局指针 + 比较一个 magic，
//       不取锁、不发 IO、不分配内存、不访问分页内存。
//-----------------------------------------------------------------------------
_IRQL_requires_max_(DISPATCH_LEVEL)
PMICLINK_CONTROL_EXT
MicLinkGetControlExt
(
    void
);

//-----------------------------------------------------------------------------
// 生命周期
//-----------------------------------------------------------------------------
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MicLinkInitializeControlDevice
(
    _In_ PDRIVER_OBJECT DriverObject
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MicLinkCreateControlDevice
(
    _In_ PDRIVER_OBJECT DriverObject
);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
MicLinkDeleteControlDevice
(
    void
);

//-----------------------------------------------------------------------------
// 设备判定（两段式）
//-----------------------------------------------------------------------------
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
MicLinkIsControlDevice
(
    _In_ PDEVICE_OBJECT DeviceObject
);

//-----------------------------------------------------------------------------
// dispatch 例程（覆盖 PortCls 的 CREATE/CLOSE/CLEANUP/DEVICE_CONTROL）
//-----------------------------------------------------------------------------
DRIVER_DISPATCH MicLinkDispatchCreate;
DRIVER_DISPATCH MicLinkDispatchClose;
DRIVER_DISPATCH MicLinkDispatchCleanup;
DRIVER_DISPATCH MicLinkDispatchDeviceControl;

//-----------------------------------------------------------------------------
// miniport -> 控制设备的格式上报（驱动禁止重采样，靠它做一致性校验）
//-----------------------------------------------------------------------------
_IRQL_requires_max_(APC_LEVEL)
VOID
MicLinkNotifyStreamOpened
(
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample
);

_IRQL_requires_max_(APC_LEVEL)
VOID
MicLinkNotifyStreamClosed
(
    void
);

#ifdef __cplusplus
}
#endif

#endif // _MICLINK_CTL_H_
