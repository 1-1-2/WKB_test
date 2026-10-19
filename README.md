# MicLink Open —— Windows 虚拟麦克风（手机当麦克风）

把 Android 手机的麦克风，变成 Windows 里一枚**真实的录音设备**：
Zoom / 腾讯会议 / OBS / 系统录音机都能直接选中它，**不需要安装任何第三方虚拟声卡**（VB-Cable 之类）。

```
  Android 手机                         Windows 主机
┌──────────────────────┐          ┌──────────────────────────────────────────┐
│ AudioRecord          │          │ miclinkserver.exe（用户态）                │
│ VOICE_COMMUNICATION  │          │   UDP 收包 → Opus 解码 + PLC             │
│   ↓ PCM16            │          │   → 抖动缓冲 → 重采样 → 环形缓冲         │
│ libopus 编码 (JNI)   │  UDP     │        ↓ DeviceIoControl(WRITE_PCM)      │
│   ↓ Opus payload     │ =======> │ ┌────────────────────────────────────┐   │
│ Packetizer 32B 头    │ Opus     │ │ miclinkvirtualmic.sys（内核）        │   │
│   ↓ UDP 5004         │ 5004     │ │  控制设备 \\.\MicLinkControl         │   │
└──────────────────────┘          │ │  WaveRT 捕获端点（虚拟麦克风）     │   │
                                  │ └───────────────┬────────────────────┘   │
                                  │                 ↓ 音频栈                 │
                                  │   设置 → 系统 → 声音 → 输入设备          │
                                  │   "MicLink Virtual Microphone"             │
                                  │        ↓                                 │
                                  │   录音机 / OBS / Zoom / 腾讯会议         │
                                  └──────────────────────────────────────────┘
```

---

## ⚠️ 先读这段：验证状态

本仓库的开发环境是 **Linux 沙箱**。下面这些**已经在沙箱里真实跑过**：

* `shared/core` 的 7 个单元测试（含 1000 万帧无锁 SPSC 压力、15 万轮 ASan/UBSan 协议 fuzz、16 组 Opus 往返 SNR）；
* `server` 的 Linux 构建 + `test_pipeline` 端到端 64 项断言（含 FEC 回归场景 E/F）；
* `driver` 的 127 项**静态一致性**自校验（不是编译，不是运行）；
* Android 端封包字节布局与 `shared/core` 权威实现的**逐字节比对**（`android/verify/protocol_crosscheck`）；
* Android 端 JNI 接线冒烟测试（**链接假 Opus**，只验符号名/签名/错误码：`android/verify/jni_smoketest`）。

下面这些**从未在本仓库里跑过**，需要你在 Windows 真机上完成：

* **WDK 驱动编译**（沙箱没有 WDK）；
* **驱动安装、端点出现在输入设备列表**；
* **录音机 / OBS / Zoom 真实采集到手机的声音**；
* 24 小时稳定性。

详细清单与复跑命令见 **[`docs/VERIFICATION.md`](docs/VERIFICATION.md)**。
拿到真机后请按 **[`docs/Windows-Manual-Verification.md`](docs/Windows-Manual-Verification.md)** 逐条验证。

**本项目交付的是“代码 + 测试签名可安装”的状态；生产签名必须由证书持有者自行完成。**

---

## 目录结构

```
miclink-open/
├── shared/core/            三方共用的协议与 DSP 契约（唯一事实来源）
│   ├── include/miclink/      miclink_protocol.h / miclink_ioctl.h / miclink_sample_rate.h ...
│   └── src/                protocol.c / ring_buffer.c / jitter_buffer.c / dsp.c ...
├── driver/                 Windows 内核虚拟麦克风驱动（WDM/KS + WaveRT）
│   ├── Source/             Main / Filters / Utilities / Inc
│   ├── Package/            驱动包工程（产出 inf + cat）
│   └── verify/             check_driver_consistency.py（127 项静态自校验）
├── server/                 Windows 用户态服务端（UDP → Opus 解码 → IOCTL）
│   ├── src/                udp_receiver / opus_decoder / jitter_pipeline / injector ...
│   ├── tests/              test_pipeline（端到端 64 项断言，含 FEC 回归）
│   └── build_windows.md    Windows 构建说明
├── android/                Android 采集端（Kotlin + AudioRecord + libopus JNI）
│   └── verify/             Linux 上可跑的交叉校验（封包字节 / JNI 接线）
├── tools/                  Windows 安装 / 卸载 / 签名 / 验证脚本（PowerShell）
├── tests/                  shared/core 的单元测试与 fuzz（ctest，7 个用例）
└── docs/                   构建、签名、人工验证、验证状态
```

**契约唯一事实来源**：`shared/core/include/miclink/miclink_protocol.h`（UDP 包）与
`miclink_ioctl.h`（IOCTL）。驱动、服务端、Android 端**共用同一份头文件**，禁止各自复制结构体定义。

---

## 快速开始

