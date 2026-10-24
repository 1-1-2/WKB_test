/* ui_layout.h — 单列纵向卡片流布局
 *
 * 单界面平铺：连接区 / 驱动状态卡 / VoiceMeter 常驻可见；
 * 音频参数、统计、日志默认收起，点标题栏展开。
 * 所有卡片在**内容坐标**里排好，再统一减掉 g_ui.scroll 落到客户区坐标，
 * 这样绘制与命中测试用的是同一套矩形，滚动时不会错位。
 */
#ifndef MICLINK_UI_LAYOUT_H
#define MICLINK_UI_LAYOUT_H

#include "app.h"

typedef struct {
    RECT title, btn_min, btn_close, btn_theme;
    RECT card_conn, ed_host, ed_port, ed_media, btn_conn;
    RECT card_drv, btn_install, btn_uninstall, btn_help;
    RECT card_meter;
    RECT card_audio, hdr_audio, seg_rate[3], seg_buf[3], slider;
    RECT card_stats, hdr_stats;
    RECT card_log, hdr_log;
    int  content_h;      /* 内容总高（不含滚动） */
    int  view_h;         /* 可视区高度（窗口高 - 标题栏 - 状态栏） */
} UiLayout;

extern UiLayout g_lo;

/* 标题栏高度 / 状态栏高度，绘制与命中测试共用 */
int ui_title_h(void);
int ui_status_h(void);
void ui_layout_compute(void);
int  ui_hit_test(int x, int y);

#endif /* MICLINK_UI_LAYOUT_H */
