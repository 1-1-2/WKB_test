# NOTICE

`miclink-open` 是 WO Mic 协议的开源双端实现：Android 采集端（手机麦克风 → Opus → UDP）
+ Windows 客户端（UDP → Opus 解码 → 灌入虚拟麦克风驱动 → Zoom/OBS 可用）。

本项目采用 MIT 许可（见 `LICENSE`）。

> **驱动路线的变更（2026-10）**：早期计划自研内核虚拟麦克风（`driver/` 目录，
> 基于微软 `Windows-driver-samples` 改编），实测后**已弃用该路线**——自研驱动需要
> 自行签发内核签名，在未采购 EV 证书的前提下无法在客户机默认加载，且与官方
> `womic.sys` 行为存在差异。当前交付路线改为：**复用 WO Mic 官方驱动
> （`womic.sys`，未修改）**，本项目只做用户态的安装/灌音/协议两端。
> `driver/` 目录保留作技术参考，不再是交付路径。

## 第三方来源声明

### wolicheng WO Mic 驱动（womic.sys）

- **项目**：WO Mic，作者 Wolicheng（吴立成）
- **官网**：https://www.wolicheng.com/womic/
- **组件**：`womic.sys` / `womic.inf` / `womic.cat`（x64 / ARM64 / x86 三套），
  版本 2.1.0.0，微软 WHCP 签名，**未做任何修改**
- **分发方式**：仅随**编译产物** `womic.exe` 以内嵌资源（deflate 压缩）形式分发，
  用于首运自动安装虚拟麦克风；**源码仓库不含该驱动的任何二进制**
  （`client/res/drvpkg.bin` 与 `client/src/drvpkg_data.h` 已被 `.gitignore` 排除）。
  要复现内置驱动构建，需自备官方驱动包并执行 `client/tools/pack_driver.py`
  （详见 `client/README.md`「构建与驱动包」）；也可不带内置包构建，运行时用
  `--driver-pack <dir>` 指向自备驱动目录。
- **合规状态**：WO Mic 官方 EULA 是否允许再分发，**未经法律确认**（架构评审 R8）。
  缓解措施：① 驱动二进制只进 exe 产物、不进公开源码库；② 提供完全的外置驱动
  通道，用户可自行从官方渠道获取；③ 客户端卸载/安装均通过系统标准机制
  （SetupAPI/pnputil），不留私货。若后续确认不允许再分发，改为引导用户自备即可，
  代码链路已按此预留。

### microsoft/Windows-driver-samples（仅 `driver/` 参考路线）

- **项目**：`microsoft/Windows-driver-samples`
- **仓库**：https://github.com/microsoft/Windows-driver-samples
- **基线 commit**：`2dc3fd3a0cc84a2933f2194e7ec0871584979071`
- **来源路径**：`audio/simpleaudiosimple`
- **许可**：MIT License，Copyright (c) Microsoft Corporation.
  原始许可文本见同路径下的 `LICENSE` / 仓库根目录 `LICENSE`。
- **使用方式**：仅用于 `driver/` 目录下的**早期自研驱动参考实现**（已弃用，
  见上文「驱动路线的变更」）。作为内核音频驱动（虚拟麦克风端点）的骨架基线，
  在此基础上替换数据通路（改为从共享环形缓冲取 PCM）、新增 IOCTL 命令
  （见 `shared/core/include/miclink/miclink_ioctl.h`）并移除与本项目无关的示例逻辑。

微软原始许可要点（MIT）：

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## 其他第三方依赖

| 依赖 | 用途 | 许可 |
| --- | --- | --- |
| Opus (libopus) | 手机侧编码 / 服务端解码、PLC | BSD 3-Clause (Xiph.Org) |
| speexdsp | 回声消除 / 噪声抑制（服务端可选链路） | BSD 3-Clause |
| soxr | 服务端重采样（采集率 → 解码率统一） | LGPL-2.1+ |
| Microsoft Windows Driver Kit (WDK) | 内核驱动构建工具链 | 微软软件许可条款 |

## 平台中立核心层

`shared/core/` 下的代码为纯 C99 实现，同时被以下三方共用：

1. Linux 单元测试（`tests/`）
2. Windows 用户态服务端
3. Windows 内核驱动

因此该目录内禁止使用 C++ 特性、禁止 POSIX-only 头文件（除 `_WIN32` 分支外），
共享结构体禁止指针字段、全部使用定宽类型，以保证内核态 / 用户态 / 32 位 / 64 位
的二进制布局一致。
