# WO Mic Link — 轻量 PC 客户端

套用官方已签名的 `womic.sys` 虚拟麦克风驱动，自己实现 PC 端：协议、界面、注音。
不改驱动、不签驱动、不开测试模式。

![暗色主题](build/womic-dark.png)

## 体积与依赖（这是本项目的首要约束）

| 指标 | 实测值 |
|---|---|
| 可执行文件 | **396,800 B（387.5 KiB）**，单文件（含内置驱动包与静态 Opus 解码器） |
| 静态资源 | 无（图标、圆角、渐变全部代码绘制；不嵌图片、不嵌字体） |
| 安装包 | 无（绿色单文件，双击即用） |
| 导入的系统 DLL | `KERNEL32` `USER32` `GDI32` `SHELL32` `UxTheme` `WS2_32` `msvcrt` `ADVAPI32` `ole32` `SETUPAPI` `newdev` `CRYPT32`（共 12 个，全部为系统自带） |
| 需要额外安装的运行库 | **零** |
| 可选组件 | 无（Opus 已静态链接；见下） |

关于 `msvcrt.dll`：它是 Windows 自带的 C 运行时（XP 起就在 `system32`），
**不是** VC++ Redistributable。因此不需要装"微软常用运行库合集"这类东西。

后 5 个系统 DLL（`ADVAPI32` `ole32` `SETUPAPI` `newdev` `CRYPT32`）随
"客户端自助安装驱动"功能引入（SetupAPI/newdev 枚举安装、ole32/crypt32/
advapi32 辅助），仍是系统自带，不构成额外运行库依赖。

Opus 解码**默认静态链接**（`build.sh` 检测到 `deps/opus/lib/libopus.a` 时
传 `-DMICLINK_HAVE_OPUS`，`-Wl,--gc-sections` 把 encoder 全部裁掉）——
没有 `opus.dll` 也能出声；只有在没有静态库的干净环境构建时才回退到运行时
`LoadLibrary("opus.dll")`，功能不缺。

`gdiplus.dll` 与 `dwmapi.dll` 是**运行时按需加载**（`LoadLibrary`），不进导入表：
缺失时自动退化为纯 GDI 绘制与直角窗口，不会启动失败。

作为对比：官方客户端安装包 1.7 MB（含驱动与 adb），Electron/Qt 同类界面起步 40–120 MB。

## 功能

- **协议**：完整实现 WO Mic Wi-Fi 协议 v1 —— `101` hello → `102` 音频参数 → `103` 开始 →
  `105` 心跳（1 s/次）→ `104` 停止；UDP 解析头格式 C（`04 00` + len + seq + ts + vol + payload）。
- **注音**：`CreateFileW(L"\\\\.\\WOMic")` 后走完整握手链——`0x222004` 读 1 字节
  握手 key → `0x222008` 提交 key^0x3B → `0x222014` 设环形缓冲容量（2 B 大端，
  钳制 [1, 11520]）→ `0x222010(1)` 启动 → 每 20 ms 灌入 1920 B（48 kHz /
  16 bit / 单声道）→ `0x222010(0)` 停止。旧编号 `0x222000` 是 2017 驱动遗物，
  在 2022 驱动上 51/51 失败，已删除。
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

## 构建与驱动包

**源码库不含第三方驱动二进制。** 内置的 `womic.sys / womic.inf / womic.cat`
版权归 Wolicheng（WO Mic）所有，虽是 WHCP 已签名的官方驱动，公开仓库再分发
仍有法律风险（架构文档 R8）。因此 `res/drvpkg.bin` 与 `src/drvpkg_data.h`
都是**生成物、已列入 `.gitignore`**，干净检出里没有它们。

`./build.sh` 发现这两个文件缺失时，先尝试真实打包（`tools/pack_driver.py`
按下面的顺序找驱动目录）；找不到就用 `--empty` 生成一个合法空包
（magic `MICLDRV1`、ver=1、count=0，16 字节）兜底并打 warned 日志，
构建不中断，产出的 exe 约 300 KB，导入表仍全是系统 DLL。

