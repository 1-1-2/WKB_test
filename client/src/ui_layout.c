/* ui_layout.c — 卡片流布局与命中测试 */
#include "ui_layout.h"
#include "ui_draw.h"

UiLayout g_lo;

int ui_title_h(void)  { return sd(52); }
int ui_status_h(void) { return sd(28); }

/* 内容坐标 → 客户区坐标（减滚动偏移） */
static RECT cs(int left, int top, int right, int bottom)
{
    RECT r;
    r.left = left; r.right = right;
    r.top = top - g_ui.scroll;
    r.bottom = bottom - g_ui.scroll;
    return r;
}

static void layout_once(void)
{
    int pad = sd(20), gap = sd(12), inner = sd(16);
    int x0 = pad, x1 = g_ui.w - pad;
    int y = ui_title_h() + sd(8);
    int i;

    g_lo.title    = cs(0, 0, g_ui.w, ui_title_h());
    g_lo.btn_close = cs(g_ui.w - sd(8) - sd(46), sd(6), g_ui.w - sd(8), sd(48));
    g_lo.btn_min   = cs(g_ui.w - sd(8) - sd(92) - sd(2), sd(6), g_ui.w - sd(8) - sd(46) - sd(2), sd(48));
    g_lo.btn_theme = cs(g_ui.w - sd(8) - sd(92) - sd(2) - sd(6) - sd(34), sd(12),
                        g_ui.w - sd(8) - sd(92) - sd(2) - sd(6), sd(42));

    /* ---- 连接区（常驻） ---- */
    g_lo.card_conn = cs(x0, y, x1, y + sd(144));
    {
        int ey = y + sd(34);
        int portw = sd(88);
        g_lo.ed_host = cs(x0 + inner, ey, x1 - inner - portw - sd(8), ey + sd(30));
        g_lo.ed_port = cs(x1 - inner - portw, ey, x1 - inner, ey + sd(30));
        g_lo.btn_conn = cs(x0 + inner, y + sd(90), x1 - inner, y + sd(132));
    }
    y += sd(144) + gap;

    /* ---- 驱动状态卡（常驻） ---- */
    g_lo.card_drv = cs(x0, y, x1, y + sd(116));
    {
        int by = y + sd(72);
        g_lo.btn_install   = cs(x0 + inner, by, x0 + inner + sd(120), by + sd(32));
        g_lo.btn_uninstall = cs(x0 + inner + sd(128), by, x0 + inner + sd(212), by + sd(32));
        g_lo.btn_help      = cs(x0 + inner + sd(220), by, x0 + inner + sd(334), by + sd(32));
    }
    y += sd(116) + gap;

    /* ---- VoiceMeter（常驻） ---- */
    g_lo.card_meter = cs(x0, y, x1, y + sd(148));
    y += sd(148) + gap;

    /* ---- 音频参数（默认收起） ---- */
    g_lo.hdr_audio = cs(x0, y, x1, y + sd(38));
    g_lo.card_audio = cs(x0, y, x1, y + sd(38));
    y += sd(38);
    if (g_ui.exp_audio) {
        int sw = (x1 - x0 - inner * 2) / 3;
        int ry = y + sd(24);
        for (i = 0; i < 3; i++)
            g_lo.seg_rate[i] = cs(x0 + inner + i * sw, ry, x0 + inner + i * sw + sw - sd(6), ry + sd(30));
        ry = y + sd(76);
        for (i = 0; i < 3; i++)
            g_lo.seg_buf[i] = cs(x0 + inner + i * sw, ry, x0 + inner + i * sw + sw - sd(6), ry + sd(28));
        g_lo.slider = cs(x0 + inner, y + sd(128), x1 - inner, y + sd(134));
        g_lo.card_audio = cs(x0, g_lo.hdr_audio.top + g_ui.scroll, x1, y + sd(148));
        y += sd(148);
    }
    y += gap;

    /* ---- 统计（默认收起） ---- */
    g_lo.hdr_stats = cs(x0, y, x1, y + sd(38));
    g_lo.card_stats = cs(x0, y, x1, y + sd(38));
    y += sd(38);
    if (g_ui.exp_stats) {
        g_lo.card_stats = cs(x0, g_lo.hdr_stats.top + g_ui.scroll, x1, y + sd(96));
        y += sd(96);
    }
    y += gap;

    /* ---- 日志（默认收起） ---- */
    g_lo.hdr_log = cs(x0, y, x1, y + sd(38));
    g_lo.card_log = cs(x0, y, x1, y + sd(38));
    y += sd(38);
    if (g_ui.exp_log) {
        g_lo.card_log = cs(x0, g_lo.hdr_log.top + g_ui.scroll, x1, y + sd(220));
        y += sd(220);
    }

    g_lo.content_h = y + sd(12);
    g_lo.view_h = g_ui.h - ui_title_h() - ui_status_h();

    /* 夹紧滚动：内容装得下就回到顶部 */
    {
        int maxv = g_lo.content_h - (ui_title_h() + g_lo.view_h);
        if (maxv < 0) maxv = 0;
        if (g_ui.scroll > maxv) g_ui.scroll = maxv;
        if (g_ui.scroll < 0) g_ui.scroll = 0;
    }
    /* 折叠面板收起时把子控件矩形清空，避免命中到看不见的区域 */
    if (!g_ui.exp_audio) {
        for (i = 0; i < 3; i++) { SetRectEmpty(&g_lo.seg_rate[i]); SetRectEmpty(&g_lo.seg_buf[i]); }
        SetRectEmpty(&g_lo.slider);
    }
}

int ui_hit_test(int x, int y)
{
    POINT p = { x, y };
    int i;
    if (PtInRect(&g_lo.btn_close, p)) return ID_CLOSE;
    if (PtInRect(&g_lo.btn_min, p)) return ID_MIN;
    if (PtInRect(&g_lo.btn_theme, p)) return ID_THEME;
    if (PtInRect(&g_lo.btn_conn, p)) return ID_CONN;
    if (PtInRect(&g_lo.btn_install, p)) return ID_INSTALL;
    if (PtInRect(&g_lo.btn_uninstall, p)) return ID_UNINSTALL;
    if (PtInRect(&g_lo.btn_help, p)) return ID_HELP;
    if (PtInRect(&g_lo.hdr_audio, p)) return ID_EXP_AUDIO;
    if (PtInRect(&g_lo.hdr_stats, p)) return ID_EXP_STATS;
    if (PtInRect(&g_lo.hdr_log, p)) return ID_EXP_LOG;
    for (i = 0; i < 3; i++) if (PtInRect(&g_lo.seg_rate[i], p)) return ID_RATE0 + i;
    for (i = 0; i < 3; i++) if (PtInRect(&g_lo.seg_buf[i], p)) return ID_BUF0 + i;
    {
        RECT s = g_lo.slider;
        if (!IsRectEmpty(&s)) {
            InflateRect(&s, 0, sd(8));
            if (PtInRect(&s, p)) return ID_SLIDER;
        }
    }
    return ID_NONE;
}

/* 两遍：第一遍排出 content_h 并把 scroll 夹紧，第二遍用夹紧后的 scroll 重算矩形。
 * 必须两遍 —— 否则滚动超界时 rects 用的是 clamp 前的 offset，卡片与原生 EDIT
 * 会错开（实测表现：地址输入框比卡片往下掉几十像素）。 */
void ui_layout_compute(void)
{
    layout_once();
    layout_once();
}
