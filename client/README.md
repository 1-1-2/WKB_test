# WO Mic Link — 轻量 PC 客户端

套用官方已签名的 `womic.sys` 虚拟麦克风驱动，自己实现 PC 端：协议、界面、注音。
不改驱动、不签驱动、不开测试模式。

![暗色主题](build/womic-dark.png)

## 体积与依赖（这是本项目的首要约束）

| 指标 | 实测值 |
|---|---|
| 可执行文件 | **83,456 B（81.5 KiB）**，单文件 |
| 静态资源 | 无（图标、圆角、渐变全部代码绘制；不嵌图片、不嵌字体） |
| 安装包 | 无（绿色单文件，双击即用） |
| 导入的系统 DLL | `KERNEL32` `USER32` `GDI32` `SHELL32` `UxTheme` `WS2_32` `msvcrt` |
| 需要额外安装的运行库 | **零** |
| 可选组件 | `opus.dll`（有则解码出声，无则只做统计与电平） |

关于 `msvcrt.dll`：它是 Windows 自带的 C 运行时（XP 起就在 `system32`），
**不是** VC++ Redistributable。因此不需要装"微软常用运行库合集"这类东西。

`gdiplus.dll` 与 `dwmapi.dll` 是**运行时按需加载**（`LoadLibrary`），不进导入表：
缺失时自动退化为纯 GDI 绘制与直角窗口，不会启动失败。

作为对比：官方客户端安装包 1.7 MB（含驱动与 adb），Electron/Qt 同类界面起步 40–120 MB。

## 功能

- **协议**：完整实现 WO Mic Wi-Fi 协议 v1 —— `101` hello → `102` 音频参数 → `103` 开始 →
  `105` 心跳（1 s/次）→ `104` 停止；UDP 解析头格式 C（`04 00` + len + seq + ts + vol + payload）。
- **注音**：`CreateFileW(L"\\\\.\\WOMic")` + `DeviceIoControl(0x222000)`，
  每 20 ms 灌入 1920 B（48 kHz / 16 bit / 单声道）。
- **界面**：Fluent 风格自绘 —— 圆角卡片、分段控件、无边框窗口（DWM 圆角/阴影）、
  深浅主题、DPI 感知、手型/缩放光标、悬停与按下反馈。
- **实时反馈**：34 段电平表 + 峰值保持、码率/帧数/丢包/延迟统计、带时间戳的事件流。
- **命令行**：`womic.exe [host] [port] [--auto] [--light]`，便于脚本化与 CI。
- **降级策略**：没有驱动 → 只做协议与统计并提示；没有 `opus.dll` → 只统计不出声；
  没有 DWM → 直角窗口。任何一环缺失都不影响启动。

![亮色主题](build/womic-light.png)

## 构建

```bash
# Linux 交叉编译（推荐，产物一致）
sudo apt install gcc-mingw-w64-x86-64
./build.sh                 # → build/womic.exe

# Windows 原生（mingw-w64 的 gcc 在 PATH 即可，无需 VS/SDK）
build.bat
```

不用 MSVC：MSVC 产物依赖 `vcruntime140.dll`，正是需求里要避免的"额外运行库"。
mingw 的 `-Os -s` 已足够小，且只链系统 DLL。

## 目录

```
src/main.c      界面：窗口、布局、自绘控件、主题、输入
src/engine.c    引擎：TCP 握手、UDP 收流、电平统计、驱动注音、可选 Opus
src/app.h       公共定义
tools/fake_server.py   服务端参考实现（无手机也能端到端联调）
build/shots/    开发过程截图（wine + Xvfb 真实渲染，非设计稿）
```

## 已验证

| 项 | 方法 | 结果 |
|---|---|---|
| 协议握手 | `tools/fake_server.py` 充当服务端 | 101 variant=4 / 102 codec=2 48000 / 103 全部接受 |
| 媒体流 | 服务端按 20 ms 下发格式 C 帧 | 客户端 78 kbps、835 帧、0 丢包（见上图统计） |
| 界面渲染 | wine 9.0 + Xvfb，真实运行 exe | 两套主题、连接/未连接态、日志时间线均正常 |
| 体积 | `stat` + `objdump -p` | 83,456 B；导入表仅系统 DLL |
| 无运行库依赖 | 导入表逐项核对 | 无 MSVC/UCRT/第三方 DLL |

## 限制（如实记录）

- **未在真实 Windows 上点开过**：界面是在 wine 里渲染验证的，DPI 缩放与 DWM 圆角
  只在代码层保证；真机首跑建议先看 `--light` 与 125%/150% 缩放。
- **驱动注音未端到端验证**：`\\.\WOMic` 只在装了官方驱动的机器上存在。
  本轮尝试在 GitHub-hosted Windows 上装驱动，卡在"装驱动会重置控制台导致 SSH 断开"，
  已用计划任务方式解耦（见 `../womic-recovery/`），实测尚在进行。
- **Opus 解码依赖外部 `opus.dll`**：不内置是为了守住 80 KB 体积。
  若确定要单文件出声，可静态链接 libopus，体积会到 300–400 KB（仍远小于 Qt/Electron）。
