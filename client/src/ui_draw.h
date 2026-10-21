/* ui_draw.h — 绘制原语与通用控件
 *
 * 全部用 GDI / GDI+ 手绘（GDI+ 是运行时 LoadLibrary，缺失自动退化为纯 GDI），
 * 不引入任何 UI 框架、不嵌图片、不嵌字体 —— 这是「单文件几十 KB + 零依赖」的
 * 前提。控件只有 8 类：卡片 / 按钮 / 分段 / 滑块 / 开关 / 折叠面板 / 电平表 / 日志。
 */
#ifndef MICLINK_UI_DRAW_H
#define MICLINK_UI_DRAW_H

#include "app.h"

/* DPI 缩放：所有尺寸都必须过一遍 sd()，否则 125%/150% 下会错位 */
int sd(int v);

/* 字体缓存：同一 (size,weight,mono,dpi) 只创建一次，DPI 变化时 ui_fonts_reset() */
HFONT ui_font(int size_pt, int weight, int mono);
void  ui_fonts_reset(void);

/* GDI+：动态加载，失败自动退化（直角 + 无抗锯齿） */
void  ui_gdiplus_init(void);
int   ui_gdiplus_ready(void);

/* 原语 */
void  ui_fill_round(HDC dc, RECT r, int rad, COLORREF c, int alpha);
void  ui_stroke_round(HDC dc, RECT r, int rad, COLORREF c, int alpha, int width);
void  ui_bg_gradient(HDC dc, RECT r);
void  ui_text(HDC dc, RECT r, const wchar_t *s, HFONT f, COLORREF c, UINT fmt);
void  ui_text_u8(HDC dc, RECT r, const char *s, HFONT f, COLORREF c, UINT fmt);

/* 图标（手绘线条，不用 Segoe MDL2 字形：字体缺失会变方块） */
enum {
    IC_WIFI = 0, IC_USB, IC_BT, IC_MIC, IC_SEARCH, IC_MIN, IC_CLOSE,
    IC_SUN, IC_MOON, IC_CHEV, IC_DOC, IC_SHIELD
};
void  ui_icon(HDC dc, RECT r, int kind, COLORREF c, int w);

/* 控件 */
void  ui_card(HDC dc, RECT r, const wchar_t *title, const wchar_t *hint);
void  ui_button(HDC dc, RECT r, const wchar_t *label, int id, int danger, int enabled);
void  ui_seg(HDC dc, RECT r, const wchar_t *label, int active, int id, int enabled);
void  ui_slider(HDC dc, RECT r, int val, int maxv, int id);
void  ui_collapse_hdr(HDC dc, RECT r, const wchar_t *title, const wchar_t *hint,
                      int expanded, int id);
void  ui_badge(HDC dc, RECT r, const wchar_t *text, COLORREF fg, COLORREF bg);

/* 各业务卡片的绘制（放在这里而不是 main.c，是为了让 main.c 保持成薄壳：
 * 事件分发 + 模块装配，不掺绘制细节） */
void  ui_draw_all(HDC dc);
void  ui_draw_titlebar(HDC dc);
void  ui_draw_conn_card(HDC dc);
void  ui_draw_drv_card(HDC dc);
void  ui_draw_audio_card(HDC dc);
void  ui_draw_stats_card(HDC dc);
void  ui_draw_log_card(HDC dc);
void  ui_draw_statusbar(HDC dc);

/* 日志环形缓冲（引擎线程 → UI 线程的字符串都先进这里） */
void  ui_log_add(const char *s, int kind);
int   ui_log_count(void);
const LogLine *ui_log_get(int i);

#endif /* MICLINK_UI_DRAW_H */
