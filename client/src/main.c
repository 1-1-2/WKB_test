/* main.c — WO Mic Link 客户端界面
 *
 * 纯 Win32 + GDI/GDI+ 自绘，无任何框架依赖。
 * 视觉规格：Fluent 风格（圆角卡片、分段控件、动态主题、Segoe MDL2 图标），
 * 窗口无系统边框，靠 DWM 提供圆角与阴影。支持 DPI 缩放与深浅主题。
 */
#define WIN32_LEAN_AND_MEAN
#include "app.h"
#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---------------------------------------------------------------- GDI+ 动态绑定
 * 不用 gdiplus.h（它含 C++ 类，C 编译不过），只手写需要的 flat API 并动态加载，
 * 好处：不增加导入表、不引入链接期依赖，gdiplus.dll 每台 Windows 都有。
 */
typedef struct { UINT32 ver; void *cb; BOOL no_thread; BOOL no_codecs; } GpStartupInput;
typedef int   GpStatus;
typedef void *GpGraphics, *GpPath, *GpBrush, *GpPen, *GpLineGrad;
typedef float REAL;

static struct {
    HMODULE mod;
    GpStatus (WINAPI *startup)(ULONG_PTR*, const GpStartupInput*, void**);
    GpStatus (WINAPI *shutdown)(ULONG_PTR);
    GpStatus (WINAPI *from_hdc)(HDC, GpGraphics*);
    GpStatus (WINAPI *del_graphics)(GpGraphics);
    GpStatus (WINAPI *set_smooth)(GpGraphics, int);
    GpStatus (WINAPI *create_path)(int, GpPath*);
    GpStatus (WINAPI *add_arc)(GpPath, REAL, REAL, REAL, REAL, REAL, REAL);
    GpStatus (WINAPI *close_fig)(GpPath);
    GpStatus (WINAPI *solid_fill)(UINT32, GpBrush*);
    GpStatus (WINAPI *line_brush)(const POINT*, const POINT*, UINT32, UINT32, int, GpLineGrad*);
    GpStatus (WINAPI *fill_path)(GpGraphics, GpBrush, GpPath);
    GpStatus (WINAPI *draw_path)(GpGraphics, GpPen, GpPath);
    GpStatus (WINAPI *create_pen)(UINT32, REAL, int, GpPen*);
    GpStatus (WINAPI *del_brush)(GpBrush);
    GpStatus (WINAPI *del_pen)(GpPen);
    GpStatus (WINAPI *del_path)(GpPath);
    ULONG_PTR token;
    int ready;
} gp;

#define GPA(field, sym, type) gp.field = (type)(void*)GetProcAddress(gp.mod, "Gdip" sym)

static void gp_init(void)
{
    gp.mod = LoadLibraryW(L"gdiplus.dll");
    if (!gp.mod) return;
    GPA(from_hdc,     "CreateFromHDC",      GpStatus (WINAPI*)(HDC, GpGraphics*));
    GPA(del_graphics, "DeleteGraphics",     GpStatus (WINAPI*)(GpGraphics));
    GPA(set_smooth,   "SetSmoothingMode",   GpStatus (WINAPI*)(GpGraphics, int));
    GPA(create_path,  "CreatePath",         GpStatus (WINAPI*)(int, GpPath*));
    GPA(add_arc,      "AddPathArc",         GpStatus (WINAPI*)(GpPath, REAL, REAL, REAL, REAL, REAL, REAL));
    GPA(close_fig,    "ClosePathFigure",    GpStatus (WINAPI*)(GpPath));
    GPA(solid_fill,   "CreateSolidFill",    GpStatus (WINAPI*)(UINT32, GpBrush*));
    GPA(fill_path,    "FillPath",           GpStatus (WINAPI*)(GpGraphics, GpBrush, GpPath));
    GPA(draw_path,    "DrawPath",           GpStatus (WINAPI*)(GpGraphics, GpPen, GpPath));
    GPA(create_pen,   "CreatePen1",         GpStatus (WINAPI*)(UINT32, REAL, int, GpPen*));
    GPA(del_brush,    "DeleteBrush",        GpStatus (WINAPI*)(GpBrush));
    GPA(del_pen,      "DeletePen",          GpStatus (WINAPI*)(GpPen));
    GPA(del_path,     "DeletePath",         GpStatus (WINAPI*)(GpPath));
    gp.startup  = (GpStatus (WINAPI*)(ULONG_PTR*, const GpStartupInput*, void**))
                  (void*)GetProcAddress(gp.mod, "GdiplusStartup");
    gp.shutdown = (GpStatus (WINAPI*)(ULONG_PTR))
                  (void*)GetProcAddress(gp.mod, "GdiplusShutdown");
    if (!gp.from_hdc || !gp.create_path || !gp.fill_path || !gp.startup) { gp.mod = NULL; return; }
    {
        GpStartupInput in = { 1, NULL, FALSE, FALSE };
        if (gp.startup(&gp.token, &in, NULL) != 0) { gp.mod = NULL; return; }
    }
    gp.ready = 1;
}

/* ---------------------------------------------------------------- 主题 */
Theme g_th;

void theme_apply(int dark)
{
    if (dark) {
        g_th.bg        = RGB(14, 15, 20);
        g_th.bg2       = RGB(20, 22, 29);
        g_th.card      = RGB(30, 33, 41);
        g_th.card_hi   = RGB(41, 45, 55);
        g_th.stroke    = RGB(52, 55, 66);
        g_th.text      = RGB(236, 239, 244);
        g_th.text_dim  = RGB(158, 165, 180);
        g_th.text_mute = RGB(108, 115, 130);
        g_th.accent    = RGB(86, 190, 255);
        g_th.accent_dim= RGB(38, 84, 112);
        g_th.ok        = RGB(72, 214, 122);
        g_th.warn      = RGB(255, 197, 92);
        g_th.err       = RGB(255, 110, 110);
        g_th.dark      = 1;
    } else {
        g_th.bg        = RGB(244, 246, 250);
        g_th.bg2       = RGB(234, 238, 245);
        g_th.card      = RGB(255, 255, 255);
        g_th.card_hi   = RGB(240, 243, 248);
        g_th.stroke    = RGB(222, 226, 234);
        g_th.text      = RGB(22, 24, 30);
        g_th.text_dim  = RGB(92, 99, 114);
        g_th.text_mute = RGB(140, 146, 160);
        g_th.accent    = RGB(0, 118, 210);
        g_th.accent_dim= RGB(198, 224, 246);
        g_th.ok        = RGB(16, 148, 78);
        g_th.warn      = RGB(176, 116, 0);
        g_th.err       = RGB(200, 42, 42);
        g_th.dark      = 0;
    }
}

