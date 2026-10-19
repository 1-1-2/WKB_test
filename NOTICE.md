# NOTICE

`miclink-open` 是一个 Windows 虚拟麦克风系统（手机 → UDP/Opus → Windows 服务端 →
DeviceIoControl → 内核驱动 → Zoom/OBS 可用）。

本项目采用 MIT 许可（见 `LICENSE`），其中内核驱动部分基于微软官方示例代码改编。

## 第三方来源声明

### microsoft/Windows-driver-samples

- **项目**：`microsoft/Windows-driver-samples`
- **仓库**：https://github.com/microsoft/Windows-driver-samples
- **基线 commit**：`2dc3fd3a0cc84a2933f2194e7ec0871584979071`
- **来源路径**：`audio/simpleaudiosimple`
- **许可**：MIT License，Copyright (c) Microsoft Corporation.
  原始许可文本见同路径下的 `LICENSE` / 仓库根目录 `LICENSE`。
- **使用方式**：作为 miclink-open 内核音频驱动（虚拟麦克风端点）的骨架基线，
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
