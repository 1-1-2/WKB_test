/* ui_voicemeter.c — VoiceMeter
 *
 * 规格（来自需求）：
 *   · 32–40 段条（这里 34 段）
 *   · 阈值 <0.66 accent / <0.86 warn / >=0.86 err
 *   · 峰值保持线 + dBFS（等宽字体）+ 峰值% + 状态徽标（无信号 / 正常 / 过载）
 *   · attack 0.35（在 engine 侧做）、release 每秒 ×0.72
 *   · 连续 N 帧低于阈值 → 转“无信号”
 *   · 断线后 2 s 内回落
 */
#include "ui_voicemeter.h"
#include "ui_draw.h"
#include <math.h>
#include <wchar.h>

#define METER_SEGS   34
#define SILENT_LEVEL 0.004f
#define SILENT_FRAMES 40        /* 20ms/帧 → 约 0.8 s 静音即判无信号 */
#define OVER_LEVEL   0.86f

static float  s_level = 0.0f;
static float  s_peak = 0.0f;
static DWORD  s_last = 0;
static int    s_state;

int ui_meter_state(void) { return s_state; }

static float decay_per_sec(float dt)
{
    /* 每秒 ×0.72：dt 秒的衰减系数 */
    return (float)pow(0.72, (double)dt);
}

void ui_meter_update(const EngineStats *st, int running)
{
    DWORD now = GetTickCount();
    float dt = s_last ? (now - s_last) / 1000.0f : 0.016f;
    float k;
    if (dt > 0.5f) dt = 0.5f;
    if (dt < 0.0f) dt = 0.0f;
    s_last = now;
    k = decay_per_sec(dt);

    if (running && st->silent_frames < SILENT_FRAMES) {
        /* attack 由引擎完成（level += (rms-level)*0.35），这里直接用 */
        s_level = st->level;
    } else {
        /* 停止推流 / 连续静音 → release 慢回落，2 s 内基本归零 */
        s_level *= k;
        if (s_level < 0.0005f) s_level = 0.0f;
    }
    if (s_level < 0.0f) s_level = 0.0f;
    if (s_level > 1.0f) s_level = 1.0f;

    s_peak *= k;
    if (s_level > s_peak) s_peak = s_level;
    if (s_peak < 0.002f) s_peak = 0.0f;

    if (!running) s_state = 0;
    else if (s_level >= OVER_LEVEL) s_state = 2;
    else if (st->silent_frames >= SILENT_FRAMES) s_state = 0;
    else s_state = 1;
}

void ui_meter_draw(HDC dc, RECT r)
{
    int x0 = r.left + sd(16), x1 = r.right - sd(16);
    int bar_top = r.top + sd(36), bar_bot = r.bottom - sd(52);
    int gap = sd(2);
    int bw = ((x1 - x0) - gap * (METER_SEGS - 1)) / METER_SEGS;
    int lit, peak_idx, i;
    wchar_t buf[64];

    ui_card(dc, r, L"输入电平", NULL);
    if (bw < 2) bw = 2;
    if (bar_bot <= bar_top) return;

    lit = (int)(s_level * METER_SEGS + 0.5f);
    peak_idx = (int)(s_peak * METER_SEGS + 0.5f);

    for (i = 0; i < METER_SEGS; i++) {
        RECT br = { x0 + i * (bw + gap), bar_top,
                    x0 + i * (bw + gap) + bw, bar_bot };
        COLORREF c = g_th.meter_off;
        if (i < lit) {
            float t = (float)i / (float)METER_SEGS;
            if (t >= 0.86f) c = g_th.err;
            else if (t >= 0.66f) c = g_th.warn;
            else c = g_th.accent;
        }
        ui_fill_round(dc, br, sd(2), c, 255);
    }

    /* 峰值保持线 */
    if (peak_idx > 0 && peak_idx <= METER_SEGS) {
        int px = x0 + peak_idx * (bw + gap);
        RECT pr = { px - sd(1), bar_top, px + bw + sd(1), bar_top + sd(2) };
        ui_fill_round(dc, pr, sd(1), g_th.text_mute, 220);
    }

    /* 底部读数：dBFS（等宽）+ 峰值% */
    if (s_level > 0.0005f)
        swprintf(buf, 64, L"%6.1f dBFS", (double)(20.0f * (float)log10((double)s_level)));
    else
        wcscpy(buf, L"    -inf dBFS");
    {
        RECT tr = { r.left + sd(16), r.bottom - sd(46), r.left + sd(160), r.bottom - sd(24) };
        ui_text(dc, tr, buf, ui_font(10, FW_NORMAL, 1), g_th.text, DT_LEFT | DT_SINGLELINE);
    }
    {
        RECT tr = { r.right - sd(240), r.bottom - sd(46), r.right - sd(150), r.bottom - sd(24) };
        swprintf(buf, 64, L"峰值 %3.0f%%", (double)(s_peak * 100.0f));
        ui_text(dc, tr, buf, ui_font(10, FW_NORMAL, 1), g_th.text_dim, DT_RIGHT | DT_SINGLELINE);
    }

    /* 状态徽标 */
    {
        RECT br = { r.right - sd(140), r.bottom - sd(48), r.right - sd(16), r.bottom - sd(22) };
        COLORREF fg, bg;
        const wchar_t *txt;
        if (s_state == 2) {
            txt = L"过载"; fg = RGB(255, 255, 255); bg = g_th.err;
        } else if (s_state == 1) {
            txt = L"正常";
            fg = g_th.dark ? RGB(10, 18, 24) : RGB(255, 255, 255);
            bg = g_th.ok;
        } else {
            txt = L"无信号"; fg = g_th.text_dim;
            bg = g_th.dark ? RGB(44, 48, 58) : RGB(232, 235, 241);
        }
        ui_badge(dc, br, txt, fg, bg);
    }
}
