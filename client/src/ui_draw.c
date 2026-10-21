/* ui_draw.c — 绘制原语与控件 */
#include "ui_draw.h"
#include "opusdec.h"
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------- GDI+ 动态绑定
 * 不用 gdiplus.h（含 C++ 类，C 编不过），只手写需要的 flat API 并动态加载。
 * 好处：不增加导入表、不引入链接期依赖，gdiplus.dll 每台 Windows 都有。
 */
typedef struct { UINT32 ver; void *cb; BOOL no_thread; BOOL no_codecs; } GpStartupInput;
typedef int   GpStatus;
typedef void *GpGraphics, *GpPath, *GpBrush, *GpPen;
typedef float REAL;

static struct {
    HMODULE mod;
    GpStatus (WINAPI *startup)(ULONG_PTR *, const GpStartupInput *, void **);
    GpStatus (WINAPI *shutdown)(ULONG_PTR);
    GpStatus (WINAPI *from_hdc)(HDC, GpGraphics *);
    GpStatus (WINAPI *del_graphics)(GpGraphics);
    GpStatus (WINAPI *set_smooth)(GpGraphics, int);
    GpStatus (WINAPI *create_path)(int, GpPath *);
    GpStatus (WINAPI *add_arc)(GpPath, REAL, REAL, REAL, REAL, REAL, REAL);
    GpStatus (WINAPI *close_fig)(GpPath);
    GpStatus (WINAPI *solid_fill)(UINT32, GpBrush *);
    GpStatus (WINAPI *fill_path)(GpGraphics, GpBrush, GpPath);
    GpStatus (WINAPI *draw_path)(GpGraphics, GpPen, GpPath);
    GpStatus (WINAPI *create_pen)(UINT32, REAL, int, GpPen *);
    GpStatus (WINAPI *del_brush)(GpBrush);
    GpStatus (WINAPI *del_pen)(GpPen);
    GpStatus (WINAPI *del_path)(GpPath);
    ULONG_PTR token;
    int ready;
} gp;

#define GPA(field, sym, type) gp.field = (type)(void *)GetProcAddress(gp.mod, "Gdip" sym)

void ui_gdiplus_init(void)
{
    gp.mod = LoadLibraryW(L"gdiplus.dll");
    if (!gp.mod) return;
    GPA(from_hdc,     "CreateFromHDC",    GpStatus (WINAPI *)(HDC, GpGraphics *));
    GPA(del_graphics, "DeleteGraphics",   GpStatus (WINAPI *)(GpGraphics));
    GPA(set_smooth,   "SetSmoothingMode", GpStatus (WINAPI *)(GpGraphics, int));
    GPA(create_path,  "CreatePath",       GpStatus (WINAPI *)(int, GpPath *));
    GPA(add_arc,      "AddPathArc",       GpStatus (WINAPI *)(GpPath, REAL, REAL, REAL, REAL, REAL, REAL));
    GPA(close_fig,    "ClosePathFigure",  GpStatus (WINAPI *)(GpPath));
    GPA(solid_fill,   "CreateSolidFill",  GpStatus (WINAPI *)(UINT32, GpBrush *));
    GPA(fill_path,    "FillPath",         GpStatus (WINAPI *)(GpGraphics, GpBrush, GpPath));
    GPA(draw_path,    "DrawPath",         GpStatus (WINAPI *)(GpGraphics, GpPen, GpPath));
    GPA(create_pen,   "CreatePen1",       GpStatus (WINAPI *)(UINT32, REAL, int, GpPen *));
    GPA(del_brush,    "DeleteBrush",      GpStatus (WINAPI *)(GpBrush));
    GPA(del_pen,      "DeletePen",        GpStatus (WINAPI *)(GpPen));
    GPA(del_path,     "DeletePath",       GpStatus (WINAPI *)(GpPath));
    gp.startup  = (GpStatus (WINAPI *)(ULONG_PTR *, const GpStartupInput *, void **))
                  (void *)GetProcAddress(gp.mod, "GdiplusStartup");
    gp.shutdown = (GpStatus (WINAPI *)(ULONG_PTR))
                  (void *)GetProcAddress(gp.mod, "GdiplusShutdown");
    if (!gp.from_hdc || !gp.create_path || !gp.fill_path || !gp.startup) { gp.mod = NULL; return; }
    {
        GpStartupInput in = { 1, NULL, FALSE, FALSE };
        if (gp.startup(&gp.token, &in, NULL) != 0) { gp.mod = NULL; return; }
    }
    gp.ready = 1;
}

