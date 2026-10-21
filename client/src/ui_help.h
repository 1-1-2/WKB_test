/* ui_help.h — GUI 内「命令行使用说明书」窗口 */
#ifndef MICLINK_UI_HELP_H
#define MICLINK_UI_HELP_H

#include "app.h"

/* 打开（或已打开则激活）说明书窗口 */
void ui_show_help(HINSTANCE hInst, HWND parent);

/* 主窗口把键盘消息转给说明书窗口时用；返回 1 表示已处理 */
int  ui_help_handle_key(WPARAM vk);

int  ui_help_is_open(void);
void ui_help_close(void);

#endif /* MICLINK_UI_HELP_H */
