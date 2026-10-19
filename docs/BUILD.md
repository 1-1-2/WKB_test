# 构建说明（Windows 驱动 + 服务端 + Android 端）

> **状态提示**：本仓库的开发沙箱是 **Linux**，**没有安装过 WDK、没有编译过驱动、没有打开过 Visual Studio**。
> 本文列出的步骤均依据工程文件（`.sln` / `.vcxproj` / `.inx` / `CMakeLists.txt`）与 WDK 的标准流程编写，
> **属于待真机验证项**（B 类，见 `docs/VERIFICATION.md`）。

---

## 1. 环境要求

| 组件 | 版本要求 | 备注 |
|---|---|---|
| Windows | **Win11 21H2（build 22000）或更高**（INF 声明值）；源码下限 Win10 2004（build 19041） | ⚠️ 两者不一致，见下面「Windows 版本下限」 |
| Visual Studio | 2022（17.4+） | 必须勾选“使用 C++ 的桌面开发” |
| Windows SDK | 10.0.22621.x（或更新） | **版本号必须与 WDK 完全一致** |
| WDK | Windows 11 WDK 10.0.22621.x（或更新） | 安装后 VS 里会出现 “Driver” 相关模板 |
| 签名工具 | `signtool.exe`（随 SDK 安装） | 位于 `C:\Program Files (x86)\Windows Kits\10\bin\<版本>\x64\` |
| 架构 | **x64** | 64 位 Windows 强制要求驱动签名；x86/ARM64 需自行改配置 |

### Windows 版本下限（两个口径，不要只看一个）

| 口径 | 值 | 依据 |
|---|---|---|
| **INF 声明的目标 OS**（决定能不能装） | **Win11 21H2（build 22000）或更高** | `driver/Source/Main/MicLinkVirtualMic.inx`：`%MfgName%=MICLINK,NT$ARCH$.10.0...22000` |
| **源码实际需要的 OS**（决定能不能跑） | **Win10 2004（build 19041）或更高** | 用了 `ExAllocatePool2`（Win10 2004+：`NewDelete.cpp:45`、`adapter.cpp:173`、`minwavertstream.cpp:289` 等）与 `EX_TIMER_HIGH_RESOLUTION`（Win10 1803+：`minwavertstream.cpp:262`） |

在 Win10 19041~21999 上，即使驱动编译通过也**装不上**：INF 没为这些 build 声明模型，
`pnputil /add-driver` 会报“找不到匹配的模型”。

若要支持 Windows 10，改 INF 的两处（段名与 TargetOS 必须一起改）：

```inf
[Manufacturer]
%MfgName%=MICLINK,NT$ARCH$.10.0...19041

[MICLINK.NT$ARCH$.10.0...19041]
%MICLINK_VM.DeviceDesc%=MICLINK_VM, ROOT\MicLinkVirtualMic
```

并同步更新 `driver/verify/check_driver_consistency.py`（该脚本在 `check_inf()` 里断言
`INF Manufacturer 段已改为 MICLINK`，写死了 `NT$ARCH$.10.0...22000`）、`README.md`
与 `docs/Windows-Manual-Verification.md`。

> 以上 INF TargetOS 调整**未在 Windows 上验证过**（B 类，仅静态推导）。

**WDK 与 SDK 不匹配**是新手最常见的坑：两者版本号必须成对（例如都是 10.0.22621.0）。
装完 WDK 后若 VS 提示找不到 `WindowsSdkDir`，去“控制面板 → 程序和功能”确认 SDK 与 WDK 版本一致，
或在 VS 里“重定解决方案目标”。

### 1.1 免安装构建（推荐，与 CI 完全一致）

本仓库驱动工程采用**微软官方 Windows-driver-samples 的 NuGet 模式**：WDK 的头文件、km 库、
`WindowsKernelModeDriver10.0` 工具集全部来自 NuGet 包（`driver/packages.config`），
**不需要在机器上安装 WDK**，也不需要 VS 的 WDK 集成：

```powershell
nuget restore driver\packages.config -PackagesDirectory driver\packages
msbuild driver\MicLinkVirtualMic.sln -t:rebuild -m -nologo -v:minimal `
  -p:Configuration=Release -p:Platform=x64 `
  -p:TargetVersion=Windows10 `
  -p:_NT_TARGET_VERSION=0xA000012 `
  -p:InfVerif_AdditionalOptions=/samples