/* ---------------------------------------------------------------- 应用状态 */
#define LOG_LINES 120
#define LOG_LEN   200

typedef struct { char text[LOG_LEN]; wchar_t ts[12]; int kind; } LogLine;

typedef struct {
    HWND hwnd, edit_host, edit_port;
    HBRUSH br_card;
    UINT dpi;
    int  w, h;                 /* 客户区逻辑尺寸 */
    int  hover;                /* 命中 id */
    int  pressed;
    int  drag_slider;
    int  seg_transport;        /* 0 wifi 1 usb 2 bt */
    int  seg_rate;             /* 0 48k 1 16k 2 8k */
    int  volume;               /* 0..200 % */
    int  dark;
    int  advanced;
    int  running;
    LogLine logs[LOG_LINES];
    int log_head, log_count;
    EngineStats st;
    DWORD t_start;
    float anim_phase;
    int  tick;
    RECT r_title, r_btn_min, r_btn_close, r_card1, r_card2, r_card3, r_btn_conn,
         r_seg[3], r_rate[3], r_slider, r_log, r_status, r_card_state, r_card_meter,
         r_card_stats, r_btn_theme, r_btn_adv, r_card_tips;
} App;

static App g;

/* 主题切换唯一入口：画刷必须同时重建，否则 EDIT 仍用旧底色（亮色下会变黑块） */
static void apply_theme(int dark)
{
    g.dark = dark;
    theme_apply(dark);
    if (g.br_card) DeleteObject(g.br_card);
    g.br_card = CreateSolidBrush(g_th.card);
    if (g.hwnd) InvalidateRect(g.hwnd, NULL, TRUE);
}



/* 控件 id（用于 hit test） */
enum { ID_NONE = 0, ID_MIN, ID_CLOSE, ID_SEG0, ID_SEG1, ID_SEG2,
       ID_RATE0, ID_RATE1, ID_RATE2, ID_CONN, ID_SLIDER, ID_THEME, ID_ADV, ID_LOGAREA };

static int sd(int v) { return MulDiv(v, (int)g.dpi, 96); }   /* scale by dpi */

/* ---------------------------------------------------------------- 日志 */
static void log_add(const char *s, int kind)
{
    LogLine *L = &g.logs[g.log_head];
    strncpy(L->text, s, LOG_LEN - 1);
    L->text[LOG_LEN - 1] = 0;
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf(L->ts, 12, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    }
    L->kind = kind;
    g.log_head = (g.log_head + 1) % LOG_LINES;
    if (g.log_count < LOG_LINES) g.log_count++;
}

static void eng_log_cb(const char *line)
{
    /* 引擎线程 → UI 线程：把字符串拷到堆上投递 */
    char *copy = (char*)malloc(LOG_LEN);
    if (!copy) return;
    strncpy(copy, line, LOG_LEN - 1);
    copy[LOG_LEN - 1] = 0;
    if (!PostMessageW(g.hwnd, WM_APP + 1, (WPARAM)copy, 0)) free(copy);
}

/* ---------------------------------------------------------------- 绘制工具 */
static UINT32 argb(COLORREF c, int a)
{
    return ((UINT32)a << 24) | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

/* 圆角矩形路径并填充/描边 */
static void fill_round(HDC dc, RECT r, int rad, COLORREF c, int alpha)
{
    if (!gp.ready) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); return; }
    GpGraphics gfx = NULL; GpPath p = NULL; GpBrush b = NULL;
    gp.from_hdc(dc, &gfx);
    gp.set_smooth(gfx, 4);            /* SmoothingModeAntiAlias */
    gp.create_path(0, &p);            /* FillModeAlternate */
    if (rad <= 0) {
        gp.add_arc(p, (REAL)r.left, (REAL)r.top, 0.1f, 0.1f, 0, 0);
    } else {
        REAL x = (REAL)r.left, y = (REAL)r.top;
        REAL w = (REAL)(r.right - r.left), h = (REAL)(r.bottom - r.top);
        REAL d = (REAL)(rad * 2);
        gp.add_arc(p, x, y, d, d, 180, 90);
        gp.add_arc(p, x + w - d, y, d, d, 270, 90);
        gp.add_arc(p, x + w - d, y + h - d, d, d, 0, 90);
        gp.add_arc(p, x, y + h - d, d, d, 90, 90);
    }
    gp.close_fig(p);
    gp.solid_fill(argb(c, alpha), &b);
    gp.fill_path(gfx, b, p);
    gp.del_brush(b); gp.del_path(p); gp.del_graphics(gfx);
}

static void stroke_round(HDC dc, RECT r, int rad, COLORREF c, int alpha, int width)
{
    if (!gp.ready) return;
    GpGraphics gfx = NULL; GpPath p = NULL; GpPen pen = NULL;
    gp.from_hdc(dc, &gfx);
    gp.set_smooth(gfx, 4);
    gp.create_path(0, &p);
    REAL x = (REAL)r.left, y = (REAL)r.top;
    REAL w = (REAL)(r.right - r.left) - 1, h = (REAL)(r.bottom - r.top) - 1;
    REAL d = (REAL)(rad * 2);
    gp.add_arc(p, x, y, d, d, 180, 90);
    gp.add_arc(p, x + w - d, y, d, d, 270, 90);
    gp.add_arc(p, x + w - d, y + h - d, d, d, 0, 90);
    gp.add_arc(p, x, y + h - d, d, d, 90, 90);
    gp.close_fig(p);
    gp.create_pen(argb(c, alpha), (REAL)width, 0, &pen);
    gp.draw_path(gfx, pen, p);
    gp.del_pen(pen); gp.del_path(p); gp.del_graphics(gfx);
}

/* 垂直渐变背景（GDI 逐行，成本可忽略） */
static void bg_gradient(HDC dc, RECT r)
{
    int n = r.bottom - r.top;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n ? n : 1);
        int R = (int)(GetRValue(g_th.bg) * (1 - t) + GetRValue(g_th.bg2) * t);
        int G = (int)(GetGValue(g_th.bg) * (1 - t) + GetGValue(g_th.bg2) * t);
        int B = (int)(GetBValue(g_th.bg) * (1 - t) + GetBValue(g_th.bg2) * t);
        RECT line = { r.left, r.top + i, r.right, r.top + i + 1 };
        HBRUSH b = CreateSolidBrush(RGB(R, G, B));
        FillRect(dc, &line, b);
        DeleteObject(b);
    }
}

