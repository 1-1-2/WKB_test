/*++

Copyright (c) Womic-open contributors. Licensed under the terms in NOTICE.md.

Module Name:

    womicctl.cpp

Abstract:

    WOMIC 控制设备（\\.\WomicControl）的实现：

      * 在 PcInitializeAdapterDriver 之后接管 IRP_MJ_CREATE / CLOSE / CLEANUP /
        DEVICE_CONTROL（顺序不可颠倒，详见 DriverEntry 里的注释）；
      * 创建一个 FILE_DEVICE_UNKNOWN 控制设备 + \DosDevices\Global\WomicControl
        符号链接，用户态用 CreateFile("\\\\.\\WomicControl") 打开；
      * IOCTL 生产侧：把用户态 PCM 拷进 NonPagedPool 里的 SPSC 环形缓冲后立刻返回，
        绝不挂起 IRP 等待空间（消费端在 DISPATCH_LEVEL 的 DPC 中，等不起）；
      * 消费侧由 CMiniportWaveRTStream::WriteBytes（DPC）通过
        WomicGetControlExt() 直接取 ring。

    设计约束回顾（坑 3 / N-2）：
      消费路径跑在 DISPATCH_LEVEL 且持有自旋锁，因此本文件的"读路径辅助函数"
      （WomicGetControlExt）不得有任何阻塞 / 分配 / 取锁 / IO。
      生产路径（本文件的 IOCTL 处理）跑在 PASSIVE_LEVEL，无此限制，
      但仍然选择"拷完就返回"，把背压交给用户态重试。

--*/

#pragma warning (disable : 4127)   // conditional expression is constant
#pragma warning (disable : 4201)   // nameless struct/union (GUID)

#include "definitions.h"

//
// IoCreateDeviceSecure 的声明与导入库（wdmsec.lib 已在 Main.vcxproj 中链接）。
//
#include <wdmsec.h>

#include "womic/womic_ctl.h"

//=============================================================================
// 全局状态
//=============================================================================

//
// 控制设备对象。NULL = 未创建 / 已销毁。
//
PDEVICE_OBJECT g_pWomicControlDevice = NULL;

//
// 控制设备的 device class GUID（见 womic_ctl.h 中的声明）。
// 定义放在这里，头文件只声明，避免每个 TU 各持一份未使用的常量。
//
const GUID WOMIC_CONTROL_CLASS_GUID =
{
    0x7c2e5e71, 0x9e0a, 0x4f5e, { 0x9e, 0x5b, 0x3a, 0x2e, 0x5d, 0x8c, 0x1b, 0x40 }
};

//
// PortCls 在被我们覆盖之前的 MajorFunction。
// 注意：转发时我们用的是 PcDispatchIrp()（PortCls 官方支持的"拦截器回投"API），
// 而不是直接调用这里保存的裸指针 —— 原因见 WomicDispatchCreate 上方的注释。
// 保存它们主要用于诊断与回归比对（VerifyCheckPcDispatchSaved()）。
//
static PDRIVER_DISPATCH g_PcSavedCreate         = NULL;
static PDRIVER_DISPATCH g_PcSavedClose          = NULL;
static PDRIVER_DISPATCH g_PcSavedCleanup        = NULL;
static PDRIVER_DISPATCH g_PcSavedDeviceControl  = NULL;

//
// 卸载时等待句柄归零的最大轮询次数（每次 10 ms，上限 2 s）。
// 兜底存在，保证卸载路径绝不无限期挂死。
//
#define WOMIC_UNLOAD_WAIT_MAX_SPINS  200u
#define WOMIC_UNLOAD_WAIT_100NS      (-10LL * 10000LL)   // 10 ms，单位 100ns

//=============================================================================
// 内部工具
//=============================================================================

//
// 下面三个格式工具函数刻意放在非分页段：它们被 DEVICE_CONTROL dispatch 调用，
// 而 dispatch 例程本身是非分页的（不允许从非分页代码调用 PAGE 段函数）。
//
#pragma code_seg()
static
BOOLEAN
WomicIsSupportedFormat
(
    _In_ const WOMIC_FORMAT *Format
)
/*++

Routine Description:

    校验一个格式是否是本端点支持的"16-bit 单声道 44.1k / 48k"。
    驱动侧不做任何重采样，因此任何其它组合一律拒绝。

--*/
{
    if (Format == NULL)
    {
        return FALSE;
    }

    if ((Format->channels != WOMIC_DEFAULT_CHANNELS) ||
        (Format->bitsPerSample != WOMIC_DEFAULT_BITS_PER_SAMPLE))
    {
        return FALSE;
    }

    if ((Format->sampleRate != 44100u) && (Format->sampleRate != 48000u))
    {
        return FALSE;
    }

    return TRUE;
}

