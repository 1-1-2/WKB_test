/* app.h — WO Mic Link 客户端公共定义
 *
 * 设计约束（来自需求）：
 *  1. 体积几十 KB 级：纯 Win32 + GDI/GDI+（系统自带 dll，动态加载），静态链接 CRT，
 *     不引入 Qt/Electron/.NET，任何第三方解码器都做成可选动态加载。
 *  2. 零额外运行库：只用 kernel32/user32/gdi32/ws2_32/shell32/dwmapi/gdiplus
 *     这类每台 Windows 都有的系统组件；不依赖 VC++ Redistributable。
 *  3. 驱动相关的硬约束见 docs/driver-whitepaper.md §9，违反任一条都会「静悄悄失败」。
 */
#ifndef WOMIC_APP_H
#define WOMIC_APP_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdint.h>

/* ------------------------------------------------------------------ 版本 */
#define APP_NAME        "WO Mic Link"
#define APP_VERSION     "1.1.0"
#define APP_VERSION_Q   1, 1, 0, 0

/* 内置驱动归属声明（--help / --status 输出与 NOTICE.md 保持一致） */
#define DRV_OWNER       "Wolicheng (WO Mic)"
#define DRV_VERSION     "2.1.0.0"
#define DRV_SOURCE_URL  "https://www.wolicheng.com/womic/"

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
    COLORREF meter_off;   /* 电平表未点亮段 */
    int      dark;
} Theme;

extern Theme g_th;
void theme_apply(int dark);

/* ------------------------------------------------------- 架构与驱动包 */
typedef enum {
    DRVARCH_UNKNOWN = 0,
    DRVARCH_INTEL   = 1,   /* PROCESSOR_ARCHITECTURE_INTEL  → womic.* */
    DRVARCH_AMD64   = 2,   /* PROCESSOR_ARCHITECTURE_AMD64 → womic_1.* */
    DRVARCH_ARM64   = 3    /* PROCESSOR_ARCHITECTURE_ARM64 → womic_2.* */
} DrvArch;

int  drvpkg_detect_arch(void);
const wchar_t *drvpkg_arch_dir(int arch);    /* "x86" / "amd64" / "arm64" */
const wchar_t *drvpkg_arch_label(int arch);  /* 人类可读，用于 --status */
int  drvpkg_available(int arch);             /* 内置包里有没有这一档 */
int  drvpkg_extract(int arch, const wchar_t *out_dir, int force);
int  drvpkg_extract_dir(const wchar_t *src_dir, const wchar_t *out_dir, int force);
const char *drvpkg_last_error(void);

/* ------------------------------------------------------------- 驱动状态 */
typedef enum {
    DRV_UNKNOWN = 0,
    DRV_NOT_INSTALLED,
    DRV_READY,          /* L1+L2 通过 */
    DRV_NEED_REBOOT     /* INF 已装但设备起不来 */
} DrvState;

typedef struct {
    DrvState state;
    int      arch;
    int      handle_ok;             /* L1：CreateFile \\.\WOMic 成功 */
    int      inf_ok;                /* L2：InfPath 非空 */
    int      sys_on_disk;           /* L3：System32\drivers\womic.sys 落盘 */
    wchar_t  inf_path[MAX_PATH];    /* oemNN.inf */
    wchar_t  inf_full[MAX_PATH];    /* C:\Windows\INF\oemNN.inf */
    wchar_t  provider[64];
    wchar_t  version[32];
    wchar_t  sys_path[MAX_PATH];
    char     detail[256];           /* 最近一次操作的说明/错误 */
} DrvStatus;

void   drv_set_external_pack(const wchar_t *dir);   /* --driver-pack */
const wchar_t *drv_external_pack(void);
int    drv_detect(DrvStatus *out, int probe_handle);
int    drv_install(void);
int    drv_uninstall(void);
int    drv_is_elevated(void);
const char *drv_last_error(void);
const wchar_t *drv_last_error_w(void);

/* ------------------------------------------------------------- 设备 IO */
/* 全部 IOCTL 编号来自 docs/driver-whitepaper.md §4（2022 驱动 2.1.0.0）。
 * 旧的 0x222000 是 2017 驱动的编号，在 2022 驱动上 51/51 失败，已删除。 */
#define DEV_CAP_MIN     1           /* 0x222014 无下界保护，0 会永久静音 */
#define DEV_CAP_MAX     0x2D00      /* 11520 B = 120 ms @48k/16/mono */
#define DEV_CAP_26MS    0x0A00
#define DEV_CAP_60MS    0x1680
#define DEV_CAP_120MS   0x2D00

