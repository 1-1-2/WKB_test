/* drvinst.h — 驱动检测 / 安装 / 卸载
 *
 * 判定「是否已安装」的三级判据（架构文档 1.2.1）：
 *   L1 快路径  CreateFile(\\.\WOMic) 成功 —— 复用 devio 的全局唯一句柄，
 *              不为了探测多开句柄（会踩掉门控）
 *   L2 权威    SetupAPI 枚举 MEDIA 类 + 注册表 InfPath 非空
 *   L3 辅助    %SystemRoot%\System32\drivers\womic.sys 落盘
 *
 * 铁律：**不看安装工具的输出**。devcon 会先打印 "Device node created" 再选驱动，
 * 失败也是这句；pnputil 同理。只看上面三个判据。
 *
 * 安装流程（必须在提权子进程里跑）：
 *   释放三件套 → 导入 catalog 签名者到 TrustedPublisher（crypt32 API，不派生
 *   certutil）→ SetupAPI 自建 root-enumerated devnode（Root\WOMic）
 *   → UpdateDriverForPlugAndPlayDevices → 再走一遍 L1/L2/L3 判定
 */
#ifndef MICLINK_DRVINST_H
#define MICLINK_DRVINST_H

#include "app.h"

/* 驱动硬件 ID（2022 版）。2017 版是 *WOMic，一律不用。 */
#define WOMIC_HWID L"Root\\WOMic"

/* 把 setupapi.dev.log 里跟本次安装相关的片段抓出来（失败兜底用） */
int drv_grab_devlog(wchar_t *out, int cap);

#endif /* MICLINK_DRVINST_H */
