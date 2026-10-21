/* singleinst.h — 强制单实例
 *
 * 为什么是硬约束：womic.sys 的灌音门控是**驱动全局**的一个字节，任何新的
 * CreateFile(\\.\WOMic) 都会把它清零（diagnosis.md §10 E2 实测：开第二个句柄后
 * 第一个句柄立刻静音 rms 2545 → 0.48）。两个客户端实例同时跑必然互相踩。
 *
 * 所以：进程级互斥量 + 第二个实例把命令行转发给已有实例后自行退出。
 */
#ifndef MICLINK_SINGLEINST_H
#define MICLINK_SINGLEINST_H

#include "app.h"

/* 返回 1 = 本进程是唯一实例（调用方继续跑）；
 *      0 = 已有实例在跑，命令行已转发（调用方应立即退出）；
 *     -1 = 互斥量创建失败（保守起见当作“单实例”，继续跑）。 */
int  singleinst_init(SingleInstCb cb, SingleInstActivate act);
void singleinst_shutdown(void);

#endif /* MICLINK_SINGLEINST_H */