```

- `nuget.exe` 可从 <https://www.nuget.org/downloads> 单独下载，或随 VS 安装。
- 版本对应：WDK 包 `10.0.28000.2526` ↔ `_NT_TARGET_VERSION=0xA000012`（NTDDI 码，10.0.28000）。
- 在装有 WDK 的 VS 2022/2026 里直接打开 `.sln` 也能编译（`Directory.Build.props` 的三个
  Import 都带 `Exists` 条件，没有还原过包时会自动跳过，不影响 IDE 打开）。

---

## 2. 编译内核驱动

### 2.1 打开解决方案

```
driver\MicLinkVirtualMic.sln
```

用 VS 2022 打开后，解决方案里有 **5 个项目**：

| 项目 | 作用 |
|---|---|
| `Main` | 驱动主体：适配器、WaveRT 微型端口、控制设备 `\\.\MicLinkControl` |
| `Filters` | 波形/拓扑过滤器的引脚与数据格式定义 |
| `Utilities` | 辅助代码（共享 DSP / 环形缓冲的 C 实现等） |
| `Inc` | 公共头文件 |
| `package` | 驱动**包**工程：产出 INF（由 `.inx` 生成）、拷贝 sys、生成 cat |

### 2.2 选对配置与平台（**这一步错了后面全白搭**）

菜单 **生成 → 配置管理器**：

| 设置项 | 必须选 |
|---|---|
| 活动解决方案配置 | `Debug`（开发期）或 `Release`（分发） |
| 活动解决方案平台 | **`x64`** |

> **必须让 5 个项目的配置与平台完全一致**。默认可能是 `Win32`，逐个项目改成 `x64`。
> 平台不一致的典型症状：链接时报 `LNK1112: 模块计算机类型“x86”与目标计算机类型“x64”冲突`。

### 2.3 编译

**生成 → 生成解决方案**（或 `Ctrl+Shift+B`）。

也可以用命令行（VS 开发者命令行，或任意带 MSBuild 的环境）：

```powershell
msbuild driver\MicLinkVirtualMic.sln /p:Configuration=Release /p:Platform=x64 /m
```

若本机没装 WDK，用上面的 1.1 节 NuGet 模式（CI 即用此模式，runner 为 `windows-2025-vs2026`）。

### 2.4 产物在哪里

编译输出目录形如（取决于配置/平台）：

```
driver\x64\Release\                    <- 或 driver\Source\Main\x64\Release\
├── miclinkvirtualmic.sys                驱动本体
├── MicLinkVirtualMic.inf                由 MicLinkVirtualMic.inx 生成
└── MicLinkVirtualMic.cat                目录文件（签名后才有；Package 工程生成）
```

| 文件 | 来源 | 说明 |
|---|---|---|
| `miclinkvirtualmic.sys` | `Main` 工程链接产物 | 文件名与 INF 的 `ServiceBinary=%13%\miclinkvirtualmic.sys` 对应 |
| `MicLinkVirtualMic.inf` | `Source\Main\MicLinkVirtualMic.inx` | `.inx` 是模板，构建时按目标架构展开 `$ARCH$` 生成 `.inf` |
| `MicLinkVirtualMic.cat` | `Package` 工程（`inf2cat` / `stampinf` 流程） | 未签名时可能不存在；测试签名前需要先有它 |

INF 关键字段（供核对）：

| 字段 | 值 |
|---|---|
| 设备类 | `MEDIA`（ClassGUID `{4d36e96c-e325-11ce-bfc1-08002be10318}`） |
| 硬件 ID | `ROOT\MicLinkVirtualMic`（软件枚举设备，非即插即用硬件） |
| 服务名 | `MicLinkVirtualMic` |
| 服务二进制 | `%13%\miclinkvirtualmic.sys` |
| 目录文件 | `MicLinkVirtualMic.cat` |
| 设备描述 | `MicLink Virtual Microphone` |

> 找不到 INF 时，`tools/install.ps1` 会在仓库里递归搜索；也可以用 `-InfPath` 显式指定。

### 2.5 常见编译错误排查

| 错误 | 原因 / 处理 |
|---|---|
| `MSB8036: 找不到 Windows SDK 版本` | SDK/WDK 版本不匹配或未安装 → 安装对应版本 SDK；或右键项目“重定目标” |
| `LNK1112: 模块计算机类型冲突` | 平台不一致 → 配置管理器里 5 个项目都改成 `x64`，然后**清理后重新生成** |
| `error C2220 / C4xxx: 将警告视为错误` | 驱动工程默认 `/WX` → 修掉对应警告；或在 `Main.vcxproj` 里对该文件关 `/WX`（不推荐长期关） |
| `inf2cat : error ... DriverVer` | `.inx` 里的 `DriverVer=` 日期格式必须是 `MM/DD/YYYY, x.x.x.x` |
| `error : 无法找到 miclinkvirtualmic.sys`（Package 工程） | 先单独生成 `Main` 工程（`Package` 依赖它的输出） |
| `error MSB3073: ... exited with code 1`（StampInf/Inf2Cat 步骤） | Package 工程的 post-build 脚本失败：确认以**管理员**身份运行 VS，且路径无中文/空格 |
| `C4700/C4715` 之类在内核代码里 | 内核代码不允许未初始化变量，按提示初始化（尤其 IOCTL 输入参数） |
| 编译通过但安装时报代码 52 | 不是编译问题，是签名问题 → 见 `docs/Signing-Guide.md` |

---

## 3. 编译 Windows 服务端

依赖：libopus（必需）、libsoxr（推荐，重采样音质更好）、libspeexdsp（可选）。

```powershell
# 用 vcpkg 装依赖（x64 静态）
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install opus:x64-windows-static soxr:x64-windows-static speexdsp:x64-windows-static

