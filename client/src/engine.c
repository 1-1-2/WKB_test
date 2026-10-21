/* engine.c — 协议引擎
 *
 * 实现 WO Mic Wi-Fi 协议 v1 的 PC 侧（连接发起方）：
 *   TCP 8125 控制面：101 hello → 102 音频参数 → 103 开始 → 105 心跳(1s) → 104 停止
 *   UDP 媒体面：手机单向下发，头格式 C（variant=4）
 *       [04 00][len:u16BE][seq:u16BE][ts:u32BE][vol:u8][payload]
 *   收到 Opus 包 → 解码 → 重采样到 48 kHz/16bit/mono → 由 devio 灌入驱动。
 *
 * 与上一版的三处关键差异（都是 P0）：
 *   1. 灌音改成 devio 的完整握手序列（0x222000 在 2022 驱动上 51/51 失败）；
 *   2. **全进程只开一次 \\.\WOMic**：eng_device_available() 不再自己 CreateFile
 *      （每次 CreateFile 都会把驱动的门控清零，实测会让另一个句柄立刻静音）；
 *   3. 输出固定 48 kHz / 16 bit / 单声道：驱动只搬运字节不做重采样，
 *      喂 44.1k 会变调且不报错（E3 实测：1000Hz → 1088Hz），所以 8k/16k 协商时
 *      由本文件做整数倍上采样（48000/16000=3、48000/8000=6）。
 *
 * 依赖：ws2_32（系统自带）。Opus 静态优先（见 opusdec.c）。
 */
#include "app.h"
#include "devio.h"
#include "opusdec.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define MAX_PKT          2048
#define DEV_RATE         48000          /* 驱动硬要求：不设成别的值 */
#define DEV_SAMPLES_20MS (DEV_RATE / 50)   /* 960 */
#define DEV_FRAME_BYTES  (DEV_SAMPLES_20MS * 2)  /* 1920 */

static volatile LONG g_run;
static HANDLE        g_thread;
static SOCKET        g_tcp = INVALID_SOCKET, g_udp = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static int           g_cs_inited;
static EngineStats   g_st;
static LogFn         g_log;

static int           g_rate_idx = 2;      /* 2=48k 1=16k 0=8k */
static int           g_buf_ms   = 60;
static int           g_gain     = 100;
static DevFeedMode   g_mode     = DEV_FEED_IOCTL;

static int16_t       g_decbuf[DEV_SAMPLES_20MS];
static int16_t       g_feedbuf[DEV_SAMPLES_20MS];
static int           g_short_writes;

/* ------------------------------------------------------------ 日志 */
static void elog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    if (g_log) g_log(buf);
}

/* ------------------------------------------------------- 采样率转换 */
/* 48000 / {48000,16000,80000} 都是整数倍：{1,3,6}。线性插值足够（人耳不敏感，
 * 且驱动侧本来就是「只搬字节」的简单环形缓冲，没必要上 soxr）。 */
static void upsample_int(const int16_t *in, int n, int16_t *out, int factor)
{
    int i, k;
    if (factor <= 1 || n <= 0) { memcpy(out, in, (size_t)n * sizeof(int16_t)); return; }
    for (i = 0; i < n; i++) {
        int a = in[i];
        int b = (i + 1 < n) ? in[i + 1] : in[i];
        for (k = 0; k < factor; k++) {
            int v = a + ((b - a) * k) / factor;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i * factor + k] = (int16_t)v;
        }
    }
}

static void apply_gain(int16_t *p, int n, int gain_pct)
{
    int i;
    if (gain_pct == 100) return;
    for (i = 0; i < n; i++) {
        int v = ((int)p[i] * gain_pct) / 100;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        p[i] = (int16_t)v;
    }
}

/* ------------------------------------------------------- TCP 小工具 */
static int tcp_send_all(const unsigned char *b, int n)
{
    int off = 0;
    while (off < n) {
        int r = send(g_tcp, (const char *)b + off, n - off, 0);
        if (r <= 0) return 0;
        off += r;
    }
    return 1;
}