要产出**内置驱动**的 exe，自备官方驱动目录后重新生成：

```bash
# 查找顺序：--driver-dir 参数 > 环境变量 WOMIC_DRIVER_DIR > 内置候选路径列表
python3 tools/pack_driver.py --driver-dir /path/to/womic2022
./build.sh          # → build/womic.exe（~396 KB，内含三套驱动）
./build.sh test     # wine 下校验 9 个驱动文件的字节一致性（9/9 PASS）
```

目录里需要 2022 版（2.1.0.0）三件套：`womic.inf` / `womic_1.inf` / `womic_2.inf`
及同名 `.sys` / `.cat`，硬件 ID `Root\WOMic`。2017 版（`womic_3/4/5/6`）
握手长度检查与 2022 版不兼容，一律不打包。

不带内置驱动的 exe 通过 `--driver-pack` 外置驱动工作：

```
womic.exe --driver-pack D:\drivers\womic2022 --install-driver
womic.exe --driver-pack D:\drivers\womic2022 --status
```

`--status` 的「驱动包来源」一栏会显示当前是内置资源、内置数组还是空包。
空包构建下 `./build.sh test` 会明确报告"无内置驱动包"并以非零退出，
绝不假装 PASS。

## 目录

```
src/main.c      界面：窗口、布局、自绘控件、主题、输入
src/engine.c    引擎：TCP 握手、UDP 收流、电平统计、驱动注音、可选 Opus
src/app.h       公共定义
tools/fake_server.py   服务端参考实现（无手机也能端到端联调）
tools/pack_driver.py   驱动包/图标生成器（--driver-dir / --empty，见上一节）
build/shots/    开发过程截图（wine + Xvfb 真实渲染，非设计稿）
```

## 已验证

| 项 | 方法 | 结果 |
|---|---|---|
| 协议握手 | `tools/fake_server.py` 充当服务端 | 101 variant=4 / 102 codec=2 48000 / 103 全部接受 |
| 媒体流 | 服务端按 20 ms 下发格式 C 帧 | 客户端 78 kbps、835 帧、0 丢包（见上图统计） |
| 界面渲染 | wine 9.0 + Xvfb，真实运行 exe | 两套主题、连接/未连接态、日志时间线均正常 |
| 体积 | `stat` + `objdump -p` | 396,800 B；导入表 12 个 DLL 全部为系统自带 |
| 无运行库依赖 | 导入表逐项核对 | 无 MSVC/UCRT/第三方 DLL |

## 限制（如实记录）

- **真机覆盖范围（如实记录）**：驱动安装与注音闭环已在 GitHub Actions
  Windows 真机端到端验证——`windows-e2e` 的 d/e/f 步：客户端自助安装驱动
  成功（`install_state=ok`），SYSTEM 计划任务跑 `e2e_probe` 拿到
  `RESULT=PASS`（waveIn 录到 288000 B / 1 kHz 正弦）。"装驱动会重置控制台
  导致 SSH 断开"的早期障碍已用计划任务方式解耦（f 步的设计前提）。
  GUI 冒烟已真机通过（h 步：无参启动 6 秒进程存活，主窗口标题 `WO Mic Link`）。
  R3 2×2 权限裁决矩阵齐备：`(SYSTEM, Session 0)=PASS`、
  `(标准用户, Session 0)=PASS`、`(标准用户, 交互会话)=FAIL`
  （`waveInOpen rc=1`）、历史 `(管理员, 交互会话)=FAIL`——
  **灌音约束取决于会话上下文（Session 0 vs 交互会话），与用户权限无关**。
  DPI 缩放与 DWM 圆角仍只在代码层保证，真机首跑建议先看 `--light` 与
  125%/150% 缩放。
- **Opus 已静态链接**：默认构建（`deps/opus` 就位时）把 Opus 解码器编进
  exe，没有 `opus.dll` 也能出声；仅在干净环境无静态库时才回退运行时加载。
