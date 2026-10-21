/* cli.h — 命令行参数与无控制台输出方案 */
#ifndef MICLINK_CLI_H
#define MICLINK_CLI_H

#include "app.h"

/* 全部开关见 cli_help_text()；GUI 的「命令行使用说明书」按钮复用同一份文本，
 * 保证两处永远一致（改一处不会漏）。 */

/* -mwindows 没有控制台：带 CLI 参数时先尝试挂到父进程控制台（cmd/PowerShell），
 * 失败才自己开一个。不带参数启动（双击）时不调用本函数，因此不会闪黑窗。 */
void cli_ensure_console(int force_alloc);
void cli_release_console(void);

/* 把 host/port 写回 GUI 输入框前，供 main 读取 */
void cli_apply_to_ui(const CliArgs *a);

#endif /* MICLINK_CLI_H */