int ui_gdiplus_ready(void) { return gp.ready; }

/* ---------------------------------------------------------------- DPI / 字体 */
int sd(int v) { return MulDiv(v, (int)g_ui.dpi, 96); }

typedef struct { int size, weight, mono, dpi; HFONT f; } FontEnt;
static FontEnt g_fonts[24];
static int     g_nfont;

HFONT ui_font(int size_pt, int weight, int mono)
{
    LOGFONTW lf;
    int i, slot;
    for (i = 0; i < g_nfont; i++)
        if (g_fonts[i].size == size_pt && g_fonts[i].weight == weight &&
            g_fonts[i].mono == mono && g_fonts[i].dpi == (int)g_ui.dpi)
            return g_fonts[i].f;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(size_pt, (int)g_ui.dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, mono ? L"Consolas" : L"Segoe UI");
    slot = g_nfont < 24 ? g_nfont++ : 0;
    if (g_fonts[slot].f) DeleteObject(g_fonts[slot].f);
    g_fonts[slot].size = size_pt;
    g_fonts[slot].weight = weight;
    g_fonts[slot].mono = mono;
    g_fonts[slot].dpi = (int)g_ui.dpi;
    g_fonts[slot].f = CreateFontIndirectW(&lf);
    return g_fonts[slot].f;
}

void ui_fonts_reset(void)
{
    int i;
    for (i = 0; i < g_nfont; i++) if (g_fonts[i].f) DeleteObject(g_fonts[i].f);
    memset(g_fonts, 0, sizeof(g_fonts));
    g_nfont = 0;
}

/* ---------------------------------------------------------------- 原语 */
static UINT32 argb(COLORREF c, int a)
{
    return ((UINT32)a << 24) | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

static void path_round(GpPath p, RECT r, int rad)
{
    REAL x = (REAL)r.left, y = (REAL)r.top;
    REAL w = (REAL)(r.right - r.left) - 1, h = (REAL)(r.bottom - r.top) - 1;
    REAL d = (REAL)(rad * 2);
    gp.add_arc(p, x, y, d, d, 180, 90);
    gp.add_arc(p, x + w - d, y, d, d, 270, 90);
    gp.add_arc(p, x + w - d, y + h - d, d, d, 0, 90);
    gp.add_arc(p, x, y + h - d, d, d, 90, 90);
    gp.close_fig(p);
}

void ui_fill_round(HDC dc, RECT r, int rad, COLORREF c, int alpha)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (!gp.ready) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return;
    }
    {
        GpGraphics gfx = NULL; GpPath p = NULL; GpBrush b = NULL;
        gp.from_hdc(dc, &gfx);
        if (gfx) {
            gp.set_smooth(gfx, 4);
            gp.create_path(0, &p);
            if (p) {
                if (rad <= 0) gp.add_arc(p, (REAL)r.left, (REAL)r.top, 0.1f, 0.1f, 0, 0);
                else path_round(p, r, rad);
                gp.close_fig(p);
                gp.solid_fill(argb(c, alpha), &b);
                if (b) { gp.fill_path(gfx, b, p); gp.del_brush(b); }
                gp.del_path(p);
            }
            gp.del_graphics(gfx);
        }
    }
}

void ui_stroke_round(HDC dc, RECT r, int rad, COLORREF c, int alpha, int width)
{
    if (!gp.ready || r.right <= r.left || r.bottom <= r.top) return;
    {
        GpGraphics gfx = NULL; GpPath p = NULL; GpPen pen = NULL;
        gp.from_hdc(dc, &gfx);
        if (!gfx) return;
        gp.set_smooth(gfx, 4);
        gp.create_path(0, &p);
        if (!p) { gp.del_graphics(gfx); return; }
        path_round(p, r, rad);
        gp.create_pen(argb(c, alpha), (REAL)width, 0, &pen);
        if (pen) { gp.draw_path(gfx, pen, p); gp.del_pen(pen); }
        gp.del_path(p);
        gp.del_graphics(gfx);
    }
}

