/*++

Copyright (c) MicLink-open contributors. Licensed under the terms in NOTICE.md.

Module Name:

    miclink_shared_dsp.c

Abstract:

    把三方共享的 DSP 实现（shared/core/src/dsp.c）接进 WDK 构建。
    动机与做法完全同 miclink_shared_ring.c，见那个文件的说明。

    【内核侧使用边界】
    本驱动只会在 DISPATCH_LEVEL 的 DPC 里调用 dsp.c 中的两个纯整数函数：
        miclink_fill_silence()      —— 填静音
        miclink_apply_declck()      —— 去咔哒线性淡入
        miclink_declck_notify_loss()
    dsp.c 里另外两个用 float / double 的函数（miclink_apply_gain /
    miclink_measure_level）本驱动绝不调用 ——
    x64 内核默认不允许在内核态使用浮点上下文，尤其在 DPC 中。
    它们被链接进来只是因为同一个编译单元，不会被本驱动的任何代码路径触达。
--*/

#include <wdm.h>

#ifndef _KERNEL_MODE
#define _KERNEL_MODE 1
#endif

#include "..\..\..\shared\core\src\dsp.c"
