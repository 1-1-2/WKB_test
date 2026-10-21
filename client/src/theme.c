/* theme.c — 两套色板 */
#include "theme.h"

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
        g_th.meter_off = RGB(48, 52, 63);
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
        g_th.meter_off = RGB(226, 231, 239);
        g_th.dark      = 0;
    }
}