#pragma code_seg()
static
BOOLEAN
WomicFormatEquals
(
    _In_ const WOMIC_FORMAT *A,
    _In_ const WOMIC_FORMAT *B
)
{
    if ((A == NULL) || (B == NULL))
    {
        return FALSE;
    }

    return (A->sampleRate == B->sampleRate) &&
           (A->channels == B->channels) &&
           (A->bitsPerSample == B->bitsPerSample);
}

#pragma code_seg()
static
ULONG
WomicFrameBytesOf
(
    _In_ const WOMIC_FORMAT *Format
)
{
    if (Format == NULL)
    {
        return 0u;
    }

    return (Format->channels * Format->bitsPerSample) / 8u;
}

#pragma code_seg()
static
NTSTATUS
WomicCompleteIrp
(
    _In_    PIRP        Irp,
    _In_    NTSTATUS    Status,
    _In_    ULONG_PTR   Information
)
{
    Irp->IoStatus.Status      = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

//=============================================================================
// 极简访问器（DPC 安全）
//=============================================================================

#pragma code_seg()
PWOMIC_CONTROL_EXT
WomicGetControlExt
(
    void
)
/*++

Routine Description:

    返回控制设备扩展；若控制设备不存在或尚未就绪则返回 NULL。

    本函数会被 miniport 的 DMA 通知 DPC 在 DISPATCH_LEVEL 且持有自旋锁时调用，
    因此这里只允许：读一个全局指针 + 比较一个 magic。
    不得取锁、不得分配、不得访问分页内存、不得发 IO。

--*/
{
    PDEVICE_OBJECT      deviceObject;
    PWOMIC_CONTROL_EXT  ext;

    deviceObject = g_pWomicControlDevice;

    if (deviceObject == NULL)
    {
        return NULL;
    }

    ext = (PWOMIC_CONTROL_EXT)deviceObject->DeviceExtension;

    if (ext == NULL)
    {
        return NULL;
    }

    if (ext->Signature != WOMIC_CONTROL_SIGNATURE)
    {
        return NULL;
    }

    return ext;
}

#pragma code_seg()
BOOLEAN
WomicIsControlDevice
(
    _In_ PDEVICE_OBJECT DeviceObject
)
/*++

Routine Description:

    两段式判定目标是"我们的控制设备"还是"PortCls 的音频设备"。

    第一段（指针快拒）：先和全局保存的控制设备指针做等值比较。
        对任何陌生设备，我们连它的 DeviceExtension 都不去读 ——
        因为陌生设备可能是 PortCls FDO、也可能是别的驱动的设备，
        读它的 extension 属于越界访问。
    第二段（magic 终裁）：指针相等之后仍然要验签。
        因为设备对象可能已经被 IoDeleteDevice 释放、内存被复用，
        此时指针比较会"巧合地"相等，只有 magic 能给出确定答案。

--*/
{
    PWOMIC_CONTROL_EXT ext;

    if (DeviceObject == NULL)
    {
        return FALSE;
    }

    //
    // 第一段：指针快拒，不读未知设备的 extension。
    //
    if (DeviceObject != g_pWomicControlDevice)
    {
        return FALSE;
    }

    //
    // 第二段：magic 终裁。
    //
    ext = (PWOMIC_CONTROL_EXT)DeviceObject->DeviceExtension;

    if (ext == NULL)
    {
        return FALSE;
    }

    if (ext->Signature != WOMIC_CONTROL_SIGNATURE)
    {
        return FALSE;
    }

    return TRUE;
}

//=============================================================================
// 控制设备生命周期
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
WomicCreateControlDevice
(
    _In_ PDRIVER_OBJECT DriverObject
)
/*++

Routine Description:

    创建 \Device\WomicControl + \DosDevices\Global\WomicControl，
    并分配 NonPagedPool 环形缓冲存储。

--*/
{
    PAGED_CODE();

    NTSTATUS            ntStatus;
    UNICODE_STRING      ntName;
    UNICODE_STRING      symLink;
    UNICODE_STRING      sddl;
    PDEVICE_OBJECT      deviceObject = NULL;
    PWOMIC_CONTROL_EXT  ext          = NULL;
    PVOID               storage      = NULL;
    SIZE_T              storageBytes = 0;

    if (g_pWomicControlDevice != NULL)
    {
        //
        // 已经创建过（理论上不会），直接成功返回。
        //
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&ntName, WOMIC_CONTROL_NT_NAME);
    RtlInitUnicodeString(&symLink, WOMIC_CONTROL_SYMLINK);
    RtlInitUnicodeString(&sddl, WOMIC_CONTROL_SDDL);

    storageBytes = (SIZE_T)WOMIC_RING_CAPACITY_FRAMES * (SIZE_T)WOMIC_FRAME_BYTES;

    storage = ExAllocatePool2(POOL_FLAG_NON_PAGED, storageBytes, WOMIC_POOLTAG);
    if (storage == NULL)
    {
        DPF(D_ERROR, ("[Womic] ring storage alloc failed (%Iu bytes)", storageBytes));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(storage, storageBytes);

    ntStatus = IoCreateDeviceSecure(
        DriverObject,
        sizeof(WOMIC_CONTROL_EXT),
        &ntName,
        WOMIC_CONTROL_DEVICE_TYPE,
        0,                      // DeviceCharacteristics
        FALSE,                  // 非独占：允许多个句柄
        &sddl,
        (LPCGUID)&WOMIC_CONTROL_CLASS_GUID,
        &deviceObject);

    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("[Womic] IoCreateDeviceSecure failed 0x%x", ntStatus));
        ExFreePoolWithTag(storage, WOMIC_POOLTAG);
        return ntStatus;
    }

    ext = (PWOMIC_CONTROL_EXT)deviceObject->DeviceExtension;
    RtlZeroMemory(ext, sizeof(WOMIC_CONTROL_EXT));

    ext->Signature       = WOMIC_CONTROL_SIGNATURE;
    ext->Self            = deviceObject;
    ext->OpenHandleCount = 0;
    ext->Injecting       = 0;
    ext->StreamOpen      = 0;
    ext->Primed          = 0;
    ext->LastSample      = 0;

    ext->InjectFormat.sampleRate    = WOMIC_DEFAULT_SAMPLE_RATE;
    ext->InjectFormat.channels      = WOMIC_DEFAULT_CHANNELS;
    ext->InjectFormat.bitsPerSample = WOMIC_DEFAULT_BITS_PER_SAMPLE;

    ext->StreamFormat.sampleRate    = 0;
    ext->StreamFormat.channels      = 0;
    ext->StreamFormat.bitsPerSample = 0;

    ext->RingStorage      = storage;
    ext->RingStorageBytes = storageBytes;

    if (womic_ring_init(&ext->Ring,
                        (int16_t *)storage,
                        WOMIC_RING_CAPACITY_FRAMES,
                        WOMIC_FRAME_BYTES) != WOMIC_RING_OK)
    {
        DPF(D_ERROR, ("[Womic] womic_ring_init failed"));
        ExFreePoolWithTag(storage, WOMIC_POOLTAG);
        ext->RingStorage = NULL;
        IoDeleteDevice(deviceObject);
        return STATUS_UNSUCCESSFUL;
    }

    //
    // 必须手工清 DO_DEVICE_INITIALIZING：
    // IoCreateDevice* 创建的设备默认带这个标志，会阻止任何打开。
    // PortCls 自己的设备在 PcAddAdapterDevice 里清，我们的控制设备得自己清。
    //
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    ntStatus = IoCreateSymbolicLink(&symLink, &ntName);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("[Womic] IoCreateSymbolicLink failed 0x%x", ntStatus));
        ext->Ring.buf = NULL;
        ExFreePoolWithTag(storage, WOMIC_POOLTAG);
        ext->RingStorage = NULL;
        ext->Signature = 0;
        IoDeleteDevice(deviceObject);
        return ntStatus;
    }

    //
    // 发布到全局。必须先写满 ext 再发布，并插入写屏障，
    // 保证 DPC 侧一旦看到非 NULL 指针，就一定能看到完整初始化的扩展。
    //
    MemoryBarrier();
    g_pWomicControlDevice = deviceObject;
    MemoryBarrier();

    DPF(D_TERSE, ("[Womic] control device created: %p, ring %u frames",
                  deviceObject, (ULONG)WOMIC_RING_CAPACITY_FRAMES));

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID
WomicDeleteControlDevice
(
    void
)
/*++

Routine Description:

    销毁控制设备。顺序是硬性要求：
        1) 删符号链接（阻断新的 CreateFile）
        2) 等已有句柄归零
        3) 断开全局指针（让 DPC 降级为静音输出）
        4) 释放环形缓冲存储
        5) IoDeleteDevice

--*/
{
    PAGED_CODE();

    PDEVICE_OBJECT      deviceObject;
    PWOMIC_CONTROL_EXT  ext;
    UNICODE_STRING      symLink;
    ULONG               spins = 0;

    deviceObject = g_pWomicControlDevice;

    if (deviceObject == NULL)
    {
        return;
    }

    ext = (PWOMIC_CONTROL_EXT)deviceObject->DeviceExtension;

    //
    // 1) 删符号链接，这一步之后用户态再也打不开 \\.\WomicControl。
    //
    RtlInitUnicodeString(&symLink, WOMIC_CONTROL_SYMLINK);
    IoDeleteSymbolicLink(&symLink);

    //
    // 2) 等句柄归零。卸载路径在 PASSIVE_LEVEL，可以睡眠等待。
    //    带上限，绝不无限期挂死。
    //
    while ((InterlockedCompareExchange(&ext->OpenHandleCount, 0, 0) != 0) &&
           (spins < WOMIC_UNLOAD_WAIT_MAX_SPINS))
    {
        LARGE_INTEGER interval;

        interval.QuadPart = WOMIC_UNLOAD_WAIT_100NS;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        spins++;
    }

    if (InterlockedCompareExchange(&ext->OpenHandleCount, 0, 0) != 0)
    {
        DPF(D_ERROR, ("[Womic] unload with %d handle(s) still open",
                      InterlockedCompareExchange(&ext->OpenHandleCount, 0, 0)));
    }

    //
    // 3) 先断开全局指针。此后进入的 DPC 一律看到 NULL，直接输出静音，
    //    不会再触碰即将被释放的存储。
    //
    g_pWomicControlDevice = NULL;
    MemoryBarrier();

    //
    // 4) 释放环形缓冲存储。
    //    残余竞态：理论上存在一个 DPC 已经读过 g_pWomicControlDevice 但还没读 buf 的
    //    极小窗口。为把危害降到最低，先把 buf 置 NULL —— womic_ring_read* 遇到
    //    buf==NULL 会返回 0，消费端随即退化为"补静音"，不会解引用已释放内存。
    //
    if (ext->RingStorage != NULL)
    {
        ext->Ring.buf = NULL;
        MemoryBarrier();
        ExFreePoolWithTag(ext->RingStorage, WOMIC_POOLTAG);
        ext->RingStorage      = NULL;
        ext->RingStorageBytes = 0;
    }

    ext->Injecting = 0;
    ext->Signature = 0;

    //
    // 5) 销毁设备对象。
    //
    IoDeleteDevice(deviceObject);

    DPF(D_TERSE, ("[Womic] control device deleted"));
}

