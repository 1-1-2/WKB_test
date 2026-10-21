/* ui_voicemeter.h — VoiceMeter 控件 */
#ifndef MICLINK_UI_VOICEMETER_H
#define MICLINK_UI_VOICEMETER_H

#include "app.h"

/* 0 = 无信号，1 = 正常，2 = 过载 */
int  ui_meter_state(void);

/* 每帧调用：做 release 平滑与状态判定（attack 已在引擎侧完成，系数 0.35） */
void ui_meter_update(const EngineStats *st, int running);

/* 在卡片矩形内绘制 34 段条 + 峰值保持线 + dBFS + 峰值% + 状态徽标 */
void ui_meter_draw(HDC dc, RECT r);

#endif /* MICLINK_UI_VOICEMETER_H */
