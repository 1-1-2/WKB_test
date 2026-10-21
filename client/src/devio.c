/* devio.c — 设备句柄与灌音序列
 *
 * 时序（whitepaper §9.2，顺序不可变）：
 *   CreateFile → 0x222004(读 key) → 0x222008(key^0x3B, in=1 且 out=1)
 *              → 0x222014(容量, 2B 大端) → 0x222010(1 启动) → 灌 PCM → 0x222010(0)
 *
 * 灌音路径的取舍（这是本项目少数几个「两说」的点，记录在案）：
 *   · IOCTL 0x222001  —— diagnosis.md §9.2 实测 PASS（rms=8485 / 主峰 1000Hz /
 *                        snr=735），但永远返回 NOT_IMPLEMENTED，拿不到真实写入字节。
 *   · WriteFile       —— whitepaper §4.3 推荐：返回 STATUS_SUCCESS + 真实写入
 *                        字节数，可做背压；但 §7.2 指出 MdlAddress 无 NULL 检查，
 *                        且 diagnosis.md §9.1 曾记录「无 FILE_FLAG_NO_BUFFERING 会
 *                        挂死」（whitepaper 静态复核未发现挂死路径，该现象未证实）。
 *   结论：**默认走 IOCTL**（已实测、无挂死风险），WriteFile 作为显式可选项
 *   （--feed write / GUI 高级区），一旦实测确认再翻转默认值。两条路径共用同一个
 *   环形缓冲，可混用，切换成本为零。
 */
#include "devio.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static HANDLE      g_dev = INVALID_HANDLE_VALUE;
static int         g_open_tryed;      /* 已经尝试过开（失败不再反复重试，避免吵） */
static int         g_handshake_ok;
static int         g_handshake_failed;/* 一次失败即永久锁死，不再试 */
static int         g_started;
static int         g_cap = DEV_CAP_60MS;
static DevFeedMode g_mode = DEV_FEED_IOCTL;
static uint64_t    g_written;
static char        g_err[256];

const char *devio_last_error(void) { return g_err; }

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_err, sizeof(g_err) - 1, fmt, ap);
    va_end(ap);
    g_err[sizeof(g_err) - 1] = 0;
}

/* --------------------------------------------------------------- 打开 */
int devio_open(void)
{
    if (g_dev != INVALID_HANDLE_VALUE) return 1;
    if (g_open_tryed && g_dev == INVALID_HANDLE_VALUE) return 0;
    g_open_tryed = 1;
    /* 不加 FILE_FLAG_NO_BUFFERING：控制设备是 DO_DIRECT_IO，加不加都走 MDL，
     * 加了反而引入对齐约束（whitepaper §9.1-3）。 */
    g_dev = CreateFileW(WOMIC_DEV_PATH, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) {
        set_err("无法打开 \\\\.\\WOMic (gle=%lu)：驱动可能未安装", GetLastError());
        return 0;
    }
    return 1;
}

void devio_close(void)
{
    if (g_dev == INVALID_HANDLE_VALUE) return;
    if (g_started) {
        unsigned char off = 0;
        DWORD ret = 0;
        DeviceIoControl(g_dev, IOCTL_WOMIC_STARTSTOP, &off, 1, NULL, 0, &ret, NULL);
        g_started = 0;
    }
    CloseHandle(g_dev);
    g_dev = INVALID_HANDLE_VALUE;
    g_handshake_ok = 0;
}

int devio_is_open(void) { return g_dev != INVALID_HANDLE_VALUE; }
int devio_handshake_ok(void) { return g_handshake_ok; }
int devio_capacity(void) { return g_cap; }
int devio_written_bytes(void) { return (int)(g_written & 0x7FFFFFFF); }

/* --------------------------------------------------------------- 探测 */
/* 0x22200C 恒返回 3、无需握手、无副作用 —— 唯一可以安全重复调用的存在性探针。
 * 注意它仍需要一个已打开的句柄，所以绝不能为了“探测”临时开一个新句柄：
 * 那会把别的进程的门控清零（E2）。 */
int devio_probe(int *val)
{
    DWORD ret = 0;
    unsigned char out[4] = { 0, 0, 0, 0 };
    if (g_dev == INVALID_HANDLE_VALUE) { set_err("设备未打开"); return 0; }
    if (!DeviceIoControl(g_dev, IOCTL_WOMIC_PROBE, NULL, 0, out, 1, &ret, NULL)) {
        set_err("0x22200C 探测失败 (gle=%lu)", GetLastError());
        return 0;
    }
    if (val) *val = ret ? out[0] : -1;
    return 1;
}