### 路线 A：测试签名（**仅限本机开发自测**）

| 步骤 | 命令 / 动作 |
|---|---|
| 1. 编译驱动 | 见 [`docs/BUILD.md`](docs/BUILD.md)，产出 `MicLinkVirtualMic.inf` / `miclinkvirtualmic.sys` |
| 2. 编译服务端 | 见 [`server/build_windows.md`](server/build_windows.md)，产出 `miclinkserver.exe` |
| 3. 测试签名 | `powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1` |
| 4. 安装驱动 | `powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force` |
| 5. **重启电脑** | 必须；重启后桌面右下角会出现 **“测试模式”水印** |
| 6. 自动检查 | `powershell -ExecutionPolicy Bypass -File tools\verify.ps1` |
| 7. 启动服务端 | `miclinkserver.exe --port 5004 --rate 48000`（若要手机自动发现，加 `--discovery on`） |
| 8. 手机采集 | 用 Android Studio 构建 `android/`（先按 `android/README.md` 放入 opus 源码），填 PC 的 IP → 开始采集 |
| 9. 人工验证 | 按 [`docs/Windows-Manual-Verification.md`](docs/Windows-Manual-Verification.md) 用录音机 / OBS / Zoom 验证 |

> 测试签名**不能分发给普通用户**：在未开启测试模式的机器上驱动会加载失败（代码 52），
> 而要求用户关闭系统安全保护在合规与安全上都不可接受。

### 路线 B：生产签名（可分发）

| 步骤 | 说明 |
|---|---|
| 1. 取得 EV 代码签名证书 | 向 CA 购买（DigiCert / Sectigo / GlobalSign 等），需要组织实名验证 |
| 2. 走 WHQL 完整认证 或 Attestation 签名 | Microsoft Partner Center 提交（详见 [`docs/Signing-Guide.md`](docs/Signing-Guide.md)） |
| 3. 用正式签名重签 `.sys` / `.cat` | 本仓库不提供该步骤的脚本（需要你的证书与 Partner Center 账号） |
| 4. 安装 | `powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode Production -InfPath <你的 inf>` |
| 5. 同路线 A 的 6~9 步 | 生产模式**不需要**开启测试签名，也不会出现“测试模式”水印 |

`install.ps1 -Mode Production` 会严格校验签名，发现未签名/无效签名**直接报错退出**，
绝不会悄悄降级成测试签名。

---

## 构建要求

| 组件 | 要求 | 说明 |
|---|---|---|
| Windows | **Win11 21H2（build 22000）或更高**（INF 声明值）；源码层面下限为 Win10 2004（build 19041） | ⚠️ 两者不一致，见下方「Windows 版本：INF 目标 OS 与源码下限的差异」 |
| Visual Studio | 2022（17.x） | 需装 “Desktop development with C++” |
| WDK | Windows 11 WDK（10.0.22621 或更新），需与 SDK 版本匹配 | **WDK 与 SDK 版本必须配对**，否则编译报 `WindowsSdkDir` 相关错 |
| Windows SDK | 与 WDK 同版本号（如 10.0.22621.x） | 提供 `signtool.exe` |
| 驱动签名 | 测试签名需 `bcdedit /set testsigning on`；生产需 EV + WHQL/Attestation | 见 `docs/Signing-Guide.md` |
| 服务端依赖 | libopus（必需）、libsoxr（推荐）、libspeexdsp（可选） | vcpkg: `opus:x64-windows-static` 等 |
| Android | Android Studio Iguana+ / AGP 8.1.4 / Gradle 8.2 / NDK 25+ / minSdk 26 | 见 `android/README.md` |
| Opus 源码（Android） | opus 1.4+ release，需手动放入 `android/app/src/main/cpp/opus/` | 仓库不内置，CMake 会给出明确报错 |
| Linux 侧测试（可选） | CMake 3.16+、Ninja、GCC/Clang、libopus-dev、pkg-config | 仅用于跑 `tests/` 与 `server` 的 Linux 冒烟测试 |

### Windows 版本：INF 目标 OS 与源码下限的差异

这里有两个**不相等**的下限，请按需取用（不要只看其中一个）：

| 口径 | 值 | 依据 |
|---|---|---|
| **INF 声明的目标 OS**（决定能不能装上） | **Win11 21H2（build 22000）或更高** | `driver/Source/Main/MicLinkVirtualMic.inx` 的 `[Manufacturer]` 段是 `%MfgName%=MICLINK,NT$ARCH$.10.0...22000`，模型只在 `[MICLINK.NT$ARCH$.10.0...22000]` 段里声明 |
| **源码实际需要的 OS**（决定能不能跑起来） | **Win10 2004（build 19041）或更高** | 驱动用了 `ExAllocatePool2`（`NewDelete.cpp:45`、`adapter.cpp:173`、`minwavertstream.cpp:289` 等）——该 API 自 Win10 2004 起提供；`EX_TIMER_HIGH_RESOLUTION`（`minwavertstream.cpp:262`）自 Win10 1803 起提供 |

