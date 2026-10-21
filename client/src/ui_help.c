/* ui_help.c — 说明书窗口（内容直接来自 cli_help_text()，与 --help 永远一致） */
#include "ui_help.h"
#include "ui_draw.h"
#include "cli.h"
#include <windowsx.h>
#include <stdlib.h>
#include <string.h>

#define HELP_CLASS L"MicLinkHelpClass"
#define MAX_LINES  600

static HWND   g_hwnd = NULL;
static HBRUSH g_br = NULL;
static wchar_t *g_lines[MAX_LINES];
static int    g_nlines;
static int    g_scroll;      /* 行偏移 */
static int    g_line_h = 18;
static int    g_page;

static void free_lines(void)
{
    int i;
    for (i = 0; i < g_nlines; i++) { free(g_lines[i]); g_lines[i] = NULL; }
    g_nlines = 0;
}

/* 按像素宽度折行；中文按双宽估算（用 GetTextExtentPoint32 逐段累加，够准） */
static void wrap_line(HDC dc, const wchar_t *src, int max_px)
{
    int len = (int)wcslen(src);
    int start = 0;
    if (len == 0) {
        if (g_nlines < MAX_LINES) {
            g_lines[g_nlines] = (wchar_t *)malloc(sizeof(wchar_t));
            g_lines[g_nlines][0] = 0;
            g_nlines++;
        }
        return;
    }
    while (start < len && g_nlines < MAX_LINES) {
        int take = len - start;
        SIZE sz;
        /* 二分找出能放下的最长前缀 */
        int lo = 1, hi = take, best = 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            GetTextExtentPoint32W(dc, src + start, mid, &sz);
            if (sz.cx <= max_px) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        /* 尽量不在 CJK 中间断行：若下一个字符是 ASCII 且当前以 ASCII 结尾，
         * 回退到最近的空格（简单处理，够用） */
        {
            int k = best;
            while (k > 1 && start + k < len &&
                   src[start + k - 1] > 0x2000 && src[start + k] > 0x2000 &&
                   src[start + k - 1] != L' ' && src[start + k] != L' ') k--;
            if (k > 1) best = k;
        }
        g_lines[g_nlines] = (wchar_t *)malloc(sizeof(wchar_t) * (best + 1));
        memcpy(g_lines[g_nlines], src + start, sizeof(wchar_t) * best);
        g_lines[g_nlines][best] = 0;
        g_nlines++;
        start += best;
    }
}

static void relayout(HDC dc)
{
    const char *txt = cli_help_text();
    wchar_t w[8192];
    int text_w;
    RECT cr;
    int i, start;

    free_lines();
    GetClientRect(g_hwnd, &cr);
    text_w = (cr.right - cr.left) - sd(40);
    if (text_w < 40) text_w = 40;

    MultiByteToWideChar(CP_UTF8, 0, txt, -1, w, 8192);
    w[8191] = 0;
    start = 0;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n') {
            wchar_t save = w[i];
            w[i] = 0;
            wrap_line(dc, w + start, text_w);
            w[i] = save;
            start = i + 1;
        }
    }
    if (w[start]) wrap_line(dc, w + start, text_w);

    g_line_h = sd(17);
    g_page = (cr.bottom - cr.top - sd(56)) / (g_line_h ? g_line_h : 1);
    if (g_page < 1) g_page = 1;
    if (g_scroll > g_nlines - g_page) g_scroll = g_nlines - g_page;
    if (g_scroll < 0) g_scroll = 0;
}