typedef enum { DEV_FEED_IOCTL = 0, DEV_FEED_WRITE = 1 } DevFeedMode;

int  devio_open(void);              /* 全进程只调用一次；句柄长期持有 */
void devio_close(void);
int  devio_is_open(void);
int  devio_probe(int *val);         /* 0x22200C，恒 3；无副作用，可做存在性探测 */
int  devio_start(int capacity, DevFeedMode mode);   /* 完整握手 + 容量 + 启动 */
int  devio_stop(void);
int  devio_write(const void *pcm, int bytes);       /* 成功返回写入字节数 */
int  devio_rehandshake(void);       /* 门控被别的进程清零时的看门狗恢复 */
int  devio_handshake_ok(void);
int  devio_capacity(void);
int  devio_written_bytes(void);
const char *devio_last_error(void);

/* ------------------------------------------------------------- Opus */
int  opusdec_open(int rate);
void opusdec_close(void);
int  opusdec_ready(void);
int  opusdec_decode(const unsigned char *pkt, int len, int16_t *out, int cap, int fec);
const char *opusdec_source(void);   /* "static" / "opus.dll" / "none" */

/* ------------------------------------------------------------- 引擎 */
typedef enum {
    ST_IDLE = 0, ST_CONNECTING, ST_HANDSHAKE, ST_STREAMING, ST_ERROR
} EngState;

typedef struct {
    EngState state;
    char     peer[64];        /* 对端地址:端口 */
    char     session[32];     /* 会话标识（对端返回/本地生成） */
    char     codec[16];
    int      rate;            /* 协商采样率（8k/16k/48k） */
    int      rate_out;        /* 灌给驱动的采样率（恒 48000） */
    int      buffer_ms;       /* 环形缓冲档位 26/60/120 */
    int      gain;            /* 本地增益 0..200 (%) */
    uint64_t frames;          /* 收到帧数 */
    uint64_t bytes;           /* 收到字节 */
    int      media_port;      /* 102 里上报的 UDP 媒体端口（0 = 尚未绑定） */
    int      media_fixed;     /* 1 = --media-port 指定；0 = 系统随机分配 */
    uint32_t lost;            /* 序号缺口估算 */
    uint32_t last_seq;
    float    level;           /* 0..1 平滑电平 */
    float    peak;            /* 0..1 峰值保持 */
    float    db;              /* dBFS */
    int      kbps;            /* 瞬时码率 */
    int      latency_ms;      /* 估计延迟（帧间隔抖动） */
    uint64_t injected;        /* 已注入驱动的 PCM 字节 */
    int      device_ok;       /* 虚拟麦克风可写（复用同一句柄，不额外 CreateFile） */
    int      dev_handshake_ok;/* 驱动握手成功 */
    int      silent_frames;   /* 连续静音帧计数（VoiceMeter 转“无信号”用） */
    char     opus_src[16];    /* static / dll / none */
    char     last_error[160];
    DWORD    started_at;
} EngineStats;

/* 引擎线程生命周期 */
int  eng_start(const char *host, int ctrl_port, int rate_idx, int buffer_ms, int gain);
void eng_stop(void);
void eng_snapshot(EngineStats *out);   /* 线程安全快照 */
int  eng_device_available(void);       /* 不再独立开句柄：返回 devio_is_open() */
void eng_set_monitor(int on);          /* 本地监听（预留，当前无 waveOut 实现） */
/* 媒体（UDP）端口：0 = 由系统随机分配（默认，向后兼容）；1..65535 = 固定端口。
 * 跨网络/过防火墙时必须固定，否则手机端无从放行。必须在 eng_start 之前调用。 */
int  eng_set_media_port(int port);
int  eng_media_port(void);
void eng_set_feed_mode(int mode);      /* DEV_FEED_IOCTL（默认）/ DEV_FEED_WRITE */

/* 日志回调：引擎线程调用，UI 通过 PostMessage 转投 */
typedef void (*LogFn)(const char *line);
void eng_set_logger(LogFn fn);

/* 单实例回调：第二个实例的命令行被转发过来 */
typedef void (*SingleInstCb)(const wchar_t *cmdline);
typedef void (*SingleInstActivate)(void);

/* ------------------------------------------------------------- 单实例 */
/* 返回：1 = 本进程是唯一实例；0 = 已有实例在跑（命令行已转发，调用方应退出） */
int  singleinst_init(SingleInstCb cb, SingleInstActivate act);
void singleinst_shutdown(void);