/* --------------------------------------------------------------- 握手 */
static int do_handshake(void)
{
    unsigned char key = 0;
    unsigned char hs[1];
    DWORD ret = 0;

    /* 1) 读 key。每次调用都会重新生成并覆盖 g_key —— 禁止连读两次。 */
    if (!DeviceIoControl(g_dev, IOCTL_WOMIC_GETKEY, NULL, 0, &key, 1, &ret, NULL) || ret != 1) {
        set_err("0x222004 读握手 key 失败 (gle=%lu ret=%lu)", GetLastError(), ret);
        return 0;
    }
    /* 2) 提交 key ^ 0x3B。2022 版检查 OutputBufferLength==1，2017 版检查
     *    InputBufferLength==1 —— 两个都传 1 才能跨版本兼容。
     *    失败也返回 STATUS_SUCCESS，所以这里看不出成败，只能继续往下走。 */
    hs[0] = (unsigned char)(key ^ 0x3B);
    if (!DeviceIoControl(g_dev, IOCTL_WOMIC_HANDSHAKE, hs, 1, hs, 1, &ret, NULL)) {
        set_err("0x222008 握手提交失败 (gle=%lu)", GetLastError());
        g_handshake_failed = 1;
        return 0;
    }
    g_handshake_ok = 1;
    return 1;
}

/* 门控被别的进程（或另一次 CreateFile）清零后的恢复手段。
 * 只能重新读 key 再提交一遍 —— 不允许盲目重试（失败计数会锁死驱动）。 */
int devio_rehandshake(void)
{
    if (g_dev == INVALID_HANDLE_VALUE) return 0;
    if (g_handshake_failed) return 0;
    return do_handshake();
}

/* --------------------------------------------------------------- 启停 */
int devio_start(int capacity, DevFeedMode mode)
{
    unsigned char cap_be[2];
    unsigned char on = 1;
    DWORD ret = 0;

    if (g_handshake_failed) {
        set_err("驱动握手曾失败，已锁死：请重启电脑或重新安装/重载驱动后重试");
        return 0;
    }
    if (!devio_open()) return 0;
    g_mode = mode;

    if (!do_handshake()) return 0;

    /* 钳制容量：0x222014 无下界保护，传 0 会把 g_capacity 置 0 → 永久静音。
     * 上界 0x2D00 是驱动内硬上限（超过也不生效，但保持一致更利于排障）。 */
    if (capacity < DEV_CAP_MIN) capacity = DEV_CAP_MIN;
    if (capacity > DEV_CAP_MAX) capacity = DEV_CAP_MAX;
    g_cap = capacity;

    cap_be[0] = (unsigned char)((capacity >> 8) & 0xFF);
    cap_be[1] = (unsigned char)(capacity & 0xFF);
    if (!DeviceIoControl(g_dev, IOCTL_WOMIC_CAPACITY, cap_be, 2, NULL, 0, &ret, NULL)) {
        set_err("0x222014 设置容量失败 (gle=%lu)", GetLastError());
        return 0;
    }
    if (!DeviceIoControl(g_dev, IOCTL_WOMIC_STARTSTOP, &on, 1, NULL, 0, &ret, NULL)) {
        set_err("0x222010 启动失败 (gle=%lu)", GetLastError());
        return 0;
    }
    g_started = 1;
    g_written = 0;
    return 1;
}

int devio_stop(void)
{
    unsigned char off = 0;
    DWORD ret = 0;
    if (g_dev == INVALID_HANDLE_VALUE) return 1;
    if (!g_started) return 1;
    DeviceIoControl(g_dev, IOCTL_WOMIC_STARTSTOP, &off, 1, NULL, 0, &ret, NULL);
    g_started = 0;
    return 1;
}

/* --------------------------------------------------------------- 灌音 */
int devio_write(const void *pcm, int bytes)
{
    DWORD ret = 0;
    if (g_dev == INVALID_HANDLE_VALUE) return -1;
    if (bytes <= 0) return -1;                       /* 永不发 0 长度写（R4） */
    if (bytes > DEV_CAP_MAX) bytes = DEV_CAP_MAX;    /* 超容量整包丢弃，不如自己截 */

    if (g_mode == DEV_FEED_WRITE) {
        DWORD got = 0;
        BOOL ok = WriteFile(g_dev, pcm, (DWORD)bytes, &got, NULL);
        if (!ok || got == 0) {
            set_err("WriteFile 灌音失败 (gle=%lu got=%lu)", GetLastError(), (unsigned long)got);
            return -1;
        }
        g_written += got;
        return (int)got;
    }

    /* IOCTL 路径：返回值恒为 NOT_IMPLEMENTED，**忽略**。 */
    DeviceIoControl(g_dev, IOCTL_WOMIC_FEED, (LPVOID)pcm, (DWORD)bytes, NULL, 0, &ret, NULL);
    g_written += (uint64_t)bytes;
    return bytes;
}
