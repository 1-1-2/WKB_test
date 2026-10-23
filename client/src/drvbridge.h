/* drvbridge.h — 应用 → 驱动安装器的转发层
 *
 * 拆分背景：驱动三件套（第三方已签名二进制）随身帯 Wi-Fi 只会扩大合规面。
 * 现在分工是——
 *   womic.exe            应用本体：GUI / CLI / 网络层 / 驱动调用，**不含第三方二进制**
 *   womic-drvinst.exe    驱动安装器：只负责装/卸/查驱动，携带驱动包
 *
 * 因此应用侧所有「安装/卸载驱动」的动作都不再自己干活，而是转发给同目录的
 * womic-drvinst.exe。**安装逻辑有且只有一份**（drvinst.c 里的 drv_install），
 * 两个 exe 共用同一份实现，不存在「两套安装逻辑」。
 *
 * 启动方式的设计取舍（这里踩过坑，勿简化）：
 *   1. 优先 CreateProcessW 直接起子进程。父进程已提权时它天然继承令牌、
 *      不弹 UAC，且能拿到真实退出码（ShellExecute 拿不到子进程退出码）。
 *   2. 父进程未提权时，带 requireAdministrator manifest 的子进程会让
 *      CreateProcessW 直接失败，返回 ERROR_ELEVATION_REQUIRED(740)。这时才
 *      退回 ShellExecuteExW("runas") 弹一次 UAC。
 *   3. 两条路都失败才报错。干活的始终是同一个 /install 参数。
 */
#ifndef MICLINK_DRVBRIDGE_H
#define MICLINK_DRVBRIDGE_H

#include "app.h"

/* 安装器文件名与可选覆盖环境变量（后者仅供调试/自测定位备用安装器） */
#define DRVINST_EXE_W   L"womic-drvinst.exe"
#define DRVINST_EXE_A   "womic-drvinst.exe"
#define DRVINST_ENV_W   L"MICLINK_DRVINST"

/* 转发动作 */
typedef enum {
    DRVOP_UNINSTALL = 0,   /* /uninstall */
    DRVOP_INSTALL   = 1,   /* /install   */
    DRVOP_STATUS    = 2    /* /status    */
} DrvOp;

/* 定位同目录的 womic-drvinst.exe；找到返回 1 并把绝对路径写进 path */
int  drvbridge_find(wchar_t *path, int cap);

/* 同步执行并等子进程结束；*exit_code 收到安装器的退出码（失败时置 -1） */
int  drvbridge_run(DrvOp op, const wchar_t *pack, int *exit_code);

/* 异步拉起（GUI 的按钮用：不等结果，界面靠 2s 一次的驱动状态复检跟进） */
int  drvbridge_launch(DrvOp op, const wchar_t *pack);

/* 上一句人话错误（用于 UI 日志 / 控制台输出） */
const char *drvbridge_last_error(void);

#endif /* MICLINK_DRVBRIDGE_H */
