/* app.h — WO Mic Link 客户端公共定义
 *
 * 设计约束（来自需求）：
 *  1. 体积几十 KB 级：纯 Win32 + GDI/GDI+（系统自带 dll，动态加载），静态链接 CRT，
 *     不引入 Qt/Electron/.NET，任何第三方解码器都做成可选动态加载。
 *  2. 零额外运行库：只用 kernel32/user32/gdi32/ws2_32/shell32/dwmapi/gdiplus
 *     这类每台 Windows 都有的系统组件；不依赖 VC++ Redistributable。
 */
#ifndef WOMIC_APP_H
#define WOMIC_APP_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdint.h>

/* ------------------------------------------------------------------ 主题 */
typedef struct {
    COLORREF bg;          /* 窗口底色 */
    COLORREF bg2;         /* 渐变次色 */
    COLORREF card;        /* 卡片 */
    COLORREF card_hi;     /* 卡片悬停/选中 */
    COLORREF stroke;      /* 描边 */
    COLORREF text;        /* 主文字 */
    COLORREF text_dim;    /* 次文字 */
    COLORREF text_mute;   /* 弱文字 */
    COLORREF accent;      /* 强调色 */
    COLORREF accent_dim;  /* 强调色弱化 */
    COLORREF ok;          /* 正常 */
    COLORREF warn;        /* 警告 */
    COLORREF err;         /* 错误 */
    int      dark;
} Theme;

extern Theme g_th;
void theme_apply(int dark);

/* ------------------------------------------------------------------ 引擎 */
typedef enum {
    ST_IDLE = 0, ST_CONNECTING, ST_HANDSHAKE, ST_STREAMING, ST_ERROR
} EngState;

typedef struct {
    EngState state;
    char     peer[64];        /* 对端地址:端口 */
    char     session[32];     /* 会话标识（对端返回/本地生成） */
    char     codec[16];
    int      rate;            /* 采样率 */
    uint64_t frames;          /* 收到帧数 */
    uint64_t bytes;           /* 收到字节 */
    uint32_t lost;            /* 序号缺口估算 */
    uint32_t last_seq;
    float    level;           /* 0..1 瞬时电平 */
    float    peak;            /* 0..1 峰值保持 */
    float    db;              /* dBFS */
    int      kbps;            /* 瞬时码率 */
    int      latency_ms;      /* 估计延迟（帧间隔抖动） */
    int      injected;        /* 已注入驱动的 PCM 字节 */
    int      device_ok;       /* 虚拟麦克风可写 */
    char     last_error[160];
    DWORD    started_at;
} EngineStats;

/* 引擎线程生命周期 */
int  eng_start(const char *host, int ctrl_port, int media_port, int rate_idx);
void eng_stop(void);
void eng_snapshot(EngineStats *out);   /* 线程安全快照 */
int  eng_device_available(void);       /* 探测 \\.\WOMic 可写 */
void eng_set_monitor(int on);          /* 本地监听（需 waveOut） */

/* 日志回调：引擎线程调用，UI 通过 PostMessage 转投 */
typedef void (*LogFn)(const char *line);
void eng_set_logger(LogFn fn);

#endif /* WOMIC_APP_H */
