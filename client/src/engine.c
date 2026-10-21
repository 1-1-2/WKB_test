/* engine.c — 协议引擎
 *
 * 实现 WO Mic Wi-Fi 协议 v1 的 PC 侧（连接发起方）：
 *   TCP 8125 控制面：101 hello → 102 音频参数 → 103 开始 → 105 心跳(1s) → 104 停止
 *   UDP 媒体面：手机单向下发，头格式 C（variant=4）
 *       [04 00][len:u16BE][seq:u16BE][ts:u32BE][vol:u8][payload]
 *   收到编码包后（若 opus.dll 可用）解码为 PCM，经 DeviceIoControl(0x222000)
 *   以 1920B/20ms 灌入官方虚拟麦克风驱动。
 *
 * 依赖：ws2_32（系统自带）。opus 解码器为可选动态加载，缺失时降级为"仅统计"。
 */
#include "app.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/time.h>
#include <process.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define IOCTL_WOMIC_FEED 0x222000u   /* CTL_CODE(0x22, 0x800, METHOD_BUFFERED, ANY) */
#define FRAME_BYTES      1920        /* 48kHz × 20ms × 16bit × 1ch */
#define MAX_PKT          2048

static volatile LONG g_run;
static HANDLE        g_thread;
static SOCKET        g_tcp = INVALID_SOCKET, g_udp = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static int           g_cs_inited;
static EngineStats   g_st;
static LogFn         g_log;

/* ---------------- 可选 Opus 解码器 ---------------- */
typedef void* (*fn_create)(int, int, int*);
typedef int   (*fn_decode)(void*, const unsigned char*, int, int16_t*, int, int);
typedef void  (*fn_destroy)(void*);
static struct { HMODULE mod; fn_create create; fn_decode decode; fn_destroy destroy; void *st; int ok; } g_opus;

static void opus_try_load(int rate)
{
    int err = 0;
    memset(&g_opus, 0, sizeof(g_opus));
    g_opus.mod = LoadLibraryW(L"opus.dll");
    if (!g_opus.mod) return;
    g_opus.create  = (fn_create)(void*)GetProcAddress(g_opus.mod, "opus_decoder_create");
    g_opus.decode  = (fn_decode)(void*)GetProcAddress(g_opus.mod, "opus_decode");
    g_opus.destroy = (fn_destroy)(void*)GetProcAddress(g_opus.mod, "opus_decoder_destroy");
    if (!g_opus.create || !g_opus.decode) return;
    g_opus.st = g_opus.create(rate, 1, &err);
    g_opus.ok = (err == 0 && g_opus.st != NULL);
}

/* ---------------- 日志 ---------------- */
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

/* ---------------- 驱动注入 ---------------- */
static HANDLE g_dev = INVALID_HANDLE_VALUE;