static void draw(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem;
    HBITMAP bmp, old;
    RECT cr;

    GetClientRect(hwnd, &cr);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    old = (HBITMAP)SelectObject(mem, bmp);

    ui_fill_round(mem, cr, 0, g_th.bg, 255);
    {
        RECT hr = { 0, 0, cr.right, sd(44) };
        ui_fill_round(mem, hr, 0, g_th.card, 255);
    }
    {
        RECT tr = { sd(20), 0, cr.right - sd(80), sd(44) };
        ui_text(mem, tr, L"命令行使用说明书", ui_font(12, FW_SEMIBOLD, 0), g_th.text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    {
        RECT cb = { cr.right - sd(52), sd(8), cr.right - sd(16), sd(36) };
        ui_fill_round(mem, cb, sd(6), g_th.err, 230);
        {
            RECT ir = { cb.left + sd(8), cb.top + sd(6), cb.right - sd(8), cb.bottom - sd(6) };
            ui_icon(mem, ir, IC_CLOSE, RGB(255, 255, 255), sd(2));
        }
    }
    {
        HFONT f = ui_font(9, FW_NORMAL, 1);
        int y = sd(52);
        int i;
        for (i = g_scroll; i < g_nlines && y < cr.bottom - sd(8); i++, y += g_line_h) {
            RECT lr = { sd(20), y, cr.right - sd(20), y + g_line_h };
            const wchar_t *s = g_lines[i];
            COLORREF c = g_th.text_dim;
            if (s[0] == L' ' && s[1] == L' ' && s[2] != L' ') c = g_th.text;   /* 小标题 */
            ui_text(mem, lr, s, f, c, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    /* 滚动条 */
    if (g_nlines > g_page) {
        int track = cr.bottom - sd(60);
        int th = track * g_page / g_nlines;
        int ty = sd(52) + track * g_scroll / g_nlines;
        RECT sb = { cr.right - sd(8), sd(52), cr.right - sd(4), sd(52) + track };
        RECT kn = { cr.right - sd(8), ty, cr.right - sd(4), ty + (th < 12 ? 12 : th) };
        ui_fill_round(mem, sb, sd(2), g_th.stroke, 160);
        ui_fill_round(mem, kn, sd(2), g_th.accent, 220);
    }

    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK help_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_br = CreateSolidBrush(g_th.card);
        {
            HDC dc = GetDC(hwnd);
            relayout(dc);
            ReleaseDC(hwnd, dc);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        draw(hwnd);
        return 0;
    case WM_SIZE: {
        HDC dc = GetDC(hwnd);
        relayout(dc);
        ReleaseDC(hwnd, dc);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        g_scroll += (d < 0 ? 3 : -3);
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > g_nlines - g_page) g_scroll = g_nlines - g_page;
        if (g_scroll < 0) g_scroll = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT cr, cb;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        GetClientRect(hwnd, &cr);
        SetRect(&cb, cr.right - sd(52), sd(8), cr.right - sd(16), sd(36));
        if (PtInRect(&cb, p)) { DestroyWindow(hwnd); return 0; }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wp == VK_DOWN)  { g_scroll++; goto clamp; }
        if (wp == VK_UP)    { g_scroll--; goto clamp; }
        if (wp == VK_NEXT)  { g_scroll += g_page; goto clamp; }
        if (wp == VK_PRIOR) { g_scroll -= g_page; goto clamp; }
        return 0;
clamp:
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > g_nlines - g_page) g_scroll = g_nlines - g_page;
        if (g_scroll < 0) g_scroll = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DESTROY:
        free_lines();
        if (g_br) { DeleteObject(g_br); g_br = NULL; }
        g_hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ui_show_help(HINSTANCE hInst, HWND parent)
{
    WNDCLASSEXW wc;
    if (g_hwnd) { SetForegroundWindow(g_hwnd); return; }
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = help_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = HELP_CLASS;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, HELP_CLASS, L"命令行使用说明书",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             CW_USEDEFAULT, CW_USEDEFAULT, sd(760), sd(620),
                             parent, NULL, hInst, NULL);
    if (g_hwnd) ShowWindow(g_hwnd, SW_SHOW);
}

int ui_help_is_open(void) { return g_hwnd != NULL; }
void ui_help_close(void) { if (g_hwnd) DestroyWindow(g_hwnd); }

int ui_help_handle_key(WPARAM vk)
{
    if (!g_hwnd) return 0;
    SendMessageW(g_hwnd, WM_KEYDOWN, vk, 0);
    return 1;
}