也就是说：**在 Win10 19041~21999 上，驱动即便编译通过也装不上** ——
`pnputil /add-driver` 会报“找不到匹配的模型”，因为 INF 没有为这些 build 声明模型。

如果确实需要支持 Windows 10，需要同时改 INF 的两处（段名与 TargetOS 必须一起改，否则段对不上）：

```inf
[Manufacturer]
%MfgName%=MICLINK,NT$ARCH$.10.0...19041          ; 下限改为 Win10 2004

[MICLINK.NT$ARCH$.10.0...19041]                  ; 段名同步改
%MICLINK_VM.DeviceDesc%=MICLINK_VM, ROOT\MicLinkVirtualMic
```

改完后必须同步：

* `driver/verify/check_driver_consistency.py`（该脚本断言 `INF Manufacturer 段已改为 MICLINK`，
  里面写死了 `NT$ARCH$.10.0...22000`，改 INF 时这条断言会失败，需要一并更新）；
* 本文档与 `docs/BUILD.md`、`docs/Windows-Manual-Verification.md` 的版本说明。

> 该改动**未在 Windows 上验证过**（B 类）：INF 的 TargetOS 匹配行为、Win10 上 `ExAllocatePool2`
> 的实际可用性都只能在本机实测确认。

---

## 常见问题（FAQ）

| 问题 | 解答 |
|---|---|
| 需要装 VB-Cable 之类的虚拟声卡吗？ | **不需要**。驱动本身就是一枚 WaveRT 捕获端点。 |
| 有回声消除 / 降噪吗？ | Android 端用 `VOICE_COMMUNICATION` 音频源，复用**手机平台既有**的 AEC/NS；**本项目不自研 AEC/NS**（PRD 非目标）。 |
| 手机采样率和 PC 不一致会怎样？ | 采集率 != 解码率 **不致命**，服务端会重采样；但**端点率 != 解码率会致命**，服务端拒绝启动（驱动侧绝不做重采样）。 |
| 延迟大概多少？ | 由抖动缓冲主导，默认目标 50 ms（可调 `--jitter-min-ms/--jitter-max-ms`，硬上限 120 ms）+ 编码/网络开销。 |
| 支持立体声吗？ | 当前驱动端点为 **16-bit 单声道**（44100/48000）。 |
| 为什么设备管理器里是黄色感叹号 / 代码 52？ | 签名未通过：确认已 `bcdedit /set testsigning on` **并重启**，且 Secure Boot 已关闭（测试签名场景）。 |
| `\\.\MicLinkControl` 能打开但输入设备列表里没有麦克风？ | 典型是 dispatch 覆盖或捕获引脚未正确暴露，排查见 `docs/Windows-Manual-Verification.md` 第 8 节。 |
| 能直接分发给别人用吗？ | **当前不行**。必须完成生产签名（EV + WHQL/Attestation）后才行，见 `docs/Signing-Guide.md`。 |
| 手机为什么搜不到服务端？ | 发现走的是**主动扫描**：手机发广播 `MICLINK-DISCOVER 1`，服务端回单播 `MICLINK-HERE`。Android 端**未**申请 `WifiManager.MulticastLock`，因此**被动接收**服务端周期公告在部分 ROM 上收不到；请确保 `--discovery on` 已开启、手机与 PC 同网段且 UDP 5005 未被防火墙拦截。 |

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/VERIFICATION.md`](docs/VERIFICATION.md) | **哪些验证过、哪些没验证过**（A 类 Linux 已跑 / B 类需真机），含复跑命令 |
| [`docs/BUILD.md`](docs/BUILD.md) | WDK/VS 版本要求、打开解决方案、选 x64、产出 sys/inf/cat、编译错误排查 |
| [`docs/Windows-Manual-Verification.md`](docs/Windows-Manual-Verification.md) | 拿到后在 Windows 上逐条验证的手册（录音机 / OBS / Zoom） |
| [`docs/Signing-Guide.md`](docs/Signing-Guide.md) | 测试签名 vs 生产签名（EV + WHQL / Attestation）的区别、流程、成本量级 |
| [`server/build_windows.md`](server/build_windows.md) | 服务端 Windows 构建 |
| [`android/README.md`](android/README.md) | Android 端构建、opus 源码准备、协议一致性说明 |
| [`tools/README.md`](tools/README.md) | 安装/卸载/签名/验证脚本用法与自审清单 |

## 许可证

仓库许可证见 [`LICENSE`](LICENSE) 与 [`NOTICE.md`](NOTICE.md)。
内核驱动部分衍生自 Microsoft 的 Simple Audio Sample（WDK 示例），保留了其原始的 MIT 许可与声明；
`android/README.md` 提到的 libopus 也有其独立许可证（BSD-3-Clause 风格），分发时需一并保留。