/* 发一帧：[type][len BE u32][payload] */
static int msg_send(unsigned char type, const unsigned char *pl, uint32_t len)
{
    unsigned char head[5 + 64];
    head[0] = type;
    head[1] = (unsigned char)(len >> 24);
    head[2] = (unsigned char)(len >> 16);
    head[3] = (unsigned char)(len >> 8);
    head[4] = (unsigned char)(len);
    if (len) memcpy(head + 5, pl, len);          /* 头 5B + 负载，一次 send 降低小包开销 */
    return tcp_send_all(head, 5 + (int)len);
}

/* 收满 n 字节（带超时） */
static int recv_exact(unsigned char *b, int n, int timeout_ms)
{
    int got = 0;
    DWORD t0 = GetTickCount();
    while (got < n) {
        if ((int)(GetTickCount() - t0) > timeout_ms) return 0;
        int r = recv(g_tcp, (char *)b + got, n - got, 0);
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}

/* 读一帧：type + len + payload。返回 0 表示超时/断链。 */
static int msg_recv(unsigned char *type, unsigned char *pl, int cap, int *len, int timeout_ms)
{
    unsigned char h[5];
    if (!recv_exact(h, 5, timeout_ms)) return 0;
    *type = h[0];
    uint32_t l = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) |
                 ((uint32_t)h[3] << 8) | h[4];
    if (l > (uint32_t)cap) return 0;
    *len = (int)l;
    if (l && !recv_exact(pl, (int)l, timeout_ms)) return 0;
    return 1;
}

/* ------------------------------------------------------------ 握手
 * 顺序很关键：UDP 必须先 bind，102 才能上报真实媒体端口。
 * 否则手机端会把音频发到端口 0，表现为"握手全成功但永远收不到包"。
 */
static int do_handshake(int rate_idx)
{
    unsigned char pl[64], type, rsp[256];
    int len, mport;
    SOCKADDR_IN ua;
    int ual = sizeof(ua);

    if (getsockname(g_udp, (SOCKADDR *)&ua, &ual) != 0) {
        strcpy(g_st.last_error, "读取本地媒体端口失败");
        return 0;
    }
    mport = ntohs(ua.sin_port);

    /* 101：payload[0]=4 表示 UDP 头格式 C；其余 5 字节原样复刻官方 6.3 */
    unsigned char hello[6] = { 0x04, 0x06, 0x03, 0x00, 0x00, 0x00 };
    if (!msg_send(101, hello, 6)) { strcpy(g_st.last_error, "101 发送失败"); return 0; }
    if (!msg_recv(&type, rsp, sizeof(rsp), &len, 6000)) { strcpy(g_st.last_error, "101 无应答"); return 0; }
    if (type != 101 || len < 2) { sprintf(g_st.last_error, "101 应答异常 type=%u len=%d", type, len); return 0; }
    if (rsp[0] != 0) { sprintf(g_st.last_error, "对端拒绝 hello，status=%u", rsp[0]); return 0; }
    if (rsp[1] != 4) { sprintf(g_st.last_error, "对端头格式=%u（预期 4）", rsp[1]); return 0; }
    elog("101 hello 成功：头格式 C（variant=4）");

    /* 102：codec=2(Opus) / rateIdx / 媒体端口（大端） */
    pl[0] = 2;
    pl[1] = (unsigned char)rate_idx;
    pl[2] = (unsigned char)(mport >> 24);
    pl[3] = (unsigned char)(mport >> 16);
    pl[4] = (unsigned char)(mport >> 8);
    pl[5] = (unsigned char)(mport);
    if (!msg_send(102, pl, 6)) { strcpy(g_st.last_error, "102 发送失败"); return 0; }
    if (!msg_recv(&type, rsp, sizeof(rsp), &len, 6000)) { strcpy(g_st.last_error, "102 无应答"); return 0; }
    if (type != 102 || len < 1) { strcpy(g_st.last_error, "102 应答异常"); return 0; }
    if (rsp[0] != 0) { sprintf(g_st.last_error, "音频参数被拒 status=%u（12=codec 非法 13=采样率非法）", rsp[0]); return 0; }
    elog("102 参数接受：Opus / %d Hz / 媒体端口 %d",
         rate_idx == 0 ? 8000 : rate_idx == 1 ? 16000 : 48000, mport);

    /* 103：开始推流 */
    if (!msg_send(103, NULL, 0)) { strcpy(g_st.last_error, "103 发送失败"); return 0; }
    if (!msg_recv(&type, rsp, sizeof(rsp), &len, 6000)) { strcpy(g_st.last_error, "103 无应答"); return 0; }
    if (len < 1 || rsp[0] != 0) { strcpy(g_st.last_error, "103 被拒"); return 0; }
    elog("103 推流已启动，等待 UDP 音频…");
    return 1;
}

