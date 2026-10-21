/* opusdec.h — Opus 解码器封装：静态优先，dll 回退
 *
 * 静态链接（build.sh 传 -DMICLINK_HAVE_OPUS + deps/opus/lib/libopus.a）时直接调用
 * 静态符号，exe 自带解码能力，“没 opus.dll 也能出声”；
 * 没有静态库时回退到 LoadLibrary("opus.dll")，行为与老版本一致。
 *
 * 只用到 decoder：build.sh 链接时用 -Wl,--gc-sections 把 encoder 全部裁掉。
 */
#ifndef MICLINK_OPUSDEC_H
#define MICLINK_OPUSDEC_H

#include "app.h"

/* 打开（按协商采样率）。返回 1 成功。重复调用会先关掉旧的。 */
int opusdec_open(int rate);

/* 关闭并释放解码器 */
void opusdec_close(void);

/* 解码器是否可用 */
int opusdec_ready(void);

/* 解码一包。返回采样数（每个声道），<=0 表示失败。out 需容纳 frame_size 个样本。 */
int opusdec_decode(const unsigned char *pkt, int len, int16_t *out, int cap, int fec);

/* "static" / "opus.dll" / "none" */
const char *opusdec_source(void);

#endif /* MICLINK_OPUSDEC_H */
