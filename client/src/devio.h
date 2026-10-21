/* devio.h — \\.\WOMic 设备句柄与灌音协议（2022 驱动 2.1.0.0）
 *
 * 全部编号来自 docs/driver-whitepaper.md §4（静态反汇编 + 实测）。
 * 旧的 0x222000 是 2017 驱动的编号，在 2022 驱动上 51/51 返回
 * ERROR_INVALID_FUNCTION，已从代码库删除。
 *
 * ★ 三条硬约束（whitepaper §9，违反即「静悄悄失败」）：
 *   1. 全进程只 CreateFile 一次，句柄长期持有。每次 CreateFile 都会把驱动的
 *      门控字节 g_gate 清零（E2 实测：开第二个句柄后第一个句柄立刻静音）。
 *   2. 握手只有一次机会：g_failCount 没有任何复位点，失败即驱动实例终身锁死，
 *      唯一恢复手段是重启或重载驱动。因此失败后不复试，直接报给用户。
 *   3. 0x222001 恒定返回 STATUS_NOT_IMPLEMENTED（Win32: FALSE + gle=1），
 *      **绝不能**用它的返回码判断成败。
 */
#ifndef MICLINK_DEVIO_H
#define MICLINK_DEVIO_H

#include "app.h"

/* IOCTL 编号：CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func, method, FILE_ANY_ACCESS) */
#define IOCTL_WOMIC_FEED      0x222001u  /* 灌 PCM（返回码骗人） */
#define IOCTL_WOMIC_GETKEY    0x222004u  /* 读 1 字节握手 key（每次调用重置 RNG） */
#define IOCTL_WOMIC_HANDSHAKE 0x222008u  /* 提交 key ^ 0x3B */
#define IOCTL_WOMIC_PROBE     0x22200Cu  /* 恒返回 3，无副作用，可做存在性探测 */
#define IOCTL_WOMIC_STARTSTOP 0x222010u  /* 1 = 启动（清空环形缓冲），0 = 停止 */
#define IOCTL_WOMIC_CAPACITY  0x222014u  /* 2 字节大端环形缓冲容量，[1, 11520] */

#define WOMIC_DEV_PATH L"\\\\.\\WOMic"

/* 单次灌音上限（字节）：48000 Hz × 2 B × 20 ms */
#define DEVIO_FRAME_BYTES 1920

#endif /* MICLINK_DEVIO_H */