/* ------------------------------------------------ UDP 帧解析（格式 C） */
static void on_udp_frame(const unsigned char *buf, int n)
{
    float rms, db;
    int i, samples = 0, factor;

    if (n < 11 || buf[0] != 0x04 || buf[1] != 0x00) return;
    int plen = (buf[2] << 8) | buf[3];
    uint16_t seq = (uint16_t)((buf[4] << 8) | buf[5]);
    const unsigned char *payload = buf + 11;
    int paylen = plen - 7;                 /* 去掉 seq2 + ts4 + vol1 */
    if (paylen <= 0 || 11 + paylen > n) return;

    EnterCriticalSection(&g_cs);
    if (g_st.frames && seq != (uint16_t)(g_st.last_seq + 1))
        g_st.lost += (uint16_t)(seq - g_st.last_seq - 1);
    g_st.last_seq = seq;
    g_st.frames++;
    g_st.bytes += (uint64_t)n;
    LeaveCriticalSection(&g_cs);

    if (!opusdec_ready()) {
        /* 无解码器：用包大小作为活动度代理，界面仍有反馈 */
        float act = paylen / 400.0f; if (act > 1) act = 1;
        EnterCriticalSection(&g_cs);
        g_st.level += (act - g_st.level) * 0.2f;
        if (act > g_st.peak) g_st.peak = act;
        if (act < 0.01f) g_st.silent_frames++; else g_st.silent_frames = 0;
        LeaveCriticalSection(&g_cs);
        return;
    }

    samples = opusdec_decode(payload, paylen, g_decbuf, DEV_SAMPLES_20MS, 0);
    if (samples <= 0) return;
    if (samples > DEV_SAMPLES_20MS) samples = DEV_SAMPLES_20MS;

    /* 解码出来的采样率 = 协商速率；驱动只认 48k，这里做整数倍上采样 */
    factor = DEV_RATE / (g_st.rate ? g_st.rate : DEV_RATE);
    if (factor < 1) factor = 1;
    upsample_int(g_decbuf, samples, g_feedbuf, factor);
    samples *= factor;                       /* 正好 960（20 ms @48k） */
    apply_gain(g_feedbuf, samples, g_gain);

    {
        double acc = 0;
        for (i = 0; i < samples; i++) { double v = g_feedbuf[i] / 32768.0; acc += v * v; }
        rms = (float)sqrt(acc / (samples ? samples : 1));
        db = (rms > 1e-6f) ? 20.0f * (float)log10((double)rms) : -96.0f;
    }

    EnterCriticalSection(&g_cs);
    g_st.level += (rms - g_st.level) * 0.35f;     /* attack 快 */
    if (rms > g_st.peak) g_st.peak = rms;
    g_st.db = db;
    if (rms < 0.004f) g_st.silent_frames++; else g_st.silent_frames = 0;
    LeaveCriticalSection(&g_cs);

    if (devio_is_open()) {
        int w = devio_write(g_feedbuf, samples * 2);
        if (w > 0) {
            EnterCriticalSection(&g_cs);
            g_st.injected += (uint64_t)w;
            LeaveCriticalSection(&g_cs);
            /* 背压看门狗：只有 WriteFile 路径能看到「被截断」，IOCTL 路径看不到，
             * 因此这个恢复手段只在 WriteFile 模式下启用。连续被截断说明环形缓冲
             * 将满，且门控可能已被别的进程清零 —— 重新走一遍握手（不重试失败）。 */
            if (g_mode == DEV_FEED_WRITE && w < samples * 2) {
                if (++g_short_writes > 20) {
                    g_short_writes = 0;
                    if (devio_rehandshake()) elog("检测到写入被持续截断，已重新握手");
                }
            } else {
                g_short_writes = 0;
            }
        }
    }
}