/* 左上角柔光 */
static void bg_glow(HDC dc, int cx, int cy, int radius, COLORREF c, int alpha)
{
    if (!gp.ready) return;
    GpGraphics gfx = NULL; GpPath p = NULL; GpBrush b = NULL;
    gp.from_hdc(dc, &gfx);
    gp.set_smooth(gfx, 4);
    gp.create_path(0, &p);
    int steps = 26;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / steps;
        int rr = (int)(radius * (1 - t * 0.85f));
        int a = (int)(alpha * (1 - t) * 0.10f);
        RECT rc = { cx - rr, cy - rr, cx + rr, cy + rr };
        fill_round(dc, rc, rr, c, a);
    }
    gp.del_path(p); gp.del_graphics(gfx);
}

/* 文字 */
static HFONT font(int size_pt, int weight, int mono)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(size_pt, (int)g.dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, mono ? L"Consolas" : L"Segoe UI");
    return CreateFontIndirectW(&lf);
}


static void text_at(HDC dc, RECT r, const wchar_t *s, HFONT f, COLORREF c, UINT fmt)
{
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

static void text_u8(HDC dc, RECT r, const char *s, HFONT f, COLORREF c, UINT fmt)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    text_at(dc, r, w, f, c, fmt);
}

/* ---------------------------------------------------------------- 手绘图标
 * 不用 Segoe MDL2 字形：字体缺失时会变方块，且图标细节不可控。
 * 全部用 GDI 线条绘制，2px 笔宽，视觉上与 Fluent 线性图标一致。
 */
enum { IC_WIFI = 0, IC_USB, IC_BT, IC_MIC, IC_SEARCH, IC_MIN, IC_CLOSE, IC_SUN, IC_MOON };

static void arc_deg(HDC dc, int cx, int cy, int rad, double a0, double a1)
{
    double r0 = a0 * 3.14159265 / 180.0, r1 = a1 * 3.14159265 / 180.0;
    int x0 = (int)(cx + rad * cos(r0)), y0 = (int)(cy - rad * sin(r0));
    int x1 = (int)(cx + rad * cos(r1)), y1 = (int)(cy - rad * sin(r1));
    Arc(dc, cx - rad, cy - rad, cx + rad, cy + rad, x0, y0, x1, y1);
}