static int dev_open(void)
{
    if (g_dev != INVALID_HANDLE_VALUE) return 1;
    g_dev = CreateFileW(L"\\\\.\\WOMic", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    return g_dev != INVALID_HANDLE_VALUE;
}

static void dev_close(void)
{
    if (g_dev != INVALID_HANDLE_VALUE) { CloseHandle(g_dev); g_dev = INVALID_HANDLE_VALUE; }
}

int eng_device_available(void)
{
    HANDLE h = CreateFileW(L"\\\\.\\WOMic", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void dev_feed(const int16_t *pcm, int bytes)
{
    DWORD ret = 0;
    if (!dev_open()) return;
    DeviceIoControl(g_dev, IOCTL_WOMIC_FEED, (LPVOID)pcm, (DWORD)bytes, NULL, 0, &ret, NULL);
}

/* ---------------- TCP 小工具 ---------------- */
static int tcp_send_all(const unsigned char *b, int n)
{
    int off = 0;
    while (off < n) {
        int r = send(g_tcp, (const char*)b + off, n - off, 0);
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
    memcpy(head + 5, pl, len);          /* 头 5B + 负载，一次 send 降低小包开销 */
    return tcp_send_all(head, 5 + (int)len);
}

/* 收满 n 字节（带超时） */
static int recv_exact(unsigned char *b, int n, int timeout_ms)
{
    int got = 0;
    DWORD t0 = GetTickCount();
    while (got < n) {
        if ((int)(GetTickCount() - t0) > timeout_ms) return 0;
        int r = recv(g_tcp, (char*)b + got, n - got, 0);
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

/* ---------------- 握手 ----------------
 * 顺序很关键：UDP 必须先 bind，102 才能上报真实媒体端口。
 * 否则手机端会把音频发到端口 0，表现为"握手全成功但永远收不到包"。
 */
static int do_handshake(int rate_idx)
{
    unsigned char pl[64], type, rsp[256];
    int len, mport;
    SOCKADDR_IN ua;
    int ual = sizeof(ua);

    if (getsockname(g_udp, (SOCKADDR*)&ua, &ual) != 0) {
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

/* ---------------- UDP 帧解析（格式 C） ---------------- */
static void on_udp_frame(const unsigned char *buf, int n)
{
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

    int16_t pcm[FRAME_BYTES / 2];
    int samples = 0;
    if (g_opus.ok) {
        samples = g_opus.decode(g_opus.st, payload, paylen, pcm, FRAME_BYTES / 2, 0);
        if (samples < 0) samples = 0;
    }

    if (samples > 0) {
        double acc = 0;
        for (int i = 0; i < samples; i++) { double v = pcm[i] / 32768.0; acc += v * v; }
        float rms = (float)sqrt(acc / (samples ? samples : 1));
        float db = (rms > 1e-6f) ? 20.0f * (float)log10(rms) : -96.0f;

        EnterCriticalSection(&g_cs);
        g_st.level += (rms - g_st.level) * 0.35f;
        if (rms > g_st.peak) g_st.peak = rms;
        g_st.db = db;
        LeaveCriticalSection(&g_cs);

        dev_feed(pcm, samples * 2);
        EnterCriticalSection(&g_cs);
        g_st.injected += samples * 2;
        LeaveCriticalSection(&g_cs);
    } else {
        /* 无解码器：用包大小作为活动度代理，界面仍有反馈 */
        float act = paylen / 400.0f; if (act > 1) act = 1;
        EnterCriticalSection(&g_cs);
        g_st.level += (act - g_st.level) * 0.2f;
        if (act > g_st.peak) g_st.peak = act;
        LeaveCriticalSection(&g_cs);
    }
}

/* ---------------- 工作线程 ---------------- */
static unsigned __stdcall worker(void *arg)
{
    int rate_idx = (int)(intptr_t)arg;
    WSADATA wsa;
    SOCKADDR_IN sa, local;
    DWORD last_hb = 0, last_stat = 0;
    uint64_t prev_bytes = 0;
    unsigned char type, pl[512];
    int len;
    char host[64], *colon;
    int port = 8125;

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
    if (bind(g_udp, (SOCKADDR*)&local, sizeof(local)) != 0) {
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
    elog("正在连接 %s:%d …", host, port);
    if (connect(g_tcp, (SOCKADDR*)&sa, sizeof(sa)) != 0) {
        sprintf(g_st.last_error, "连接失败 WSA=%d", WSAGetLastError());
        g_st.state = ST_ERROR;
        goto out;
    }
    { BOOL nb = TRUE; setsockopt(g_tcp, IPPROTO_TCP, TCP_NODELAY, (char*)&nb, sizeof(nb)); }

    g_st.state = ST_HANDSHAKE;
    if (!do_handshake(rate_idx)) { g_st.state = ST_ERROR; goto out; }

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
                int n = recv(g_udp, (char*)buf, sizeof(buf), 0);
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
                    char err[128] = {0};
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
            g_st.peak *= 0.72f;
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
    dev_close();
    if (g_st.state != ST_ERROR) g_st.state = ST_IDLE;
    return 0;
}

/* ---------------- 对外 API ---------------- */
void eng_set_logger(LogFn fn) { g_log = fn; }

int eng_start(const char *hostport, int ctrl_port, int media_port, int rate_idx)
{
    (void)media_port;
    if (g_run) return 0;
    if (!g_cs_inited) { InitializeCriticalSection(&g_cs); g_cs_inited = 1; }
    EnterCriticalSection(&g_cs);
    memset(&g_st, 0, sizeof(g_st));
    LeaveCriticalSection(&g_cs);
    g_st.state = ST_CONNECTING;
    g_st.rate = rate_idx == 0 ? 8000 : rate_idx == 1 ? 16000 : 48000;
    snprintf(g_st.peer, sizeof(g_st.peer), "%s:%d", hostport, ctrl_port);
    snprintf(g_st.session, sizeof(g_st.session), "local-%08X", (unsigned)GetTickCount());
    strcpy(g_st.codec, "Opus");
    g_st.device_ok = eng_device_available();

    opus_try_load(g_st.rate);
    if (!g_opus.ok) elog("未找到 opus.dll：仅统计与电平，不注入音频");
    else elog("Opus 解码器已加载（%d Hz）", g_st.rate);
    if (!g_st.device_ok) elog("未检测到 \\\\.\\WOMic 设备：注音将跳过");

    g_run = 1;
    g_thread = (HANDLE)_beginthreadex(NULL, 0, worker, (void*)(intptr_t)rate_idx, 0, NULL);
    return g_thread != NULL;
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
    if (g_opus.st && g_opus.destroy) g_opus.destroy(g_opus.st);
    g_opus.st = NULL;
    g_opus.ok = 0;
    g_st.state = ST_IDLE;
}

void eng_snapshot(EngineStats *out)
{
    if (!g_cs_inited) { memset(out, 0, sizeof(*out)); return; }
    EnterCriticalSection(&g_cs);
    *out = g_st;
    LeaveCriticalSection(&g_cs);
}

void eng_set_monitor(int on) { (void)on; }