# 构建
cmake -B server\build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DMICLINK_WITH_FAKE_DEVICE=OFF
cmake --build server\build --config Release
```

产物：`server\build\Release\miclinkserver.exe`。

> **Windows 上必须 `MICLINK_WITH_FAKE_DEVICE=OFF`**，否则 `--backend auto` 会解析到内置 Fake 后端，
> 不会真的往驱动写 PCM。详细参数见 `server/build_windows.md`。

运行：

```powershell
miclinkserver.exe --port 5004 --rate 48000               # 最常用
miclinkserver.exe --port 5004 --rate 48000 --opus-rate 16000   # 手机 16k、端点 48k（服务端重采样）
miclinkserver.exe --port 5004 --discovery on             # 开启广播发现（手机端可自动找到）
miclinkserver.exe --config miclinkserver.ini               # 用配置文件
miclinkserver.exe --dry-run                              # 只打印解析后的配置，不启动（排障首选）
```

---

## 4. 构建 Android 采集端

见 [`android/README.md`](../android/README.md)。**前置必做**：把 opus 1.4+ 的 release 源码放到
`android/app/src/main/cpp/opus/`，否则 CMake 会以清晰的 `FATAL_ERROR` 中止（不会静默失败）。

```bash
cd android
./gradlew :app:assembleDebug
```

---

## 5. 签名与安装

| 目的 | 命令 |
|---|---|
| 测试签名（本机自测） | `tools\make-test-cert.ps1` → `tools\install.ps1 -Mode TestSign -Force` → **重启** |
| 生产安装（已正式签名） | `tools\install.ps1 -Mode Production -InfPath <inf>` |
| 安装后自检 | `tools\verify.ps1` |
| 卸载 | `tools\uninstall.ps1` |

签名细节见 [`docs/Signing-Guide.md`](Signing-Guide.md)；安装后的人工验证见
[`docs/Windows-Manual-Verification.md`](Windows-Manual-Verification.md)。

---

## 6. 只想在 Linux 上跑已有的自动化验证

驱动相关只能静态检查，其余可以真跑：

```bash
# shared/core：7 个单元测试 + 协议 fuzz
cd tests && ./run_tests.sh

# driver：127 项静态一致性自校验（不编译驱动）
python3 driver/verify/check_driver_consistency.py

# server：Linux 真实构建 + 端到端 test_pipeline
cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release && cmake --build server/build
ctest --test-dir server/build --output-on-failure
```

详见 [`docs/VERIFICATION.md`](VERIFICATION.md)。