static void icon_draw(HDC dc, RECT r, int kind, COLORREF c, int w)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int s = (r.right - r.left) < (r.bottom - r.top) ? (r.right - r.left) : (r.bottom - r.top);
    int h = s / 2;
    HPEN pen = CreatePen(PS_SOLID, w, c);
    HPEN old = (HPEN)SelectObject(dc, pen);
    SetBkMode(dc, TRANSPARENT);

    switch (kind) {
    case IC_WIFI:
        arc_deg(dc, cx, cy + h / 3, h, 35, 145);
        arc_deg(dc, cx, cy + h / 3, h * 2 / 3, 35, 145);
        arc_deg(dc, cx, cy + h / 3, h / 3, 35, 145);
        break;
    case IC_USB:
        MoveToEx(dc, cx, cy - h, NULL); LineTo(dc, cx, cy + h);
        Ellipse(dc, cx - w, cy - h - w, cx + w, cy - h + w);
        MoveToEx(dc, cx, cy + h - h / 3, NULL); LineTo(dc, cx - h / 2, cy + h / 3);
        MoveToEx(dc, cx, cy + h - h / 3, NULL); LineTo(dc, cx + h / 2, cy + h / 3);
        MoveToEx(dc, cx - h / 2, cy + h / 3, NULL); LineTo(dc, cx - h / 2, cy + h);
        break;
    case IC_BT:
        MoveToEx(dc, cx - h / 2, cy - h / 2, NULL); LineTo(dc, cx + h / 2, cy + h / 2);
        LineTo(dc, cx, cy + h); LineTo(dc, cx, cy - h);
        LineTo(dc, cx + h / 2, cy - h / 2); LineTo(dc, cx - h / 2, cy + h / 2);
        break;
    case IC_MIC:
        RoundRect(dc, cx - h / 2, cy - h, cx + h / 2, cy + h / 4, h, h);
        arc_deg(dc, cx, cy + h / 4, h * 3 / 4, 200, 340);
        MoveToEx(dc, cx, cy + h * 3 / 4, NULL); LineTo(dc, cx, cy + h);
        break;
    case IC_SEARCH:
        Ellipse(dc, cx - h, cy - h, cx + h / 2, cy + h / 2);
        MoveToEx(dc, cx + h / 3, cy + h / 3, NULL); LineTo(dc, cx + h, cy + h);
        break;
    case IC_MIN:
        MoveToEx(dc, cx - h / 2, cy, NULL); LineTo(dc, cx + h / 2, cy);
        break;
    case IC_CLOSE:
        MoveToEx(dc, cx - h / 2, cy - h / 2, NULL); LineTo(dc, cx + h / 2, cy + h / 2);
        MoveToEx(dc, cx + h / 2, cy - h / 2, NULL); LineTo(dc, cx - h / 2, cy + h / 2);
        break;
    case IC_SUN: {
        Ellipse(dc, cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2);
        for (int i = 0; i < 8; i++) {
            double a = i * 45.0 * 3.14159265 / 180.0;
            int x0 = (int)(cx + (h * 0.72) * cos(a)), y0 = (int)(cy + (h * 0.72) * sin(a));
            int x1 = (int)(cx + h * cos(a)), y1 = (int)(cy + h * sin(a));
            MoveToEx(dc, x0, y0, NULL); LineTo(dc, x1, y1);
        }
        break;
    }
    case IC_MOON:
        arc_deg(dc, cx, cy, h, 40, 320);
        arc_deg(dc, cx + h / 2, cy - h / 4, h * 3 / 4, 150, 250);
        break;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* ---------------------------------------------------------------- 布局 */
static void layout(void)
{
    int pad = sd(20);
    int title_h = sd(52);
    int status_h = sd(30);
    int left_w = sd(392);
    int gap = sd(16);
    int y;

    g.r_title.left = 0; g.r_title.top = 0;
    g.r_title.right = g.w; g.r_title.bottom = sd(52);

    int bw = sd(46);
    g.r_btn_close.right = g.w - sd(8);
    g.r_btn_close.left = g.r_btn_close.right - bw;
    g.r_btn_close.top = sd(6); g.r_btn_close.bottom = sd(48);
    g.r_btn_min.right = g.r_btn_close.left - sd(2);
    g.r_btn_min.left = g.r_btn_min.right - bw;
    g.r_btn_min.top = g.r_btn_close.top; g.r_btn_min.bottom = g.r_btn_close.bottom;

    g.r_btn_theme.right = g.r_btn_min.left - sd(6);
    g.r_btn_theme.left = g.r_btn_theme.right - sd(34);
    g.r_btn_theme.top = sd(12); g.r_btn_theme.bottom = sd(42);

    /* 左列 */
    int lx = pad, lw = left_w;
    y = title_h + sd(8);
    g.r_card1.left = lx; g.r_card1.right = lx + lw;
    g.r_card1.top = y; g.r_card1.bottom = y + sd(104);

    y = g.r_card1.bottom + gap;
    g.r_card2.left = lx; g.r_card2.right = lx + lw;
    g.r_card2.top = y; g.r_card2.bottom = y + sd(132);

    y = g.r_card2.bottom + gap;
    g.r_card3.left = lx; g.r_card3.right = lx + lw;
    g.r_card3.top = y; g.r_card3.bottom = y + sd(136);

    y = g.r_card3.bottom + gap;
    g.r_btn_conn.left = lx; g.r_btn_conn.right = lx + lw;
    g.r_btn_conn.top = y; g.r_btn_conn.bottom = y + sd(46);

    y = g.r_btn_conn.bottom + gap;
    g.r_card_tips.left = lx; g.r_card_tips.right = lx + lw;
    g.r_card_tips.top = y; g.r_card_tips.bottom = g.h - status_h - sd(8);

    /* 分段控件（三格） */
    int inner = sd(12);
    int segy = g.r_card1.top + sd(36);
    int segw = (lw - inner * 2) / 3;
    for (int i = 0; i < 3; i++) {
        g.r_seg[i].left = lx + inner + i * segw;
        g.r_seg[i].right = g.r_seg[i].left + segw - sd(4);
        g.r_seg[i].top = segy; g.r_seg[i].bottom = segy + sd(34);
    }
    int ry = g.r_card3.top + sd(40);
    for (int i = 0; i < 3; i++) {
        g.r_rate[i].left = lx + inner + i * segw;
        g.r_rate[i].right = g.r_rate[i].left + segw - sd(4);
        g.r_rate[i].top = ry; g.r_rate[i].bottom = ry + sd(30);
    }
    g.r_slider.left = lx + inner; g.r_slider.right = lx + lw - inner;
    g.r_slider.top = g.r_card3.top + sd(98);
    g.r_slider.bottom = g.r_slider.top + sd(22);

    /* 右列 */
    int rx = lx + lw + gap;
    int rw = g.w - rx - pad;
    int ry0 = title_h + sd(8);
    g.r_card_state.left = rx; g.r_card_state.right = rx + rw;
    g.r_card_state.top = ry0; g.r_card_state.bottom = ry0 + sd(92);

    y = g.r_card_state.bottom + gap;
    g.r_card_meter.left = rx; g.r_card_meter.right = rx + rw;
    g.r_card_meter.top = y; g.r_card_meter.bottom = y + sd(196);

    y = g.r_card_meter.bottom + gap;
    g.r_card_stats.left = rx; g.r_card_stats.right = rx + rw;
    g.r_card_stats.top = y; g.r_card_stats.bottom = y + sd(92);

    y = g.r_card_stats.bottom + gap;
    g.r_log.left = rx; g.r_log.right = rx + rw;
    g.r_log.top = y; g.r_log.bottom = g.h - status_h - sd(10);

    g.r_status.left = 0; g.r_status.right = g.w;
    g.r_status.top = g.h - status_h; g.r_status.bottom = g.h;
}

/* 输入框定位（原生 EDIT，跟随卡片布局） */
static void place_edits(void)
{
    int inner = sd(12);
    int x = g.r_card2.left + inner;
    int w = g.r_card2.right - g.r_card2.left - inner * 2;
    int h = sd(30);
    int y = g.r_card2.top + sd(34);
    int portw = sd(88);
    MoveWindow(g.edit_host, x + sd(34), y, w - sd(34) - portw - sd(8), h, TRUE);
    MoveWindow(g.edit_port, x + w - portw, y, portw, h, TRUE);
}

/* ---------------------------------------------------------------- 绘制：卡片 */
static void draw_card(HDC dc, RECT r, const wchar_t *title, const wchar_t *hint)
{
    fill_round(dc, r, sd(12), g_th.card, 255);
    stroke_round(dc, r, sd(12), g_th.stroke, g_th.dark ? 150 : 200, 1);
    if (title) {
        RECT tr = { r.left + sd(16), r.top + sd(11), r.right - sd(16), r.top + sd(31) };
        HFONT f = font(9, FW_SEMIBOLD, 0);
        text_at(dc, tr, title, f, g_th.text_dim, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f);
    }
    if (hint) {
        RECT tr = { r.left + sd(16), r.top + sd(11), r.right - sd(16), r.top + sd(31) };
        HFONT f = font(8, FW_NORMAL, 0);
        text_at(dc, tr, hint, f, g_th.text_mute, DT_RIGHT | DT_SINGLELINE);
        DeleteObject(f);
    }
}

/* 分段按钮 */
static void draw_seg(HDC dc, RECT r, int icon_kind, const wchar_t *label, int active, int id)
{
    int hov = (g.hover == id);
    RECT rr = r;
    if (active) {
        fill_round(dc, rr, sd(8), g_th.accent, 255);
    } else if (hov) {
        fill_round(dc, rr, sd(8), g_th.card_hi, 255);
    }
    COLORREF fg = active ? (g_th.dark ? RGB(12, 20, 26) : RGB(255, 255, 255)) : g_th.text_dim;

    int iw = icon_kind >= 0 ? sd(16) : 0;
    if (icon_kind >= 0) {
        RECT ir = { rr.left + sd(11), rr.top + sd(9), rr.left + sd(11) + iw, rr.bottom - sd(9) };
        icon_draw(dc, ir, icon_kind, fg, sd(2) < 1 ? 1 : sd(2));
    }
    RECT tr = { rr.left + sd(11) + iw + (icon_kind >= 0 ? sd(6) : 0), rr.top, rr.right - sd(6), rr.bottom };
    HFONT f = font(9, active ? FW_SEMIBOLD : FW_NORMAL, 0);
    text_at(dc, tr, label, f, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f);
}

/* 主按钮 */
static void draw_button(HDC dc, RECT r, const wchar_t *label, int id, int danger)
{
    int hov = (g.hover == id);
    int down = (g.pressed == id);
    COLORREF base = danger ? g_th.err : g_th.accent;
    if (down) base = RGB(GetRValue(base) * 78 / 100, GetGValue(base) * 78 / 100, GetBValue(base) * 78 / 100);
    else if (hov) base = RGB(min(255, GetRValue(base) + 18), min(255, GetGValue(base) + 18), min(255, GetBValue(base) + 18));

    fill_round(dc, r, sd(10), base, 255);
    COLORREF fg = g_th.dark ? RGB(10, 18, 24) : RGB(255, 255, 255);
    if (danger) fg = RGB(255, 255, 255);
    HFONT f = font(10, FW_SEMIBOLD, 0);
    text_at(dc, r, label, f, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f);
}

/* ---------------------------------------------------------------- 绘制：各卡片 */
static void draw_state_card(HDC dc)
{
    RECT r = g.r_card_state;
    fill_round(dc, r, sd(12), g_th.card, 255);
    stroke_round(dc, r, sd(12), g_th.stroke, g_th.dark ? 150 : 200, 1);

    COLORREF dot = g_th.text_mute;
    const wchar_t *st = L"未连接";
    if (g.st.state == ST_CONNECTING) { dot = g_th.warn; st = L"连接中…"; }
    else if (g.st.state == ST_HANDSHAKE) { dot = g_th.warn; st = L"握手中…"; }
    else if (g.st.state == ST_STREAMING) { dot = g_th.ok; st = L"已连接"; }
    else if (g.st.state == ST_ERROR) { dot = g_th.err; st = L"连接失败"; }

    int cx = r.left + sd(26), cy = r.top + sd(32);
    /* 状态点 + 呼吸光晕 */
    if (g.st.state == ST_STREAMING) {
        int rp = sd(14) + (int)(sin(g.anim_phase) * sd(3));
        fill_round(dc, (RECT){ cx - rp, cy - rp, cx + rp, cy + rp }, rp, dot, 40);
    }
    int d = sd(6);
    fill_round(dc, (RECT){ cx - d, cy - d, cx + d, cy + d }, d, dot, 255);

    RECT tr = { cx + sd(16), r.top + sd(14), r.right - sd(16), r.top + sd(48) };
    HFONT f1 = font(15, FW_SEMIBOLD, 0);
    text_at(dc, tr, st, f1, g_th.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f1);

    /* 副行：地址或错误 */
    wchar_t sub[192];
    if (g.st.state == ST_ERROR && g.st.last_error[0])
        MultiByteToWideChar(CP_UTF8, 0, g.st.last_error, -1, sub, 192);
    else if (g.st.peer[0])
        MultiByteToWideChar(CP_UTF8, 0, g.st.peer, -1, sub, 192);
    else
        wcscpy(sub, L"输入手机地址后连接");

    RECT sr = { cx + sd(16), r.top + sd(44), r.right - sd(16), r.top + sd(64) };
    HFONT f2 = font(9, FW_NORMAL, 0);
    text_at(dc, sr, sub, f2, g.st.state == ST_ERROR ? g_th.err : g_th.text_dim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    DeleteObject(f2);

    /* 右上角：计时 */
    if (g.st.state == ST_STREAMING) {
        DWORD el = (GetTickCount() - g.st.started_at) / 1000;
        wchar_t tm[32];
        swprintf(tm, 32, L"%02u:%02u:%02u", el / 3600, (el / 60) % 60, el % 60);
        RECT tr2 = { r.right - sd(120), r.top + sd(18), r.right - sd(16), r.top + sd(40) };
        HFONT f3 = font(11, FW_NORMAL, 1);
        text_at(dc, tr2, tm, f3, g_th.text_dim, DT_RIGHT | DT_SINGLELINE);
        DeleteObject(f3);
    }
}

static void draw_meter(HDC dc)
{
    RECT r = g.r_card_meter;
    draw_card(dc, r, L"输入电平", NULL);

    int x0 = r.left + sd(16), x1 = r.right - sd(16);
    int bar_top = r.top + sd(36), bar_bot = r.bottom - sd(30);
    int n = 34;
    int gap = sd(2);
    int bw = ((x1 - x0) - gap * (n - 1)) / n;
    if (bw < 2) bw = 2;

    float level = g.st.level;
    if (level < 0) level = 0; if (level > 1) level = 1;
    int lit = (int)(level * n);
    int peak_idx = (int)(g.st.peak * n);

    for (int i = 0; i < n; i++) {
        RECT br = { x0 + i * (bw + gap), bar_top, x0 + i * (bw + gap) + bw, bar_bot };
        COLORREF c = g_th.dark ? RGB(48, 52, 63) : RGB(226, 231, 239);
        if (i < lit) {
            float t = (float)i / n;
            if (t > 0.86f) c = g_th.err;
            else if (t > 0.66f) c = g_th.warn;
            else c = g_th.accent;
        }
        fill_round(dc, br, sd(2), c, 255);
    }

    /* 峰值标记线 */
    if (peak_idx > 0 && peak_idx <= n) {
        int px = x0 + peak_idx * (bw + gap);
        RECT pr = { px - sd(1), bar_top, px + bw + sd(1), bar_top + sd(2) };
        fill_round(dc, pr, sd(1), g_th.text_mute, 220);
    }

    /* 底部读数 */
    wchar_t s[64];
    if (g.st.frames && g.st.db < -0.05f)
        swprintf(s, 64, L"%.1f dB", g.st.db);
    else
        wcscpy(s, L"— dB");
    RECT tr = { r.left + sd(16), r.bottom - sd(26), r.left + sd(120), r.bottom - sd(8) };
    HFONT f = font(9, FW_NORMAL, 1);
    text_at(dc, tr, s, f, g_th.text_dim, DT_LEFT | DT_SINGLELINE);
    DeleteObject(f);

    wchar_t sr[64];
    swprintf(sr, 64, L"峰值 %.0f%%", g.st.peak * 100.0f);
    RECT tr2 = { r.right - sd(140), r.bottom - sd(26), r.right - sd(16), r.bottom - sd(8) };
    /* 注意：不能复用上面已 DeleteObject 的 f —— 失效句柄会让 GDI 回退到不含
     * 中文字形的系统字体，"峰值"会渲染成方块（实测踩过）。 */
    HFONT f2 = font(9, FW_NORMAL, 1);
    text_at(dc, tr2, sr, f2, g_th.text_mute, DT_RIGHT | DT_SINGLELINE);
    DeleteObject(f2);
}

static void draw_stats(HDC dc)
{
    RECT r = g.r_card_stats;
    draw_card(dc, r, L"会话统计", NULL);

    struct { const wchar_t *k; wchar_t v[32]; } items[4];
    swprintf(items[0].v, 32, L"%d kbps", g.st.kbps);
    items[0].k = L"码率";
    swprintf(items[1].v, 32, L"%llu", (unsigned long long)g.st.frames);
    items[1].k = L"帧数";
    swprintf(items[2].v, 32, L"%u", g.st.lost);
    items[2].k = L"丢包";
    swprintf(items[3].v, 32, L"%d ms", g.st.latency_ms);
    items[3].k = L"延迟";

    int cw = (r.right - r.left - sd(32)) / 4;
    int y = r.top + sd(34);
    for (int i = 0; i < 4; i++) {
        int x = r.left + sd(16) + i * cw;
        RECT vr = { x, y, x + cw, y + sd(24) };
        HFONT f1 = font(12, FW_SEMIBOLD, 0);
        text_at(dc, vr, items[i].v, f1, g_th.text, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f1);
        RECT kr = { x, y + sd(22), x + cw, y + sd(42) };
        HFONT f2 = font(8, FW_NORMAL, 0);
        text_at(dc, kr, items[i].k, f2, g_th.text_mute, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f2);
        if (i) {
            RECT ln = { x - sd(8), y + sd(2), x - sd(8) + 1, y + sd(36) };
            fill_round(dc, ln, 0, g_th.stroke, 120);
        }
    }
}

static void draw_log(HDC dc)
{
    RECT r = g.r_log;
    draw_card(dc, r, L"事件", NULL);

    int y = r.top + sd(34);
    int bottom = r.bottom - sd(8);
    int line_h = sd(17);
    int shown = 0;
    HFONT f = font(8, FW_NORMAL, 1);

    /* 从最新往回画 */
    for (int i = 0; i < g.log_count; i++) {
        int idx = (g.log_head - 1 - i + LOG_LINES * 2) % LOG_LINES;
        LogLine *L = &g.logs[idx];
        if (y + line_h > bottom) break;
        RECT tr = { r.left + sd(16), y, r.left + sd(52), y + line_h };
        text_at(dc, tr, L->ts, f, g_th.text_mute, DT_LEFT | DT_SINGLELINE);

        RECT mr = { r.left + sd(56), y, r.right - sd(16), y + line_h };
        COLORREF c = L->kind == 1 ? g_th.err : L->kind == 2 ? g_th.ok : g_th.text_dim;
        text_u8(dc, mr, L->text, f, c, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line_h;
        shown++;
    }
    DeleteObject(f);
    if (!shown) {
        RECT tr = { r.left + sd(16), r.top + sd(38), r.right - sd(16), r.top + sd(60) };
        HFONT f2 = font(9, FW_NORMAL, 0);
        text_at(dc, tr, L"暂无事件", f2, g_th.text_mute, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f2);
    }
}

static void draw_titlebar(HDC dc)
{
    int pad = sd(20);
    /* 应用图标：圆角方块 + 声波 */
    RECT ir = { pad, sd(12), pad + sd(28), sd(40) };
    fill_round(dc, ir, sd(8), g_th.accent, 255);
    {
        RECT iw = { ir.left + sd(7), ir.top + sd(7), ir.right - sd(7), ir.bottom - sd(7) };
        icon_draw(dc, iw, IC_MIC, g_th.dark ? RGB(10, 18, 24) : RGB(255, 255, 255), sd(2));
    }

    RECT tr = { pad + sd(38), sd(12), pad + sd(200), sd(42) };
    HFONT ft = font(11, FW_SEMIBOLD, 0);
    text_at(dc, tr, L"WO Mic Link", ft, g_th.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(ft);

    RECT vr = { pad + sd(133), sd(13), pad + sd(220), sd(41) };
    HFONT fv = font(8, FW_NORMAL, 0);
    text_at(dc, vr, L"1.0", fv, g_th.text_mute, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(fv);

    /* 窗口按钮 */
    int hov_min = (g.hover == ID_MIN), hov_cls = (g.hover == ID_CLOSE);
    if (hov_min) fill_round(dc, g.r_btn_min, sd(6), g_th.card_hi, 200);
    if (hov_cls) fill_round(dc, g.r_btn_close, sd(6), g_th.err, 230);
    {
        RECT ir2 = { g.r_btn_min.left + sd(14), g.r_btn_min.top + sd(14),
                     g.r_btn_min.right - sd(14), g.r_btn_min.bottom - sd(14) };
        icon_draw(dc, ir2, IC_MIN, hov_min ? g_th.text : g_th.text_dim, sd(2) < 1 ? 1 : sd(2));
        RECT ir3 = { g.r_btn_close.left + sd(14), g.r_btn_close.top + sd(14),
                     g.r_btn_close.right - sd(14), g.r_btn_close.bottom - sd(14) };
        icon_draw(dc, ir3, IC_CLOSE, hov_cls ? RGB(255, 255, 255) : g_th.text_dim, sd(2) < 1 ? 1 : sd(2));
    }

    /* 主题切换 */
    RECT th = g.r_btn_theme;
    if (g.hover == ID_THEME) fill_round(dc, th, sd(6), g_th.card_hi, 200);
    {
        RECT ir4 = { th.left + sd(9), th.top + sd(9), th.right - sd(9), th.bottom - sd(9) };
        icon_draw(dc, ir4, g.dark ? IC_SUN : IC_MOON, g_th.text_dim, sd(2) < 1 ? 1 : sd(2));
    }
}

static void draw_statusbar(HDC dc)
{
    RECT r = g.r_status;
    fill_round(dc, r, 0, g_th.card, 255);
    RECT ln = { 0, r.top, g.w, r.top + 1 };
    fill_round(dc, ln, 0, g_th.stroke, 120);

    wchar_t left[128];
    const wchar_t *eng = g.running ? L"引擎运行中" : L"引擎空闲";
    swprintf(left, 128, L"%ls", eng);
    RECT lr = { sd(20), r.top, sd(300), r.bottom };
    HFONT f = font(8, FW_NORMAL, 0);
    text_at(dc, lr, left, f, g_th.text_mute, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t right[192];
    int dev = g.st.device_ok;
    swprintf(right, 192, L"虚拟麦克风 %ls   ·   解码器 %ls   ·   注入 %d KB",
             dev ? L"就绪" : L"未安装",
             L"Opus(可选)",
             g.st.injected / 1024);
    RECT rr = { g.w - sd(460), r.top, g.w - sd(20), r.bottom };
    text_at(dc, rr, right, f, dev ? g_th.text_mute : g_th.warn,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f);
}

static void draw_ui(HDC dc)
{
    RECT full = { 0, 0, g.w, g.h };
    bg_gradient(dc, full);

    draw_titlebar(dc);

    /* ---- 左列 ---- */
    draw_card(dc, g.r_card1, L"连接方式", NULL);
    draw_seg(dc, g.r_seg[0], IC_WIFI, L"Wi-Fi", g.seg_transport == 0, ID_SEG0);
    draw_seg(dc, g.r_seg[1], IC_USB, L"USB", g.seg_transport == 1, ID_SEG1);
    draw_seg(dc, g.r_seg[2], IC_BT, L"蓝牙", g.seg_transport == 2, ID_SEG2);

    draw_card(dc, g.r_card2, L"设备地址", L"控制端口 8125");
    {
        RECT ir = { g.r_card2.left + sd(16), g.r_card2.top + sd(40),
                    g.r_card2.left + sd(40), g.r_card2.top + sd(58) };
        icon_draw(dc, ir, IC_SEARCH, g_th.text_mute, sd(2) < 1 ? 1 : sd(2));
        /* 输入框描边 */
        RECT hr = { g.edit_host ? 0 : 0, 0, 0, 0 };
        (void)hr;
        RECT box = { g.r_card2.left + sd(14), g.r_card2.top + sd(32),
                     g.r_card2.right - sd(14), g.r_card2.top + sd(66) };
        stroke_round(dc, box, sd(8), g_th.stroke, 255, 1);
        RECT hint = { g.r_card2.left + sd(20), g.r_card2.top + sd(74),
                      g.r_card2.right - sd(20), g.r_card2.top + sd(92) };
        HFONT f = font(8, FW_NORMAL, 0);
        text_at(dc, hint, L"手机与电脑需在同一局域网", f, g_th.text_mute, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f);
    }

    draw_card(dc, g.r_card3, L"音频", L"Opus · 20ms 帧");
    draw_seg(dc, g.r_rate[0], -1, L"48 kHz", g.seg_rate == 0, ID_RATE0);
    draw_seg(dc, g.r_rate[1], -1, L"16 kHz", g.seg_rate == 1, ID_RATE1);
    draw_seg(dc, g.r_rate[2], -1, L"8 kHz", g.seg_rate == 2, ID_RATE2);
    {
        wchar_t v[64];
        swprintf(v, 64, L"本地增益 %d%%", g.volume);
        RECT vr = { g.r_card3.left + sd(16), g.r_card3.top + sd(74),
                    g.r_card3.right - sd(16), g.r_card3.top + sd(92) };
        HFONT f = font(8, FW_NORMAL, 0);
        text_at(dc, vr, v, f, g_th.text_mute, DT_LEFT | DT_SINGLELINE);
        DeleteObject(f);

        RECT tr = g.r_slider;
        tr.bottom = tr.top + sd(6);
        tr.top += sd(0);
        fill_round(dc, tr, sd(3), g_th.card_hi, 255);
        int frac = g.volume > 0 ? g.volume : 0;
        RECT fillr = tr;
        fillr.right = tr.left + (tr.right - tr.left) * frac / 200;
        fill_round(dc, fillr, sd(3), g_th.accent, 255);
        int knob_x = tr.left + (tr.right - tr.left) * frac / 200;
        int kr = sd(7);
        fill_round(dc, (RECT){ knob_x - kr, tr.top - sd(4), knob_x + kr, tr.bottom + sd(4) },
                   kr, g_th.card, 255);
        stroke_round(dc, (RECT){ knob_x - kr, tr.top - sd(4), knob_x + kr, tr.bottom + sd(4) },
                     kr, g_th.accent, 255, 2);
    }

    draw_button(dc, g.r_btn_conn, g.running ? L"断开连接" : L"连接", ID_CONN, g.running);

    /* 使用提示：填掉按钮下方空白，同时把关键前置条件写清楚 */
    {
        RECT tr = g.r_card_tips;
        if (tr.bottom > tr.top + sd(40)) {
            draw_card(dc, tr, L"开始之前", NULL);
            const wchar_t *tips[3] = {
                L"1. 手机与电脑接入同一 Wi-Fi",
                L"2. 手机端开启 WO Mic 并选 Wi-Fi",
                L"3. 填入手机 IP，点连接",
            };
            HFONT f = font(8, FW_NORMAL, 0);
            int ty = tr.top + sd(36);
            for (int i = 0; i < 3; i++) {
                RECT ir = { tr.left + sd(16), ty, tr.left + sd(24), ty + sd(16) };
                RECT rr = { tr.left + sd(26), ty, tr.right - sd(16), ty + sd(16) };
                fill_round(dc, (RECT){ ir.left + sd(2), ir.top + sd(6), ir.left + sd(5), ir.top + sd(9) },
                           sd(2), g_th.accent, 200);
                text_at(dc, rr, tips[i], f, g_th.text_dim, DT_LEFT | DT_SINGLELINE);
                ty += sd(19);
            }
            DeleteObject(f);
        }
    }

    /* ---- 右列 ---- */
    draw_state_card(dc);
    draw_meter(dc);
    draw_stats(dc);
    draw_log(dc);

    draw_statusbar(dc);
}

/* ---------------------------------------------------------------- 双缓冲呈现 */
static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g.w, g.h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    draw_ui(mem);

    BitBlt(dc, 0, 0, g.w, g.h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ---------------------------------------------------------------- 命中测试 */
static int hit_test(int x, int y)
{
    POINT p = { x, y };
    if (PtInRect(&g.r_btn_close, p)) return ID_CLOSE;
    if (PtInRect(&g.r_btn_min, p)) return ID_MIN;
    if (PtInRect(&g.r_btn_theme, p)) return ID_THEME;
    for (int i = 0; i < 3; i++) if (PtInRect(&g.r_seg[i], p)) return ID_SEG0 + i;
    for (int i = 0; i < 3; i++) if (PtInRect(&g.r_rate[i], p)) return ID_RATE0 + i;
    if (PtInRect(&g.r_btn_conn, p)) return ID_CONN;
    {
        RECT s = g.r_slider;
        InflateRect(&s, 0, sd(8));
        if (PtInRect(&s, p)) return ID_SLIDER;
    }
    return ID_NONE;
}

static void toggle_connection(void)
{
    if (g.running) {
        eng_stop();
        g.running = 0;
        log_add("已断开连接", 0);
    } else {
        wchar_t host[64] = {0}, port[16] = {0};
        GetWindowTextW(g.edit_host, host, 64);
        GetWindowTextW(g.edit_port, port, 16);
        char hostA[64];
        WideCharToMultiByte(CP_UTF8, 0, host, -1, hostA, 64, NULL, NULL);
        int cp = _wtoi(port);
        if (cp <= 0) cp = 8125;
        if (!hostA[0]) { log_add("请先输入手机地址", 1); InvalidateRect(g.hwnd, NULL, FALSE); return; }
        int rate_idx = g.seg_rate == 0 ? 2 : g.seg_rate == 1 ? 1 : 0;
        g.running = eng_start(hostA, cp, 0, rate_idx);
        log_add(g.running ? "正在建立会话…" : "启动引擎失败", g.running ? 0 : 1);
    }
    InvalidateRect(g.hwnd, NULL, FALSE);
}

/* ---------------------------------------------------------------- 窗口过程 */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        if (!g.dpi) {
            HDC dc = GetDC(hwnd);
            g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(hwnd, dc);
        }
        g.br_card = CreateSolidBrush(g_th.card);
        g.seg_transport = 0;
        g.seg_rate = 0;
        g.volume = 100;

        g.edit_host = CreateWindowExW(0, L"EDIT", L"192.168.1.100",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 10, 10, hwnd, (HMENU)100, NULL, NULL);
        g.edit_port = CreateWindowExW(0, L"EDIT", L"8125",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER,
            0, 0, 10, 10, hwnd, (HMENU)101, NULL, NULL);
        SetWindowTheme(g.edit_host, L" ", L" ");
        SetWindowTheme(g.edit_port, L" ", L" ");
        {
            HFONT f = font(10, FW_NORMAL, 0);
            SendMessageW(g.edit_host, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(g.edit_port, WM_SETFONT, (WPARAM)f, TRUE);
        }
        eng_set_logger(eng_log_cb);
        log_add("客户端已启动，等待连接", 0);
        SetTimer(hwnd, 1, 16, NULL);
        return 0;
    }
    case WM_SIZE:
        g.w = LOWORD(lp); g.h = HIWORD(lp);
        layout(); place_edits();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_TIMER: {
        EngineStats st;
        eng_snapshot(&st);
        int was = g.running;
        g.st = st;
        /* 引擎自行结束时同步 UI 状态 */
        if (g.running && (st.state == ST_IDLE || st.state == ST_ERROR)) {
            if (st.state == ST_ERROR) { g.running = 0; log_add(st.last_error, 1); }
            else { g.running = 0; }
        }
        g.anim_phase += 0.06f;
        if (g.anim_phase > 6.283f) g.anim_phase -= 6.283f;
        g.tick++;
        if (g.running || was) InvalidateRect(hwnd, NULL, FALSE);
        else if (st.state != ST_IDLE) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_APP + 2:
        if (!g.running) toggle_connection();
        return 0;
    case WM_APP + 1: {
        char *s = (char*)wp;
        log_add(s, 0);
        free(s);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int h = hit_test(x, y);
        if (h != g.hover) {
            g.hover = h;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g.drag_slider) {
            int w = g.r_slider.right - g.r_slider.left;
            int v = (x - g.r_slider.left) * 200 / (w ? w : 1);
            if (v < 0) v = 0; if (v > 200) v = 200;
            g.volume = v;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        /* 手型光标 */
        SetCursor(LoadCursorW(NULL, (LPCWSTR)((h == ID_SLIDER) ? IDC_SIZEWE :
                              (h != ID_NONE) ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = ID_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int h = hit_test(x, y);
        g.pressed = h;
        if (h == ID_SLIDER) {
            g.drag_slider = 1;
            int w = g.r_slider.right - g.r_slider.left;
            int v = (x - g.r_slider.left) * 200 / (w ? w : 1);
            if (v < 0) v = 0; if (v > 200) v = 200;
            g.volume = v;
        }
        if (h >= ID_SEG0 && h <= ID_SEG2) g.seg_transport = h - ID_SEG0;
        if (h >= ID_RATE0 && h <= ID_RATE2) g.seg_rate = h - ID_RATE0;
        if (h == ID_CONN) toggle_connection();
        if (h == ID_THEME) { apply_theme(!g.dark); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        g.pressed = 0; g.drag_slider = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, g_th.text);
        SetBkColor(dc, g_th.card);
        return (LRESULT)g.br_card;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) toggle_connection();
        return 0;
    case WM_NCCALCSIZE:
        if (wp) return 0;        /* 去掉系统标题栏，内容铺满 */
        break;
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        int bw = sd(6);
        if (p.y < bw && p.x < bw) return HTTOPLEFT;
        if (p.y < bw && p.x > g.w - bw) return HTTOPRIGHT;
        if (p.y > g.h - bw && p.x < bw) return HTBOTTOMLEFT;
        if (p.y > g.h - bw && p.x > g.w - bw) return HTBOTTOMRIGHT;
        if (p.y < bw) return HTTOP;
        if (p.y > g.h - bw) return HTBOTTOM;
        if (p.x < bw) return HTLEFT;
        if (p.x > g.w - bw) return HTRIGHT;
        if (p.y < sd(52) && p.x < g.r_btn_min.left) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = sd(960);
        mm->ptMinTrackSize.y = sd(640);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        eng_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- 入口 */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    /* 高 DPI：Per-Monitor V2（Win10 1703+；失败则退回系统 DPI 感知） */
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *fn_ctx)(void*);
        fn_ctx f = (fn_ctx)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (f) f((void*)-4); else SetProcessDPIAware();
    }

    gp_init();
    apply_theme(1);      /* 必须走统一入口：同时置 g.dark，否则命令行阶段会被覆盖成亮色 */

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"WomicLinkWnd";
    wc.hIcon = LoadIconW(hInst, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);

    /* DPI 必须在算尺寸之前就位：否则 sd() 返回 0，窗口退化成 1x1 */
    {
        HDC screen = GetDC(NULL);
        g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
        if (!g.dpi) g.dpi = 96;
    }
    g.w = sd(1080); g.h = sd(700);
    RECT wr = { 0, 0, g.w, g.h };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, L"WomicLinkWnd", L"WO Mic Link",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g.hwnd = hwnd;

    /* Win11 圆角 + 暗色边框（失败无所谓，Win10 会忽略） */
    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            typedef HRESULT (WINAPI *fn_attr)(HWND, DWORD, LPCVOID, DWORD);
            fn_attr set = (fn_attr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (set) {
                int pref = 2;                       /* DWMWCP_ROUND */
                set(hwnd, 33, &pref, sizeof(pref));
                BOOL dark = TRUE;
                set(hwnd, 20, &dark, sizeof(dark));
            }
        }
    }

    /* 命令行：womic.exe [host] [port] [--auto]
     * 便于脚本化与 CI；不带参数则用界面默认值。 */
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int auto_connect = 0;
        if (argv) {
            int pos = 0;
            for (int i = 1; i < argc; i++) {
                if (!wcscmp(argv[i], L"--auto")) { auto_connect = 1; continue; }
                if (!wcscmp(argv[i], L"--light")) { g.dark = 0; continue; }
                if (pos == 0) { SetWindowTextW(g.edit_host, argv[i]); pos = 1; }
                else if (pos == 1) { SetWindowTextW(g.edit_port, argv[i]); pos = 2; }
            }
            LocalFree(argv);
        }
        apply_theme(g.dark);
        if (auto_connect) PostMessageW(hwnd, WM_APP + 2, 0, 0);
    }

    layout(); place_edits();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