void ui_bg_gradient(HDC dc, RECT r)
{
    int n = r.bottom - r.top, i;
    for (i = 0; i < n; i++) {
        float t = (float)i / (float)(n ? n : 1);
        int R = (int)(GetRValue(g_th.bg) * (1 - t) + GetRValue(g_th.bg2) * t);
        int G = (int)(GetGValue(g_th.bg) * (1 - t) + GetGValue(g_th.bg2) * t);
        int B = (int)(GetBValue(g_th.bg) * (1 - t) + GetBValue(g_th.bg2) * t);
        RECT line = { r.left, r.top + i, r.right, r.top + i + 1 };
        HBRUSH b = CreateSolidBrush(RGB(R, G, B));
        FillRect(dc, &line, b);
        DeleteObject(b);
    }
}

void ui_text(HDC dc, RECT r, const wchar_t *s, HFONT f, COLORREF c, UINT fmt)
{
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

void ui_text_u8(HDC dc, RECT r, const char *s, HFONT f, COLORREF c, UINT fmt)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    w[511] = 0;
    ui_text(dc, r, w, f, c, fmt);
}

/* ---------------------------------------------------------------- 图标 */
static void arc_deg(HDC dc, int cx, int cy, int rad, double a0, double a1)
{
    double r0 = a0 * 3.14159265 / 180.0, r1 = a1 * 3.14159265 / 180.0;
    int x0 = (int)(cx + rad * cos(r0)), y0 = (int)(cy - rad * sin(r0));
    int x1 = (int)(cx + rad * cos(r1)), y1 = (int)(cy - rad * sin(r1));
    Arc(dc, cx - rad, cy - rad, cx + rad, cy + rad, x0, y0, x1, y1);
}

