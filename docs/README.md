# docs/ 文档索引

本目录收录 MicLink Open 的全部工程文档，按用途分六类。
每篇一句话摘要如下；新增文档请在此登记（路径 + 一句话 + 分类）。

## 仓库范围与历史文档（先读）

`main` 分支只含 **WO Mic 第三方兼容线**：`android/` 采集端 + `client/` Windows
客户端 + CI + 文档。自研内核驱动线（`driver/` `server/` `shared/` `tests/`）已整体
归档至 **`self-driver` 分支**（孤儿快照提交，含归档说明），不再随 `main` 分发。

下表标注 **🗄 历史资产** 的文档成文于自研线时期：内容仍有效（记录了当时的决策、
验证与事实），但其中指向 `driver/` `server/` `shared/` `tests/` 的路径已在
`self-driver` 分支；引用这些目录做复跑前，请先 `git checkout self-driver`。
自研线的弃用决策本身对兼容线有效，见
[`adr/0001-driver-route.md`](adr/0001-driver-route.md)。

## 设计

| 文档 | 一句话摘要 |
|---|---|
| [`PRD.md`](PRD.md) | 🗄 产品需求文档 v1.0：手机当麦克风的场景、功能边界与验收口径（自研线时期技术栈已过期，产品目标与兼容线一致）。 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 🗄 总体架构 v1.0：双链路模块划分、数据流与契约（自研线时期；兼容线现状以根 `README.md` 与协议规格为准）。 |
| [`architecture/incremental-2026-10.md`](architecture/incremental-2026-10.md) | 2026-10 增量架构设计与风险登记册（R1–R14），含各风险的应对与闭环状态。 |
| [`architecture/T05-delivery.md`](architecture/T05-delivery.md) | T05 交付记录：Android 主题/调色板/导航任务的实现与验收结论。 |

## 构建

| 文档 | 一句话摘要 |
|---|---|
| [`BUILD.md`](BUILD.md) | 构建说明：Android 采集端 + Windows 客户端的环境要求与构建步骤、驱动包选项与 CI 作业。 |
| [`devops/windows-interactive-debug.md`](devops/windows-interactive-debug.md) | Windows Runner 交互式联调工具选型：action-tmate 已不可用，当前用 action-debugger，附 CI 作业分工（含已归档 workflow 的历史分工）。 |

## 协议

| 文档 | 一句话摘要 |
|---|---|
| [`protocol/womic-wifi-protocol-v1.md`](protocol/womic-wifi-protocol-v1.md) | WO Mic WiFi 协议规格 v1：TCP 8125 控制面 + UDP/Opus 媒体面的完整逆向规格（附 35 个反编译证据文件，见 `protocol/evidence/`）。 |
| [`driver-whitepaper.md`](driver-whitepaper.md) | 驱动灌音协议白皮书：`\\.\WOMic` 握手链（0x222004→0x222008→0x222014→0x222010→0x222001）、48k/16bit/mono 1920B/20ms 帧、防逆向行为与驱动行为矩阵。 |

## 合规

| 文档 | 一句话摘要 |
|---|---|
| [`adr/0001-driver-route.md`](adr/0001-driver-route.md) | ADR：放弃自研内核驱动、复用官方 `womic.sys` 未修改二进制的决策记录（背景/后果/备选/合规状态）。 |

> 第三方来源与许可声明全文在仓库根 [`../NOTICE.md`](../NOTICE.md)；整体许可见 [`../LICENSE`](../LICENSE)。

## 通道

| 文档 | 一句话摘要 |
|---|---|
| [`tailscale-channel.md`](tailscale-channel.md) | CI Runner 与开发者机器的组网通道对比：upTerm SSH 直连 / 反代 / Tailscale 三方案的踩坑与选型结论。 |
| [`github-api-push.md`](github-api-push.md) | 受限网络/凭证环境下向 GitHub 推送的通道考古与结论：最终用 Git Data API 四步法（实现见 `../tools/api_push.py`）。 |

## 报告

| 文档 | 一句话摘要 |
|---|---|
| [`windows-e2e-report.md`](windows-e2e-report.md) | Windows 客户端真机端到端 CI 验证报告：三轮 run 记录、失败根因（均为测试侧）与修复。 |
| [`android-interop-checklist.md`](android-interop-checklist.md) | 与官方 WO Mic App / Client 互操作人工清单（R14 闭环）：双向连接验证步骤、已知限制与待回填的记录表。 |
| [`TEST_REPORT.md`](TEST_REPORT.md) | 🗄 全局验证与一致性审查报告（WOMP 时代快照；Android 端部分已过期，驱动/服务端部分对自研线有效）。 |
| [`VERIFICATION.md`](VERIFICATION.md) | 🗄 验证状态总账：A 类（沙箱内真实跑过）/ B 类（必须在 Windows + WDK 执行）逐条清单（自研线时期）。 |
| [`Windows-Manual-Verification.md`](Windows-Manual-Verification.md) | 🗄 Windows 人工验证手册：真机上逐条执行的预期/实际/通过三栏清单与失败处置格式（自研线时期）。 |

---

## 快速入口

- 想接入协议：先读 [`protocol/womic-wifi-protocol-v1.md`](protocol/womic-wifi-protocol-v1.md)（网络侧）+ [`driver-whitepaper.md`](driver-whitepaper.md)（驱动侧）；
- 想自己构建：先读 [`BUILD.md`](BUILD.md) 与 [`../README.md`](../README.md)；
- 想知道兼容线什么验过了、什么没有：先读 [`../README.md`](../README.md)「验证状态」与 [`windows-e2e-report.md`](windows-e2e-report.md)；
- 想在 CI/受限环境里推代码：先读 [`github-api-push.md`](github-api-push.md)；
- 想复现驱动取证结论：先读 [`driver-whitepaper.md`](driver-whitepaper.md) §7 方法论；
- 想找自研线（已归档）：`git checkout self-driver`（归档说明见该分支根 README）。