/* --------------------------------------------------------- 工作线程 */
static unsigned __stdcall worker(void *arg)
{
    WSADATA wsa;
    SOCKADDR_IN sa, local;
    DWORD last_hb = 0, last_stat = 0;
    uint64_t prev_bytes = 0;
    unsigned char type, pl[512];
    int len;
    char host[64], *colon;
    int port = 8125;
    (void)arg;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        strcpy(g_st.last_error, "WSAStartup 失败");
        g_st.state = ST_ERROR;
        return 0;
    }

    strncpy(host, g_st.peer, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }

    /* 1) 先建 UDP 并 bind，拿到真实端口 */
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) { strcpy(g_st.last_error, "UDP socket 失败"); g_st.state = ST_ERROR; goto out; }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(g_udp, (SOCKADDR *)&local, sizeof(local)) != 0) {
        sprintf(g_st.last_error, "UDP bind 失败 WSA=%d", WSAGetLastError());
        g_st.state = ST_ERROR;
        goto out;
    }

    /* 2) 再连 TCP */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, host, &sa.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) { strcpy(g_st.last_error, "地址解析失败"); g_st.state = ST_ERROR; goto out; }
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    }
    g_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_tcp == INVALID_SOCKET) { strcpy(g_st.last_error, "TCP socket 失败"); g_st.state = ST_ERROR; goto out; }
    elog("正在连接 %s:%d …", host, port);
    if (connect(g_tcp, (SOCKADDR *)&sa, sizeof(sa)) != 0) {
        sprintf(g_st.last_error, "连接失败 WSA=%d", WSAGetLastError());
        g_st.state = ST_ERROR;
        goto out;
    }
    { BOOL nb = TRUE; setsockopt(g_tcp, IPPROTO_TCP, TCP_NODELAY, (char *)&nb, sizeof(nb)); }

    g_st.state = ST_HANDSHAKE;
    if (!do_handshake(g_rate_idx)) { g_st.state = ST_ERROR; goto out; }

    g_st.state = ST_STREAMING;
    g_st.started_at = GetTickCount();
    elog("已连接，进入推流状态");

    while (g_run) {
        fd_set rf;
        struct timeval tv = { 0, 200 * 1000 };
        FD_ZERO(&rf);
        FD_SET(g_tcp, &rf);
        FD_SET(g_udp, &rf);
        int sel = select(0, &rf, NULL, NULL, &tv);
        if (sel > 0) {
            if (FD_ISSET(g_udp, &rf)) {
                unsigned char buf[MAX_PKT];
                int n = recv(g_udp, (char *)buf, sizeof(buf), 0);
                if (n > 0) on_udp_frame(buf, n);
            }
            if (FD_ISSET(g_tcp, &rf)) {
                if (!msg_recv(&type, pl, sizeof(pl), &len, 100)) {
                    elog("控制连接已断开");
                    strcpy(g_st.last_error, "对端关闭控制连接");
                    g_st.state = ST_ERROR;
                    break;
                }
                if (type == 0 && len >= 5) {      /* 手机端错误串：00 len 01 ascii */
                    char err[128] = { 0 };
                    int cp = len - 5 < 127 ? len - 5 : 127;
                    memcpy(err, pl + 5, cp);
                    elog("对端错误：%s", err);
                }
            }
        }
        DWORD now = GetTickCount();
        if (now - last_hb >= 1000) {              /* 105 心跳 1s/次；对端无应答 */
            msg_send(105, NULL, 0);
            last_hb = now;
        }
        if (now - last_stat >= 1000) {
            uint64_t b;
            EnterCriticalSection(&g_cs);
            b = g_st.bytes;
            g_st.kbps = (int)((b - prev_bytes) * 8 / 1000);
            g_st.peak *= 0.72f;                   /* release 慢 */
            if (g_st.peak < 0.002f) g_st.peak = 0;
            g_st.latency_ms = 20;
            LeaveCriticalSection(&g_cs);
            prev_bytes = b;
            last_stat = now;
        }
    }

out:
    if (g_tcp != INVALID_SOCKET) {
        msg_send(104, NULL, 0);                   /* 主动停止推流 */
        closesocket(g_tcp); g_tcp = INVALID_SOCKET;
    }
    if (g_udp != INVALID_SOCKET) { closesocket(g_udp); g_udp = INVALID_SOCKET; }
    WSACleanup();
    devio_stop();
    if (g_st.state != ST_ERROR) g_st.state = ST_IDLE;
    return 0;
}

