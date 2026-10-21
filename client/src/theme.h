/* theme.h — 主题配色
 *
 * 深浅两套色板，外加折叠面板与 VoiceMeter 用到的层级色。
 * 所有颜色集中在 theme_apply() 一处定义，改主题只动这一个函数。
 */
#ifndef MICLINK_THEME_H
#define MICLINK_THEME_H

#include "app.h"

/* 深色/浅色切换。切换后必须同步重建画刷（EDIT 控件否则仍用旧底色），
 * 这个职责在 main.c 的 ui_toggle_theme() 里。 */
void theme_apply(int dark);

#endif /* MICLINK_THEME_H */
