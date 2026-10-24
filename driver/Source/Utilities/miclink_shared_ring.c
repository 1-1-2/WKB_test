/*++

Copyright (c) MicLink-open contributors. Licensed under the terms in NOTICE.md.

Module Name:

    miclink_shared_ring.c

Abstract:

    把三方共享的 SPSC 环形缓冲实现（shared/core/src/ring_buffer.c）接进 WDK 构建。

    【为什么需要一个外壳文件，而不是直接把 ring_buffer.c 加进工程】
    shared/core 的 miclink_atomic.h 用
        #if defined(_WIN32) && defined(_KERNEL_MODE)
    来选择内核分支（ReadAcquire / WriteRelease / MemoryBarrier）。
    若该宏未定义，它会掉进 `#elif defined(_WIN32)` 分支并 #include <windows.h>，
    在内核构建里直接炸掉。而 _KERNEL_MODE 到底由 WDK 的哪一层提供，
    不同 WDK 版本并不一致 —— 用 /D 命令行定义又有 C4005（宏重定义）风险。

    这里先 #include <wdm.h>（若 WDK 会定义 _KERNEL_MODE，就在此处定义完毕），
    再用 #ifndef 兜底。这样两种情形都成立且绝不产生重定义告警。

    【为什么整段 include 一个 .c】
    这样 shared/core 的源文件保持零改动、零副本，
    内核 / 用户态 / Linux 单测三方永远编译同一份实现。
--*/

#include <wdm.h>

#ifndef _KERNEL_MODE
#define _KERNEL_MODE 1
#endif

#include "..\..\..\shared\core\src\ring_buffer.c"