/* --------------------------------------------------------- 对外 API */
void eng_set_logger(LogFn fn) { g_log = fn; }

/* 不再独立 CreateFile：门控是驱动全局的，任何新句柄都会把它清零。
 * “设备可用”只看本进程持有的那个长句柄。 */
int eng_device_available(void) { return devio_is_open() && devio_handshake_ok(); }

static int cap_from_ms(int ms)
{
    if (ms <= 26) return DEV_CAP_26MS;
    if (ms <= 60) return DEV_CAP_60MS;
    return DEV_CAP_120MS;
}

int eng_start(const char *hostport, int ctrl_port, int rate_idx, int buffer_ms, int gain)
{
    if (g_run) return 0;
    if (!g_cs_inited) { InitializeCriticalSection(&g_cs); g_cs_inited = 1; }

    g_rate_idx = (rate_idx >= 0 && rate_idx <= 2) ? rate_idx : 2;
    g_buf_ms = (buffer_ms == 26 || buffer_ms == 60 || buffer_ms == 120) ? buffer_ms : 60;
    g_gain = (gain >= 0 && gain <= 200) ? gain : 100;

    EnterCriticalSection(&g_cs);
    memset(&g_st, 0, sizeof(g_st));
    LeaveCriticalSection(&g_cs);

    g_st.state = ST_CONNECTING;
    g_st.rate = g_rate_idx == 0 ? 8000 : g_rate_idx == 1 ? 16000 : 48000;
    g_st.rate_out = DEV_RATE;
    g_st.buffer_ms = g_buf_ms;
    g_st.gain = g_gain;
    snprintf(g_st.peer, sizeof(g_st.peer), "%s:%d", hostport, ctrl_port);
    snprintf(g_st.session, sizeof(g_st.session), "local-%08X", (unsigned)GetTickCount());
    strcpy(g_st.codec, "Opus");

    if (!opusdec_open(g_st.rate)) {
        elog("Opus 解码器不可用（%s）：仅统计与电平，不注入音频", opusdec_source());
    } else {
        elog("Opus 解码器已就绪（%s，%d Hz → 输出 48000 Hz）", opusdec_source(), g_st.rate);
    }
    snprintf(g_st.opus_src, sizeof(g_st.opus_src), "%s", opusdec_source());

    /* 设备：复用全局唯一句柄。打开失败不阻塞连接（协议与统计照跑）。 */
    if (devio_open()) {
        int cap = cap_from_ms(g_buf_ms);
        if (devio_start(cap, g_mode)) {
            g_st.device_ok = 1;
            g_st.dev_handshake_ok = 1;
            elog("虚拟麦克风已就绪：容量 %d B（%d ms），灌音路径 %s",
                 cap, g_buf_ms, g_mode == DEV_FEED_WRITE ? "WriteFile" : "IOCTL 0x222001");
        } else {
            elog("驱动握手失败：%s", devio_last_error());
            elog("提示：2022 驱动握手失败计数没有复位点，需重启电脑或重载驱动后重试");
        }
    } else {
        elog("未检测到 \\\\.\\WOMic：%s（可点“安装驱动”）", devio_last_error());
    }

    g_run = 1;
    g_thread = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)(intptr_t)g_rate_idx, 0, NULL);
    if (!g_thread) { g_run = 0; return 0; }
    return 1;
}

void eng_stop(void)
{
    if (!g_run) return;
    InterlockedExchange(&g_run, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    devio_stop();
    opusdec_close();
    EnterCriticalSection(&g_cs);
    g_st.state = ST_IDLE;
    g_st.level = 0;
    g_st.peak = 0;
    g_st.db = -96.0f;
    g_st.silent_frames = 0;
    LeaveCriticalSection(&g_cs);
}

void eng_snapshot(EngineStats *out)
{
    if (!g_cs_inited) { memset(out, 0, sizeof(*out)); return; }
    EnterCriticalSection(&g_cs);
    *out = g_st;
    LeaveCriticalSection(&g_cs);
}

void eng_set_monitor(int on) { (void)on; }

/* 供 CLI/GUI 选择灌音路径（默认 IOCTL，见 devio.c 顶部注释） */
void eng_set_feed_mode(int mode) { g_mode = (DevFeedMode)mode; }