#pragma code_seg("PAGE")
NTSTATUS
WomicInitializeControlDevice
(
    _In_ PDRIVER_OBJECT DriverObject
)
/*++

Routine Description:

    保存 PortCls 的 4 个 dispatch 例程 -> 覆盖为我们自己的 -> 创建控制设备。

    调用时机必须是 PcInitializeAdapterDriver 之后：
    PcInitializeAdapterDriver 会整体重写 DriverObject->MajorFunction 表，
    在它之前做的任何覆盖都会被静默抹掉。官方示例自己覆盖 IRP_MJ_PNP 用的
    正是这个顺序（adapter.cpp:341），我们照抄这个已验证的模式。

--*/
{
    PAGED_CODE();

    NTSTATUS ntStatus;

    if (DriverObject == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // 先保存（用于诊断；转发统一走 PcDispatchIrp）。
    //
    g_PcSavedCreate        = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_PcSavedClose         = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_PcSavedCleanup       = DriverObject->MajorFunction[IRP_MJ_CLEANUP];
    g_PcSavedDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];

    DPF(D_VERBOSE, ("[Womic] saved PortCls dispatch: CREATE=%p CLOSE=%p CLEANUP=%p IOCTL=%p",
                    g_PcSavedCreate, g_PcSavedClose,
                    g_PcSavedCleanup, g_PcSavedDeviceControl));

    //
    // 再覆盖。
    //
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = WomicDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = WomicDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = WomicDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WomicDispatchDeviceControl;

    ntStatus = WomicCreateControlDevice(DriverObject);

    return ntStatus;
}