/* ------------------------------------------------------------- CLI */
typedef struct {
    const wchar_t *host_pos;  /* --host 的原始（宽字符）值 */
    int         port;         /* --port，默认 8125 */
    int         media_port;   /* --media-port，0 = 随机分配（默认） */
    int         rate;         /* --rate 48000/16000/8000 → rate_idx 2/1/0 */
    int         rate_idx;
    int         buffer_ms;    /* --buffer 26|60|120 */
    int         gain;         /* --gain 0..200 */
    int         connect;      /* --connect */
    int         status;       /* --status */
    int         install;      /* --install-driver */
    int         uninstall;    /* --uninstall-driver */
    int         no_gui;       /* --no-gui */
    int         help;         /* --help */
    int         version;      /* --version */
    int         dark;         /* --dark */
    int         light;        /* --light */
    int         theme_set;
    int         feed_write;   /* --feed write：灌音走 WriteFile 而不是 IOCTL */
    int         console;      /* 输出是否需要走 stdout（已挂上控制台） */
    const wchar_t *driver_pack;  /* --driver-pack <dir> */
    int         argc;
    wchar_t   **argv;
} CliArgs;

#define CLI_EXIT_OK        0
#define CLI_EXIT_PARAM     1
#define CLI_EXIT_CONNECT   2
#define CLI_EXIT_DEVUNAVA  3
#define CLI_EXIT_DEVINST   4

int  cli_parse(CliArgs *a, int argc, wchar_t **argv);
const char *cli_parse_error(void);    /* cli_parse 返回 0 时的具体原因（不是 NULL） */
void cli_attach_console(void);        /* -mwindows 下的输出方案 */
void cli_detach_console(void);
int  cli_run(CliArgs *a);             /* 返回 1 表示需要退出进程（code 见 cli_exit_code） */
int  cli_exit_code(void);
void cli_print_help(void);
const char *cli_help_text(void);      /* GUI 说明书窗口复用同一份文本 */
void cli_set_defaults(CliArgs *a);

/* ------------------------------------------------------------- 界面 */
#define LOG_LINES 200
#define LOG_LEN   220

typedef struct { char text[LOG_LEN]; wchar_t ts[12]; int kind; } LogLine;

typedef struct {
    HWND  hwnd;
    HWND  edit_host, edit_port, edit_media;
    HBRUSH br_card;
    UINT  dpi;
    int   w, h;                /* 客户区逻辑尺寸 */
    int   scroll;              /* 纵向滚动偏移 */
    int   content_h;           /* 内容总高（布局后得出） */
    int   hover, pressed;
    int   drag_slider;
    int   seg_rate;            /* 0 48k 1 16k 2 8k */
    int   buf_idx;             /* 0 26ms 1 60ms 2 120ms */
    int   gain;                /* 0..200 % */
    int   dark;
    int   running;
    int   exp_audio, exp_stats, exp_log, exp_driver;  /* 折叠面板展开标志 */
    LogLine logs[LOG_LINES];
    int   log_head, log_count;
    EngineStats st;
    DrvStatus drv;
    float anim_phase;
    int   tick;
    int   installing;
} Ui;

extern Ui g_ui;

/* 命中 id */
enum {
    ID_NONE = 0, ID_MIN, ID_CLOSE, ID_THEME, ID_CONN,
    ID_RATE0, ID_RATE1, ID_RATE2,
    ID_BUF0, ID_BUF1, ID_BUF2,
    ID_SLIDER, ID_INSTALL, ID_UNINSTALL, ID_HELP, ID_HELP_CLOSE,
    ID_EXP_AUDIO, ID_EXP_STATS, ID_EXP_LOG, ID_EXP_DRIVER,
    ID_FEED0, ID_FEED1, ID_MAX_
};

/* 界面模块（详见各 .h） */
void ui_log_add(const char *s, int kind);
int  ui_log_count(void);
const LogLine *ui_log_get(int i);   /* i=0 为最新 */

void ui_layout_compute(void);
void ui_draw_all(HDC dc);
int  ui_hit_test(int x, int y);
void ui_on_click(int id, int x, int y);
void ui_on_release(void);
void ui_toggle_theme(void);
void ui_show_help(HINSTANCE hInst, HWND parent);
int  ui_help_handle_key(WPARAM vk);

#endif /* WOMIC_APP_H */