void ui_icon(HDC dc, RECT r, int kind, COLORREF c, int w)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int s = (r.right - r.left) < (r.bottom - r.top) ? (r.right - r.left) : (r.bottom - r.top);
    int h = s / 2;
    HPEN pen = CreatePen(PS_SOLID, w < 1 ? 1 : w, c);
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
        int i;
        Ellipse(dc, cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2);
        for (i = 0; i < 8; i++) {
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
    case IC_CHEV:   /* 折叠指示：向右的尖括号，展开时由调用方旋转成向下（用两条线画） */
        MoveToEx(dc, cx - h / 3, cy - h / 2, NULL);
        LineTo(dc, cx + h / 3, cy);
        LineTo(dc, cx - h / 3, cy + h / 2);
        break;
    case IC_DOC:
        Rectangle(dc, cx - h / 2, cy - h, cx + h / 2, cy + h);
        MoveToEx(dc, cx - h / 4, cy - h / 2, NULL); LineTo(dc, cx + h / 4, cy - h / 2);
        MoveToEx(dc, cx - h / 4, cy, NULL);          LineTo(dc, cx + h / 4, cy);
        MoveToEx(dc, cx - h / 4, cy + h / 2, NULL);  LineTo(dc, cx + h / 4, cy + h / 2);
        break;
    case IC_SHIELD:
        MoveToEx(dc, cx, cy - h, NULL);
        LineTo(dc, cx + h / 2, cy - h / 2);
        LineTo(dc, cx + h / 2, cy + h / 4);
        LineTo(dc, cx, cy + h);
        LineTo(dc, cx - h / 2, cy + h / 4);
        LineTo(dc, cx - h / 2, cy - h / 2);
        LineTo(dc, cx, cy - h);
        break;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* ---------------------------------------------------------------- 控件 */
void ui_card(HDC dc, RECT r, const wchar_t *title, const wchar_t *hint)
{
    ui_fill_round(dc, r, sd(12), g_th.card, 255);
    ui_stroke_round(dc, r, sd(12), g_th.stroke, g_th.dark ? 150 : 200, 1);
    if (title) {
        RECT tr = { r.left + sd(16), r.top + sd(11), r.right - sd(16), r.top + sd(31) };
        ui_text(dc, tr, title, ui_font(9, FW_SEMIBOLD, 0), g_th.text_dim,
                DT_LEFT | DT_SINGLELINE);
    }
    if (hint) {
        RECT tr = { r.left + sd(16), r.top + sd(11), r.right - sd(16), r.top + sd(31) };
        ui_text(dc, tr, hint, ui_font(8, FW_NORMAL, 0), g_th.text_mute,
                DT_RIGHT | DT_SINGLELINE);
    }
}

void ui_button(HDC dc, RECT r, const wchar_t *label, int id, int danger, int enabled)
{
    int hov = (g_ui.hover == id) && enabled;
    int down = (g_ui.pressed == id) && enabled;
    COLORREF base = danger ? g_th.err : g_th.accent;
    COLORREF fg;

    if (!enabled) {
        ui_fill_round(dc, r, sd(10), g_th.card_hi, 255);
        ui_stroke_round(dc, r, sd(10), g_th.stroke, 200, 1);
        ui_text(dc, r, label, ui_font(10, FW_SEMIBOLD, 0), g_th.text_mute,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (down) base = RGB(GetRValue(base) * 78 / 100, GetGValue(base) * 78 / 100, GetBValue(base) * 78 / 100);
    else if (hov) base = RGB(min(255, GetRValue(base) + 18), min(255, GetGValue(base) + 18), min(255, GetBValue(base) + 18));

    ui_fill_round(dc, r, sd(10), base, 255);
    fg = g_th.dark ? RGB(10, 18, 24) : RGB(255, 255, 255);
    if (danger) fg = RGB(255, 255, 255);
    ui_text(dc, r, label, ui_font(10, FW_SEMIBOLD, 0), fg,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ui_seg(HDC dc, RECT r, const wchar_t *label, int active, int id, int enabled)
{
    int hov = (g_ui.hover == id) && enabled;
    COLORREF fg;
    if (active) {
        ui_fill_round(dc, r, sd(8), g_th.accent, 255);
    } else if (hov && enabled) {
        ui_fill_round(dc, r, sd(8), g_th.card_hi, 255);
    }
    fg = active ? (g_th.dark ? RGB(12, 20, 26) : RGB(255, 255, 255))
                : (enabled ? g_th.text_dim : g_th.text_mute);
    ui_text(dc, r, label, ui_font(9, active ? FW_SEMIBOLD : FW_NORMAL, 0), fg,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ui_slider(HDC dc, RECT r, int val, int maxv, int id)
{
    RECT tr = r, fillr;
    int knob_x, kr = sd(7);
    (void)id;
    tr.bottom = tr.top + sd(6);
    ui_fill_round(dc, tr, sd(3), g_th.card_hi, 255);
    fillr = tr;
    fillr.right = tr.left + (tr.right - tr.left) * (maxv ? val : 0) / (maxv ? maxv : 1);
    ui_fill_round(dc, fillr, sd(3), g_th.accent, 255);
    knob_x = fillr.right;
    ui_fill_round(dc, (RECT){ knob_x - kr, tr.top - sd(4), knob_x + kr, tr.bottom + sd(4) },
                  kr, g_th.card, 255);
    ui_stroke_round(dc, (RECT){ knob_x - kr, tr.top - sd(4), knob_x + kr, tr.bottom + sd(4) },
                    kr, g_th.accent, 255, 2);
}

void ui_collapse_hdr(HDC dc, RECT r, const wchar_t *title, const wchar_t *hint,
                     int expanded, int id)
{
    RECT ir = { r.left, r.top, r.left + sd(20), r.bottom };
    RECT tr = { r.left + sd(24), r.top, r.right - sd(284), r.bottom };
    RECT hr = { r.right - sd(280), r.top, r.right - sd(6), r.bottom };
    int hov = (g_ui.hover == id);

    if (hov) ui_fill_round(dc, r, sd(8), g_th.card_hi, 255);
    {
        /* 折叠指示：收起 = 向右的尖括号；展开 = 向下的尖括号（同一支笔画两种方向） */
        int cx = (ir.left + ir.right) / 2, cy = (ir.top + ir.bottom) / 2;
        int u = sd(4);
        HPEN pen = CreatePen(PS_SOLID, sd(2) < 1 ? 1 : sd(2), g_th.text_dim);
        HPEN old = (HPEN)SelectObject(dc, pen);
        SetBkMode(dc, TRANSPARENT);
        if (expanded) {
            MoveToEx(dc, cx - u, cy - u / 2, NULL);
            LineTo(dc, cx, cy + u / 2);
            LineTo(dc, cx + u, cy - u / 2);
        } else {
            MoveToEx(dc, cx - u / 2, cy - u, NULL);
            LineTo(dc, cx + u / 2, cy);
            LineTo(dc, cx - u / 2, cy + u);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    ui_text(dc, tr, title, ui_font(10, FW_SEMIBOLD, 0), g_th.text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hint)
        ui_text(dc, hr, hint, ui_font(8, FW_NORMAL, 0), g_th.text_mute,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void ui_badge(HDC dc, RECT r, const wchar_t *text, COLORREF fg, COLORREF bg)
{
    int rad = (r.bottom - r.top) / 2;
    ui_fill_round(dc, r, rad, bg, 255);
    ui_text(dc, r, text, ui_font(8, FW_SEMIBOLD, 0), fg,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* ------------------------------------------------------------ 业务卡片 */
#include "ui_layout.h"
#include "ui_voicemeter.h"
#include "drvinst.h"

static const wchar_t *drv_state_text(DrvState s)
{
    switch (s) {
    case DRV_READY: return L"驱动已就绪";
    case DRV_NEED_REBOOT: return L"驱动需重启";
    case DRV_NOT_INSTALLED: return L"驱动未安装";
    default: return L"状态未知";
    }
}

static COLORREF drv_state_color(DrvState s)
{
    switch (s) {
    case DRV_READY: return g_th.ok;
    case DRV_NEED_REBOOT: return g_th.warn;
    case DRV_NOT_INSTALLED: return g_th.warn;
    default: return g_th.text_mute;
    }
}

void ui_draw_titlebar(HDC dc)
{
    int pad = sd(20);
    RECT ir = { pad, sd(12), pad + sd(28), sd(40) };
    ui_fill_round(dc, ir, sd(8), g_th.accent, 255);
    {
        RECT iw = { ir.left + sd(7), ir.top + sd(7), ir.right - sd(7), ir.bottom - sd(7) };
        ui_icon(dc, iw, IC_MIC, g_th.dark ? RGB(10, 18, 24) : RGB(255, 255, 255), sd(2));
    }
    {
        RECT tr = { pad + sd(38), sd(12), pad + sd(200), sd(42) };
        ui_text(dc, tr, L"WO Mic Link", ui_font(11, FW_SEMIBOLD, 0), g_th.text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    {
        RECT vr = { pad + sd(133), sd(13), pad + sd(230), sd(41) };
        ui_text(dc, vr, L"1.1", ui_font(8, FW_NORMAL, 0), g_th.text_mute,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (g_ui.hover == ID_MIN) ui_fill_round(dc, g_lo.btn_min, sd(6), g_th.card_hi, 200);
    if (g_ui.hover == ID_CLOSE) ui_fill_round(dc, g_lo.btn_close, sd(6), g_th.err, 230);
    {
        RECT a = { g_lo.btn_min.left + sd(14), g_lo.btn_min.top + sd(14),
                   g_lo.btn_min.right - sd(14), g_lo.btn_min.bottom - sd(14) };
        RECT b = { g_lo.btn_close.left + sd(14), g_lo.btn_close.top + sd(14),
                   g_lo.btn_close.right - sd(14), g_lo.btn_close.bottom - sd(14) };
        ui_icon(dc, a, IC_MIN, g_ui.hover == ID_MIN ? g_th.text : g_th.text_dim, sd(2));
        ui_icon(dc, b, IC_CLOSE, g_ui.hover == ID_CLOSE ? RGB(255, 255, 255) : g_th.text_dim, sd(2));
    }
    if (g_ui.hover == ID_THEME) ui_fill_round(dc, g_lo.btn_theme, sd(6), g_th.card_hi, 200);
    {
        RECT t = { g_lo.btn_theme.left + sd(9), g_lo.btn_theme.top + sd(9),
                   g_lo.btn_theme.right - sd(9), g_lo.btn_theme.bottom - sd(9) };
        ui_icon(dc, t, g_ui.dark ? IC_SUN : IC_MOON, g_th.text_dim, sd(2));
    }
}

void ui_draw_conn_card(HDC dc)
{
    RECT r = g_lo.card_conn;
    ui_card(dc, r, L"连接", L"控制端口默认 8125");
    {
        RECT ir = { r.left + sd(18), r.top + sd(40), r.left + sd(36), r.top + sd(58) };
        ui_icon(dc, ir, IC_SEARCH, g_th.text_mute, sd(2));
    }
    {
        RECT box = { r.left + sd(14), r.top + sd(32), r.right - sd(14), r.top + sd(66) };
        ui_stroke_round(dc, box, sd(8), g_th.stroke, 255, 1);
    }
    {
        RECT hint = { r.left + sd(16), r.top + sd(70), r.right - sd(16), r.top + sd(88) };
        ui_text(dc, hint, L"手机与电脑需在同一局域网；手机端打开 WO Mic 选 Wi-Fi",
                ui_font(8, FW_NORMAL, 0), g_th.text_mute, DT_LEFT | DT_SINGLELINE);
    }
    ui_button(dc, g_lo.btn_conn, g_ui.running ? L"断开连接" : L"连接", ID_CONN,
              g_ui.running, 1);
}

void ui_draw_drv_card(HDC dc)
{
    RECT r = g_lo.card_drv;
    DrvStatus *d = &g_ui.drv;
    int cx = r.left + sd(26), cy = r.top + sd(40);
    COLORREF dot = drv_state_color(d->state);

    ui_card(dc, r, L"虚拟麦克风驱动", drvpkg_arch_label(d->arch));
    if (d->state == DRV_READY && g_ui.running) {
        int rp = sd(13) + (int)(sin(g_ui.anim_phase) * sd(3));
        RECT gr = { cx - rp, cy - rp, cx + rp, cy + rp };
        ui_fill_round(dc, gr, rp, dot, 40);
    }
    {
        int dd = sd(6);
        RECT dr = { cx - dd, cy - dd, cx + dd, cy + dd };
        ui_fill_round(dc, dr, dd, dot, 255);
    }
    {
        RECT tr = { cx + sd(16), r.top + sd(28), r.right - sd(16), r.top + sd(50) };
        ui_text(dc, tr, drv_state_text(d->state), ui_font(12, FW_SEMIBOLD, 0),
                g_th.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    {
        wchar_t sub[220];
        if (d->state == DRV_NOT_INSTALLED)
            wcscpy(sub, L"未检测到 \\\\.\\WOMic：点击下方“安装驱动”");
        else if (d->inf_path[0])
            _snwprintf(sub, 220, L"%ls  ·  %ls", d->inf_path,
                       d->version[0] ? d->version : L"版本未知");
        else
            _snwprintf(sub, 220, L"%ls", drv_last_error_w());
        RECT sr = { cx + sd(16), r.top + sd(52), r.right - sd(16), r.top + sd(70) };
        ui_text(dc, sr, sub, ui_font(8, FW_NORMAL, 0), g_th.text_dim,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ui_button(dc, g_lo.btn_install, L"安装驱动", ID_INSTALL, 0,
              d->state != DRV_READY);
    ui_button(dc, g_lo.btn_uninstall, L"卸载驱动", ID_UNINSTALL, 1,
              d->state != DRV_NOT_INSTALLED);
    ui_button(dc, g_lo.btn_help, L"命令行说明书", ID_HELP, 0, 1);
}

void ui_draw_audio_card(HDC dc)
{
    wchar_t hint[48];
    static const wchar_t *rate_lbl[3] = { L"48 kHz", L"16 kHz", L"8 kHz" };
    static const wchar_t *buf_lbl[3] = { L"26 ms", L"60 ms", L"120 ms" };
    int i;

    _snwprintf(hint, 48, L"%ls · 缓冲 %ls · 增益 %d%%",
               rate_lbl[g_ui.seg_rate], buf_lbl[g_ui.buf_idx], g_ui.gain);
    ui_card(dc, g_lo.card_audio, NULL, NULL);
    ui_collapse_hdr(dc, g_lo.hdr_audio, L"音频参数", hint, g_ui.exp_audio, ID_EXP_AUDIO);
    if (!g_ui.exp_audio) return;

    {
        RECT lr = { g_lo.card_audio.left + sd(16), g_lo.hdr_audio.bottom,
                    g_lo.card_audio.right - sd(16), g_lo.hdr_audio.bottom + sd(18) };
        ui_text(dc, lr, L"采样率（灌给驱动的一律是 48 kHz）", ui_font(8, FW_NORMAL, 0),
                g_th.text_mute, DT_LEFT | DT_SINGLELINE);
    }
    for (i = 0; i < 3; i++)
        ui_seg(dc, g_lo.seg_rate[i], rate_lbl[i], g_ui.seg_rate == i, ID_RATE0 + i, 1);

    {
        RECT lr = { g_lo.card_audio.left + sd(16), g_lo.seg_rate[0].bottom + sd(6),
                    g_lo.card_audio.right - sd(16), g_lo.seg_rate[0].bottom + sd(22) };
        ui_text(dc, lr, L"环形缓冲（26 ms 低延迟 / 120 ms 更稳）", ui_font(8, FW_NORMAL, 0),
                g_th.text_mute, DT_LEFT | DT_SINGLELINE);
    }
    for (i = 0; i < 3; i++)
        ui_seg(dc, g_lo.seg_buf[i], buf_lbl[i], g_ui.buf_idx == i, ID_BUF0 + i, 1);

    {
        wchar_t v[48];
        RECT lr = { g_lo.card_audio.left + sd(16), g_lo.seg_buf[0].bottom + sd(6),
                    g_lo.card_audio.right - sd(16), g_lo.seg_buf[0].bottom + sd(22) };
        swprintf(v, 48, L"本地增益 %d%%", g_ui.gain);
        ui_text(dc, lr, v, ui_font(8, FW_NORMAL, 0), g_th.text_dim, DT_LEFT | DT_SINGLELINE);
    }
    ui_slider(dc, g_lo.slider, g_ui.gain, 200, ID_SLIDER);
}

static void stat_item(HDC dc, int x, int y, int cw, const wchar_t *v, const wchar_t *k)
{
    RECT vr = { x, y, x + cw, y + sd(22) };
    RECT kr = { x, y + sd(20), x + cw, y + sd(38) };
    ui_text(dc, vr, v, ui_font(11, FW_SEMIBOLD, 0), g_th.text, DT_LEFT | DT_SINGLELINE);
    ui_text(dc, kr, k, ui_font(8, FW_NORMAL, 0), g_th.text_mute, DT_LEFT | DT_SINGLELINE);
}

void ui_draw_stats_card(HDC dc)
{
    EngineStats *s = &g_ui.st;
    wchar_t v[8][32];
    const wchar_t *k[8] = { L"码率", L"帧数", L"丢包", L"延迟",
                            L"注入", L"协商速率", L"缓冲", L"设备" };
    int cw, i, x, y;

    ui_card(dc, g_lo.card_stats, NULL, NULL);
    ui_collapse_hdr(dc, g_lo.hdr_stats, L"会话统计", NULL, g_ui.exp_stats, ID_EXP_STATS);
    if (!g_ui.exp_stats) return;

    swprintf(v[0], 32, L"%d kbps", s->kbps);
    swprintf(v[1], 32, L"%llu", (unsigned long long)s->frames);
    swprintf(v[2], 32, L"%u", s->lost);
    swprintf(v[3], 32, L"%d ms", s->latency_ms);
    swprintf(v[4], 32, L"%llu KB", (unsigned long long)(s->injected / 1024));
    swprintf(v[5], 32, L"%d Hz", s->rate);
    swprintf(v[6], 32, L"%d ms", s->buffer_ms);
    wcscpy(v[7], s->device_ok ? L"已握手" : L"未就绪");

    cw = (g_lo.card_stats.right - g_lo.card_stats.left - sd(32)) / 4;
    y = g_lo.hdr_stats.bottom + sd(8);
    for (i = 0; i < 8; i++) {
        int row = i / 4, col = i % 4;
        x = g_lo.card_stats.left + sd(16) + col * cw;
        stat_item(dc, x, y + row * sd(42), cw, v[i], k[i]);
    }
}

void ui_draw_log_card(HDC dc)
{
    int y, i, line_h = sd(17);
    int bottom;
    HFONT f;

    ui_card(dc, g_lo.card_log, NULL, NULL);
    ui_collapse_hdr(dc, g_lo.hdr_log, L"事件", NULL, g_ui.exp_log, ID_EXP_LOG);
    if (!g_ui.exp_log) return;

    y = g_lo.hdr_log.bottom + sd(6);
    bottom = g_lo.card_log.bottom - sd(8);
    f = ui_font(8, FW_NORMAL, 1);
    for (i = 0; i < ui_log_count(); i++) {
        const LogLine *L = ui_log_get(i);
        RECT tr, mr;
        if (!L || y + line_h > bottom) break;
        SetRect(&tr, g_lo.card_log.left + sd(16), y, g_lo.card_log.left + sd(56), y + line_h);
        SetRect(&mr, g_lo.card_log.left + sd(60), y, g_lo.card_log.right - sd(16), y + line_h);
        ui_text(dc, tr, L->ts, f, g_th.text_mute, DT_LEFT | DT_SINGLELINE);
        ui_text_u8(dc, mr, L->text, f,
                   L->kind == 1 ? g_th.err : L->kind == 2 ? g_th.ok : g_th.text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line_h;
    }
    if (!ui_log_count()) {
        RECT tr = { g_lo.card_log.left + sd(16), g_lo.hdr_log.bottom + sd(10),
                    g_lo.card_log.right - sd(16), g_lo.hdr_log.bottom + sd(30) };
        ui_text(dc, tr, L"暂无事件", ui_font(9, FW_NORMAL, 0), g_th.text_mute,
                DT_LEFT | DT_SINGLELINE);
    }
}

void ui_draw_statusbar(HDC dc)
{
    RECT r = { 0, g_ui.h - ui_status_h(), g_ui.w, g_ui.h };
    RECT ln = { 0, r.top, g_ui.w, r.top + 1 };
    HFONT f = ui_font(8, FW_NORMAL, 0);
    wchar_t left[128], right[200];

    ui_fill_round(dc, r, 0, g_th.card, 255);
    ui_fill_round(dc, ln, 0, g_th.stroke, 120);

    swprintf(left, 128, L"%ls", g_ui.running ? L"引擎运行中" : L"引擎空闲");
    {
        RECT lr = { sd(20), r.top, sd(300), r.bottom };
        ui_text(dc, lr, left, f, g_th.text_mute, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    swprintf(right, 200, L"虚拟麦克风 %ls   ·   解码器 %hs   ·   灌音 %ls",
             g_ui.drv.state == DRV_READY ? L"就绪" : L"未安装",
             g_ui.st.opus_src[0] ? g_ui.st.opus_src : opusdec_source(),
             g_ui.st.buffer_ms ? L"进行中" : L"空闲");
    {
        RECT rr = { g_ui.w - sd(520), r.top, g_ui.w - sd(20), r.bottom };
        ui_text(dc, rr, right, f,
                g_ui.drv.state == DRV_READY ? g_th.text_mute : g_th.warn,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void ui_draw_all(HDC dc)
{
    RECT full = { 0, 0, g_ui.w, g_ui.h };
    int saved;

    ui_bg_gradient(dc, full);
    ui_draw_titlebar(dc);

    /* 卡片流只画在「标题栏与状态栏之间」的可视区里：展开面板后内容超出窗口时，
     * 超出的部分必须被裁掉，而不是画到状态栏下面去 */
    saved = SaveDC(dc);
    IntersectClipRect(dc, 0, ui_title_h(), g_ui.w, g_ui.h - ui_status_h());
    ui_draw_conn_card(dc);
    ui_draw_drv_card(dc);
    ui_meter_draw(dc, g_lo.card_meter);
    ui_draw_audio_card(dc);
    ui_draw_stats_card(dc);
    ui_draw_log_card(dc);
    RestoreDC(dc, saved);

    ui_draw_statusbar(dc);

    /* 有滚动空间时画一条细指示，避免用户以为内容就这么多 */
    if (g_lo.content_h > ui_title_h() + g_lo.view_h) {
        int track_y = ui_title_h() + sd(4);
        int track_h = g_lo.view_h - sd(8);
        int total = g_lo.content_h;
        int th = track_h * g_lo.view_h / (total ? total : 1);
        int ty = track_y + (track_h - th) * g_ui.scroll /
                 ((total - ui_title_h() - g_lo.view_h) > 0 ? (total - ui_title_h() - g_lo.view_h) : 1);
        RECT sb = { g_ui.w - sd(6), track_y, g_ui.w - sd(2), track_y + track_h };
        RECT kn = { g_ui.w - sd(6), ty, g_ui.w - sd(2), ty + (th < sd(16) ? sd(16) : th) };
        ui_fill_round(dc, sb, sd(2), g_th.stroke, 120);
        ui_fill_round(dc, kn, sd(2), g_th.accent, 200);
    }
}

/* ---------------------------------------------------------------- 日志 */
static LogLine g_logs[LOG_LINES];
static int g_head, g_count;

void ui_log_add(const char *s, int kind)
{
    LogLine *L = &g_logs[g_head];
    strncpy(L->text, s, LOG_LEN - 1);
    L->text[LOG_LEN - 1] = 0;
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf(L->ts, 12, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    }
    L->kind = kind;
    g_head = (g_head + 1) % LOG_LINES;
    if (g_count < LOG_LINES) g_count++;
}

int ui_log_count(void) { return g_count; }

const LogLine *ui_log_get(int i)
{
    if (i < 0 || i >= g_count) return NULL;
    return &g_logs[(g_head - 1 - i + LOG_LINES * 2) % LOG_LINES];
}