//=============================================================================
// dispatch 例程
//
// 转发的正确做法（这是本驱动最容易做错的地方）：
//   对"不是控制设备"的目标，必须用 PcDispatchIrp(DeviceObject, Irp) 原样回投给
//   PortCls，不能吞掉 IRP（例如直接返回 SUCCESS 或返回错误）。
//   若吞掉，PortCls 就永远收不到 create / device-control，filter 与 pin 对象不会
//   被实例化，AudioEndpointBuilder 不会枚举出任何端点 ——
//   表现就是"CreateFile 成功，但声音设置里看不到麦克风"。
//   PcDispatchIrp 是 PortCls 官方支持的"拦截器回投"API，本示例自己在
//   adapter.cpp:839 就是这么用的。
//=============================================================================

#pragma code_seg()
_Dispatch_type_(IRP_MJ_CREATE)
NTSTATUS
WomicDispatchCreate
(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
)
{
    PWOMIC_CONTROL_EXT ext;

    if (!WomicIsControlDevice(DeviceObject))
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    ext = (PWOMIC_CONTROL_EXT)DeviceObject->DeviceExtension;

    InterlockedIncrement(&ext->OpenHandleCount);

    return WomicCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

#pragma code_seg()
_Dispatch_type_(IRP_MJ_CLEANUP)
NTSTATUS
WomicDispatchCleanup
(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
)
{
    PWOMIC_CONTROL_EXT ext;

    if (!WomicIsControlDevice(DeviceObject))
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    ext = (PWOMIC_CONTROL_EXT)DeviceObject->DeviceExtension;

    //
    // CLEANUP 在最后一个句柄关闭时触发；与 CREATE 的递增严格配对。
    // 用 InterlockedDecrement 后夹到 >= 0，防止异常情况把计数打穿。
    //
    if (InterlockedDecrement(&ext->OpenHandleCount) < 0)
    {
        InterlockedExchange(&ext->OpenHandleCount, 0);
    }

    return WomicCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

#pragma code_seg()
_Dispatch_type_(IRP_MJ_CLOSE)
NTSTATUS
WomicDispatchClose
(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
)
{
    if (!WomicIsControlDevice(DeviceObject))
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    //
    // 句柄计数在 CLEANUP 里已经减过，这里只负责完成 IRP。
    //
    return WomicCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

#pragma code_seg()
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
NTSTATUS
WomicDispatchDeviceControl
(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
)
{
    PIO_STACK_LOCATION  stack;
    PWOMIC_CONTROL_EXT  ext;
    PVOID               buffer;
    ULONG               ioctl;
    ULONG               inputLength;
    ULONG               outputLength;
    NTSTATUS            ntStatus        = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR           information     = 0;

    if (!WomicIsControlDevice(DeviceObject))
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    ext         = (PWOMIC_CONTROL_EXT)DeviceObject->DeviceExtension;
    stack       = IoGetCurrentIrpStackLocation(Irp);
    ioctl       = stack->Parameters.DeviceIoControl.IoControlCode;
    inputLength  = stack->Parameters.DeviceIoControl.InputBufferLength;
    outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

    //
    // METHOD_BUFFERED：输入与输出共用 Irp->AssociatedIrp.SystemBuffer。
    //
    buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (ioctl)
    {
    case WOMIC_IOCTL_GET_VERSION:
    {
        WOMIC_VERSION version;

        if (outputLength < sizeof(WOMIC_VERSION))
        {
            ntStatus    = STATUS_BUFFER_TOO_SMALL;
            information = sizeof(WOMIC_VERSION);
            break;
        }

        version.major = 1;
        version.minor = 0;
        version.patch = 0;
        version.build = 1;

        RtlCopyMemory(buffer, &version, sizeof(version));
        ntStatus    = STATUS_SUCCESS;
        information = sizeof(version);
        break;
    }

    case WOMIC_IOCTL_GET_CAPS:
    {
        WOMIC_CAPS caps;

        if (outputLength < sizeof(WOMIC_CAPS))
        {
            ntStatus    = STATUS_BUFFER_TOO_SMALL;
            information = sizeof(WOMIC_CAPS);
            break;
        }

        RtlZeroMemory(&caps, sizeof(caps));
        caps.ringCapacityFrames    = WOMIC_RING_CAPACITY_FRAMES;
        caps.supportedRates[0]     = 48000u;
        caps.supportedRates[1]     = 44100u;
        caps.supportedRates[2]     = 0u;    // 0 = 列表结尾
        caps.supportedChannels     = 0x1u;  // bit0 = 1ch
        caps.maxFrameBytesPerIoctl = WOMIC_MAX_FRAME_BYTES_PER_IOCTL;
        caps.supportsSilenceFill   = 1u;    // 驱动侧恒定补静音（DPC 无法返回 NO_STREAM）

        RtlCopyMemory(buffer, &caps, sizeof(caps));
        ntStatus    = STATUS_SUCCESS;
        information = sizeof(caps);
        break;
    }

    case WOMIC_IOCTL_GET_FORMAT:
    {
        if (outputLength < sizeof(WOMIC_FORMAT))
        {
            ntStatus    = STATUS_BUFFER_TOO_SMALL;
            information = sizeof(WOMIC_FORMAT);
            break;
        }

        RtlCopyMemory(buffer, &ext->InjectFormat, sizeof(WOMIC_FORMAT));
        ntStatus    = STATUS_SUCCESS;
        information = sizeof(WOMIC_FORMAT);
        break;
    }

    case WOMIC_IOCTL_SET_FORMAT:
    {
        WOMIC_FORMAT requested;

        if ((buffer == NULL) || (inputLength < sizeof(WOMIC_FORMAT)))
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        RtlCopyMemory(&requested, buffer, sizeof(requested));

        if (!WomicIsSupportedFormat(&requested))
        {
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;   // WOMIC_ERR_FORMAT_MISMATCH
            break;
        }

        //
        // 驱动侧禁止重采样：若有捕获流正在运行，注入格式必须与流的格式完全一致。
        //
        if ((ext->StreamOpen != 0) &&
            (ext->StreamFormat.sampleRate != 0) &&
            !WomicFormatEquals(&requested, &ext->StreamFormat))
        {
            DPF(D_ERROR, ("[Womic] SET_FORMAT %u/%u/%u rejected: stream is %u/%u/%u",
                          requested.sampleRate, requested.channels, requested.bitsPerSample,
                          ext->StreamFormat.sampleRate, ext->StreamFormat.channels,
                          ext->StreamFormat.bitsPerSample));
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;   // WOMIC_ERR_FORMAT_MISMATCH
            break;
        }

        //
        // 采样率变化时清空缓冲：旧采样率下的残留帧若被新速率消费，
        // 时长与音高都会错乱。
        //
        if (requested.sampleRate != ext->InjectFormat.sampleRate)
        {
            womic_ring_reset(&ext->Ring);
            ext->Primed = 0;
        }

        ext->InjectFormat = requested;

        ntStatus    = STATUS_SUCCESS;
        information = 0;
        break;
    }

    case WOMIC_IOCTL_WRITE_PCM:
    {
        WOMIC_WRITE_PCM_IO   header;
        ULONG                frameBytes;
        ULONGLONG            payloadBytes;
        ULONG                freeFrames;
        ULONG                written;

        if ((buffer == NULL) || (inputLength < sizeof(WOMIC_WRITE_PCM_IO)))
        {
            ntStatus = STATUS_INVALID_PARAMETER;   // WOMIC_ERR_INVALID_PARAM
            break;
        }

        RtlCopyMemory(&header, buffer, sizeof(header));

        if (header.reserved != 0)
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        if (header.frameCount == 0)
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        if (header.frameCount > WOMIC_MAX_FRAMES_PER_IOCTL)
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // 必须先 SET_FORMAT（或 START）确定格式，否则无从校验。
        //
        if (ext->InjectFormat.sampleRate == 0)
        {
            ntStatus = STATUS_DEVICE_NOT_READY;    // WOMIC_ERR_NOT_READY
            break;
        }

        frameBytes = WomicFrameBytesOf(&ext->InjectFormat);

        //
        // 格式不一致 -> 直接拒绝。内核绝不做 SRC。
        //
        if (header.frameBytes != frameBytes)
        {
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;   // WOMIC_ERR_FORMAT_MISMATCH
            break;
        }

        if ((ext->StreamOpen != 0) &&
            (ext->StreamFormat.sampleRate != 0) &&
            !WomicFormatEquals(&ext->InjectFormat, &ext->StreamFormat))
        {
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;   // WOMIC_ERR_FORMAT_MISMATCH
            break;
        }

        payloadBytes = (ULONGLONG)header.frameCount * (ULONGLONG)header.frameBytes;

        if ((ULONGLONG)inputLength < (ULONGLONG)sizeof(WOMIC_WRITE_PCM_IO) + payloadBytes)
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // 全有或全无（all-or-nothing）：
        // 装不下就一帧都不写，返回 STATUS_BUFFER_TOO_SMALL（WOMIC_ERR_BUFFER_FULL）。
        // 之所以不"能写多少写多少"，是因为部分写 + 错误状态会让用户态无法判断
        // 该从哪个偏移重试，反而更容易写重；全有或全无让重试语义完全确定。
        //
        // 绝不在这里挂起 IRP 等待空间：消费端在 DISPATCH_LEVEL 的 DPC 里，
        // 生产者等待 == 死锁风险（坑 3 / N-2）。
        //
        freeFrames = womic_ring_free(&ext->Ring);

        if (header.frameCount > freeFrames)
        {
            InterlockedExchangeAdd64(&ext->DroppedFrames, (LONG64)header.frameCount);
            InterlockedIncrement(&ext->OverrunCount);

            ntStatus    = STATUS_BUFFER_TOO_SMALL;   // WOMIC_ERR_BUFFER_FULL
            information = 0;
            break;
        }

        written = womic_ring_write(
            &ext->Ring,
            (const int16_t *)((PUCHAR)buffer + sizeof(WOMIC_WRITE_PCM_IO)),
            header.frameCount);

        if (written != header.frameCount)
        {
            //
            // 理论上不会走到这里（freeFrames 已经校验过）。
            // 兜底记账，保证统计自洽。
            //
            InterlockedExchangeAdd64(&ext->DroppedFrames,
                                     (LONG64)(header.frameCount - written));
            InterlockedIncrement(&ext->OverrunCount);

            ntStatus    = STATUS_BUFFER_TOO_SMALL;
            information = (ULONG_PTR)written;
            break;
        }

        InterlockedExchangeAdd64(&ext->FramesWritten, (LONG64)written);

        //
        // 高水位（生产者独占写）。
        //
        {
            ULONG available = womic_ring_available(&ext->Ring);

            if (available > (ULONG)ext->RingHighWatermark)
            {
                ext->RingHighWatermark = (LONG)available;
            }
        }

        if (ext->Primed == 0)
        {
            ext->Primed = 1;
        }

        ntStatus    = STATUS_SUCCESS;
        information = (ULONG_PTR)written;
        break;
    }

    case WOMIC_IOCTL_GET_STATUS:
    {
        WOMIC_STATUS status;

        if (outputLength < sizeof(WOMIC_STATUS))
        {
            ntStatus    = STATUS_BUFFER_TOO_SMALL;
            information = sizeof(WOMIC_STATUS);
            break;
        }

        RtlZeroMemory(&status, sizeof(status));

        //
        // 显式转换到共享结构体的定宽类型，避免 LONG -> uint32_t 的
        // signed/unsigned 不匹配告警（本工程开了 /W4 + TreatWarningAsError）。
        //
        status.state = (ext->Injecting != 0)
            ? ((ext->StreamOpen != 0) ? WOMIC_STATE_RUNNING : WOMIC_STATE_STARTED)
            : WOMIC_STATE_STOPPED;

        status.ringAvailableFrames = womic_ring_available(&ext->Ring);
        status.ringFreeFrames      = womic_ring_free(&ext->Ring);
        status.underrunCount       = (uint32_t)ext->UnderrunCount;
        status.framesWritten       = (uint64_t)ext->FramesWritten;
        status.framesConsumed      = (uint64_t)ext->FramesConsumed;
        status.droppedFrames       = (uint64_t)ext->DroppedFrames;
        status.overrunCount        = (uint32_t)ext->OverrunCount;
        status.ringHighWatermark   = (uint32_t)ext->RingHighWatermark;
        status.currentFormat       = (ext->StreamOpen != 0) ? ext->StreamFormat
                                                            : ext->InjectFormat;

        RtlCopyMemory(buffer, &status, sizeof(status));
        ntStatus    = STATUS_SUCCESS;
        information = sizeof(status);
        break;
    }

    case WOMIC_IOCTL_RESET:
    {
        WOMIC_RESET_IO reset;

        RtlZeroMemory(&reset, sizeof(reset));

        if ((buffer != NULL) && (inputLength >= sizeof(WOMIC_RESET_IO)))
        {
            RtlCopyMemory(&reset, buffer, sizeof(reset));
        }

        womic_ring_reset(&ext->Ring);
        ext->Primed = 0;                 // 消费端据此触发去咔哒淡入

        if (reset.keepFormat == 0)
        {
            ext->InjectFormat.sampleRate    = WOMIC_DEFAULT_SAMPLE_RATE;
            ext->InjectFormat.channels      = WOMIC_DEFAULT_CHANNELS;
            ext->InjectFormat.bitsPerSample = WOMIC_DEFAULT_BITS_PER_SAMPLE;
        }

        if (reset.clearStats != 0)
        {
            InterlockedExchange64(&ext->FramesWritten, 0);
            InterlockedExchange64(&ext->FramesConsumed, 0);
            InterlockedExchange64(&ext->DroppedFrames, 0);
            InterlockedExchange(&ext->UnderrunCount, 0);
            InterlockedExchange(&ext->OverrunCount, 0);
            InterlockedExchange(&ext->RingHighWatermark, 0);
        }

        ntStatus    = STATUS_SUCCESS;
        information = 0;
        break;
    }

    case WOMIC_IOCTL_START:
    {
        WOMIC_START_IO  start;
        WOMIC_FORMAT    requested;

        RtlZeroMemory(&start, sizeof(start));

        if ((buffer != NULL) && (inputLength >= sizeof(WOMIC_START_IO)))
        {
            RtlCopyMemory(&start, buffer, sizeof(start));
        }

        requested.sampleRate = (start.sampleRate != 0)
            ? start.sampleRate
            : ((ext->InjectFormat.sampleRate != 0)
                ? ext->InjectFormat.sampleRate
                : ((ext->StreamOpen != 0)
                    ? ext->StreamFormat.sampleRate
                    : WOMIC_DEFAULT_SAMPLE_RATE));

        requested.channels = (start.channels != 0)
            ? start.channels
            : ((ext->InjectFormat.channels != 0)
                ? ext->InjectFormat.channels
                : WOMIC_DEFAULT_CHANNELS);

        requested.bitsPerSample = WOMIC_DEFAULT_BITS_PER_SAMPLE;

        if (!WomicIsSupportedFormat(&requested))
        {
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;   // WOMIC_ERR_FORMAT_MISMATCH
            break;
        }

        //
        // 流已打开时，注入格式必须与流的格式一致（禁止重采样）。
        //
        if ((ext->StreamOpen != 0) &&
            (ext->StreamFormat.sampleRate != 0) &&
            !WomicFormatEquals(&requested, &ext->StreamFormat))
        {
            DPF(D_ERROR, ("[Womic] START %u rejected: capture stream runs at %u",
                          requested.sampleRate, ext->StreamFormat.sampleRate));
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        if (requested.sampleRate != ext->InjectFormat.sampleRate)
        {
            womic_ring_reset(&ext->Ring);
            ext->Primed = 0;
        }

        ext->InjectFormat = requested;
        ext->Injecting    = 1;

        //
        // 注意：WOMIC_START_IO.fillSilence 在本驱动里恒按 1 处理。
        // 消费端跑在 DPC 中，DMA 缓冲必须被写满，没有"返回 NO_STREAM"的位置；
        // 缓冲排空时一律补静音 + 去咔哒（见 CMiniportWaveRTStream::WriteBytes）。
        //

        ntStatus    = STATUS_SUCCESS;
        information = 0;
        break;
    }

    case WOMIC_IOCTL_STOP:
    {
        ext->Injecting = 0;

        ntStatus    = STATUS_SUCCESS;
        information = 0;
        break;
    }

    default:
        DPF(D_VERBOSE, ("[Womic] unsupported IOCTL 0x%x", ioctl));
        ntStatus    = STATUS_INVALID_DEVICE_REQUEST;
        information = 0;
        break;
    }

    return WomicCompleteIrp(Irp, ntStatus, information);
}

//=============================================================================
// miniport -> 控制设备的格式上报
//=============================================================================

#pragma code_seg()
VOID
WomicNotifyStreamOpened
(
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample
)
/*++

Routine Description:

    捕获流创建时上报其协商到的格式。驱动禁止重采样，
    后续 SET_FORMAT / START / WRITE_PCM 都要与它比对。

--*/
{
    PWOMIC_CONTROL_EXT ext = WomicGetControlExt();

    if (ext == NULL)
    {
        return;
    }

    ext->StreamFormat.sampleRate    = SampleRate;
    ext->StreamFormat.channels      = Channels;
    ext->StreamFormat.bitsPerSample = BitsPerSample;

    MemoryBarrier();
    ext->StreamOpen = 1;

    //
    // 关键：驱动不做任何重采样，所以"注入格式"必须以"捕获流实际运行的格式"为准。
    // 因此流一打开就用 StreamFormat 覆盖 InjectFormat，无论用户态之前 SET_FORMAT
    // 成什么 —— 否则会出现"用户按 48k 注入、端点按 44.1k 消费"的错乱。
    //
    // 这条同时让"先开录音机、后开服务端"这种顺序也能正常工作：
    // 服务端随后 GET_FORMAT 拿到的就是端点真正运行的格式。
    //
    if ((ext->InjectFormat.sampleRate != 0) &&
        !WomicFormatEquals(&ext->InjectFormat, &ext->StreamFormat))
    {
        //
        // 采样率变了：清空环形缓冲，避免旧采样率的残留帧被新速率消费
        // （那会让时长和音高一起错乱）。
        //
        womic_ring_reset(&ext->Ring);
        ext->Primed = 0;

        DPF(D_TERSE, ("[Womic] inject format realigned: %u -> %u Hz",
                      ext->InjectFormat.sampleRate, SampleRate));
    }

    ext->InjectFormat = ext->StreamFormat;

    DPF(D_TERSE, ("[Womic] capture stream opened: %u Hz, %u ch, %u bit",
                  SampleRate, Channels, BitsPerSample));
}

#pragma code_seg()
VOID
WomicNotifyStreamClosed
(
    void
)
{
    PWOMIC_CONTROL_EXT ext = WomicGetControlExt();

    if (ext == NULL)
    {
        return;
    }

    ext->StreamOpen = 0;

    ext->StreamFormat.sampleRate    = 0;
    ext->StreamFormat.channels      = 0;
    ext->StreamFormat.bitsPerSample = 0;

    DPF(D_TERSE, ("[Womic] capture stream closed"));
}
