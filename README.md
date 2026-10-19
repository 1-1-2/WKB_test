# MicLink Open —— WO Mic 协议开源兼容实现（Android 采集端 + Windows 客户端）

把 Android 手机的麦克风，通过 **WO Mic Wi-Fi 协议**推给 PC 端；PC 侧套用 WO Mic
官方已签名的 `womic.sys` 虚拟麦克风驱动，让手机麦克风成为 Windows 里一枚
**真实的录音设备**：Zoom / 腾讯会议 / OBS / 系统录音机都能直接选中它，
**不需要安装第三方虚拟声卡**（VB-Cable 之类），也**不需要开启测试签名模式**。

本仓库包含两端可独立取用的实现：

- **`android/` —— WO Mic 协议兼容采集端**（当前主力维护对象）：手机侧协议兼容实现
  （手机 = TCP server 8125 + UDP sender），与 PC 端任意 WO Mic 兼容客户端
  （官方 Windows Client，或本仓库的 `client/`）对接。
- **`client/` —— WO Mic 协议兼容 PC 客户端**（`womic.exe`）：Windows 用户态实现，
  协议引擎 + Fluent 风格自绘界面 + 驱动注音全套；随附驱动安装器
  `womic-drvinst.exe`，负责装 / 卸 / 查官方 `womic.sys`。

协议规格由官方 APK / PC 客户端逆向获得，全部结论有反编译证据支撑
（见 [`docs/protocol/womic-wifi-protocol-v1.md`](docs/protocol/womic-wifi-protocol-v1.md)，
附 35 个证据文件；驱动侧握手链取证见
[`docs/driver-whitepaper.md`](docs/driver-whitepaper.md)）。
两端**都不是**官方应用，与官方无关联，仅供协议研究与开发联调使用。

> **自研线归档说明**：本仓库早期还有一条自研内核驱动线（`driver/` 虚拟麦克风驱动 +
> `server/` 用户态服务端 + `shared/core` 共用契约 + `tests/` 单元测试），实测后已弃用
> （决策记录见 [`docs/adr/0001-driver-route.md`](docs/adr/0001-driver-route.md)）。
> 该线已整体归档至本仓库的 **`self-driver` 分支**（孤儿快照提交，附归档说明），
> `main` 分支不再包含这些目录；本文所有内容仅描述第三方兼容线。

## 链路

```
WO Mic 协议兼容链路（手机 = TCP server + UDP sender，PC = 客户端）
+--------------------+   TCP 8125（控制，PC 发起连接）   +---------------------------+
|  Android 手机       | <==============================> |  womic.exe（本仓库实现）    |
|  = TCP server 8125 |   101 协商 -> 102 参数 -> 103 推流  |  = TCP client              |
|  = UDP sender      | <==============================> |  = UDP receiver            |
|  AudioRecord->Opus |   UDP 单向（Opus，格式 C 头 11B）  |  -> Opus 解码 -> 电平/统计   |
+--------------------+                                    |  -> DeviceIoControl 灌音   |
                                                          +------------+--------------+
                                                                       | 48kHz/16bit/mono
                                                          +------------v--------------+
                                                          |  womic.sys（官方已签名）   |
                                                          |  WaveRT 虚拟麦克风端点     |
                                                          +------------+--------------+
                                                                       | Windows 音频栈
                                                   录音机 / OBS / Zoom / 腾讯会议（直接选中即可用）
```

- **控制通道**：手机在 `0.0.0.0:8125`（可改）监听，PC 连接后按
  `[type:u8][len:u32 大端][payload]` 帧格式交互：101 版本协商 → 102 参数协商
  （采样率索引 / PC 媒体端口）→ 103 开始推流 / 104 停止 / 105 心跳。
- **媒体通道**：收到 103 后手机以本地临时端口向 PC 媒体端口单向发送 Opus 包
  （格式 C 头，11 字节），PC 不回 UDP。
- **心跳看门狗**：PC 约 1s 发一次 105；手机侧 5s 未收到即强制断链（镜像官方行为）。
- **注音**：`womic.exe` 经 `\\.\WOMic` 走官方驱动握手链，每 20 ms 灌入 1920 B。

完整消息表、错误码（10/12/13）、104 停止应答怪癖（官方实际写 type 102）、
UDP 三种头格式（A/B/C）见协议规格文档，本文不复述。

---

## ⚠️ 先读这段：验证状态

本仓库的开发环境是 **Linux 沙箱**。下面这些**已经在沙箱 / CI 里真实跑过**：

* Android 端协议实现的**字节级一致性**（`android/core` 纯 JVM 模块 23 个单元测试：
  帧编解码 / UDP 三种头格式 / 会话状态机 / 5s 心跳看门狗）；
* Android 端**完整构建**（AGP 9.1.0 + Gradle 9.3.1 + NDK 28.2.13676358 + CMake 3.22.1，
  真实 libopus 经 CMake/NDK 编译并打进 APK）；
* Android 端 JNI 接线冒烟测试（链接假 Opus，只验符号名/签名/错误码：
  `android/verify/jni_smoketest`）；
* Windows 客户端**Linux 交叉编译 + wine 运行**（两套主题、连接/未连接态、
  日志时间线均正常；产物导入表全为系统自带 DLL，零额外运行库）；
* Windows 客户端**真机端到端 CI**（GitHub Actions `windows-e2e`：驱动安装器自助安装
  官方驱动成功，SYSTEM 计划任务跑 `e2e_probe` 拿到 `RESULT=PASS`——waveIn 录到
  288000 B / 1 kHz 正弦；GUI 冒烟真机通过，详见
  [`docs/windows-e2e-report.md`](docs/windows-e2e-report.md)）；
* **双机联调 CI**（`tailscale-dual`：真 `womic.exe` ↔ 真 Android app 跨 Tailscale
  全链路 + 440 Hz 音频取证，四作业全绿）；
* 联调脚本 `android/verify/integration/pc_client.py` 的自测 9 项 + 对 mock 服务端的
  15 项闭环干跑（含 5s 看门狗断链与重连）。

下面这些**从未跑过**，需要你在真机上完成：

* Android app 与**官方 WO Mic Windows Client** 的互联互通（人工清单见
  [`docs/android-interop-checklist.md`](docs/android-interop-checklist.md)）；
* 长时间（小时级）连续推流的稳定性；
* 非 KVM 模拟器环境（真实手机设备）下的采集质量。

**本项目交付的是"代码 + 测试签名可安装"的状态；驱动为官方已签名二进制，
未经任何修改，再分发合规状态见 [`NOTICE.md`](NOTICE.md)。**

---

## 仓库结构

```
miclink-open/
├── android/               Android 采集端（WO Mic 协议兼容；Compose UI + libopus JNI）
│   ├── core/              纯 JVM 协议模块（帧/会话/包化 + 23 个字节级测试）
│   ├── app/               应用层（前台服务采集 + Compose UI + 采集选项）
│   └── verify/            Linux 上可跑的交叉校验（JNI 接线冒烟 + 联调脚本）
├── client/                Windows PC 客户端（womic.exe + womic-drvinst.exe）
│   ├── src/               应用/安装器共用源码（协议引擎、注音、自绘 UI、驱动安装）
│   ├── res/               资源（图标/manifest/rc；驱动包为生成物，不入库）
│   └── tools/             构建辅助（pack_driver / check_pe / fake_server / e2e_probe）
├── docs/                  协议规格、构建说明、验证报告、ADR、CI 通道文档
│   └── protocol/          WO Mic WiFi 协议规格 v1 + 35 个反编译证据文件
├── tools/                 api_push.py（经 GitHub Git Data API 推送，见 tools/README.md）
└── .github/               CI：android.yml / windows-e2e.yml / tailscale-dual.yml + 脚本
```

自研线（`driver/` `server/` `shared/` `tests/`）不在此列，已归档至 `self-driver` 分支。

---

## 快速开始

### 路线 1：Android 采集端 → 任意 WO Mic 兼容客户端

| 步骤 | 命令 / 动作 |
|---|---|
| 1. 准备 opus 源码 | 按 [`android/README.md`](android/README.md) 把 opus 1.4+ release 放到 `android/app/src/main/cpp/opus/` |
| 2. 构建 | `cd android && ./gradlew :app:assembleDebug` |
| 3. 安装运行 | 装到手机，点"启动服务"，状态卡显示"等待 PC 连接"与**手机地址** |
| 4. PC 端连接 | 官方 WO Mic Client 或本仓库 `client/` 构建的 `womic.exe`，填手机地址（`IP:控制端口`，默认 8125） |
| 5. 验证 | PC 端选虚拟麦克风的应用（录音机 / OBS / Zoom）直接采集 |

### 路线 2：Windows 客户端（本仓库 PC 端）

| 步骤 | 命令 / 动作 |
|---|---|
| 1. 安装驱动（只需一次） | `womic-drvinst.exe /install`（交互菜单：1 装 / 2 卸 / 3 查 / 0 退）；也可 `--driver-pack <官方驱动目录> /install` 用外置驱动 |
| 2. 启动应用 | `womic.exe`（GUI）或 `womic.exe <手机IP> [端口] [--auto]`（CLI） |
| 3. 连接推流 | 应用内填手机地址连接；状态卡显示帧数 / 码率 / 电平 |
| 4. 选设备 | 系统输入设备出现 **WOMic** 虚拟麦克风，录音机 / OBS / Zoom 直接选中 |

构建 PC 客户端（产物 `womic.exe` 291.5 KiB + `womic-drvinst.exe` ~209 KiB，
只依赖系统自带 DLL，零额外运行库）：

```bash
# Linux 交叉编译（推荐，产物一致；也可用 Windows 原生 build.bat）
sudo apt install gcc-mingw-w64-x86-64
cd client && ./build.sh          # all：两个 exe 都编；app / drvinst / test 可单编
```

干净检出**不含**第三方驱动二进制（版权归 Wolicheng 所有，再分发有法律风险，架构评审 R8）：
`build.sh` 找不到官方驱动目录时自动生成合法空包兜底，安装器需
`/driver-pack <dir>` 外置驱动；要内置驱动需自备官方驱动目录后跑
`tools/pack_driver.py` 重新打包。详见
[`client/README.md`](client/README.md)「构建与驱动包」与
[`docs/BUILD.md`](docs/BUILD.md)。

### 路线 3：CI 验证（无需本地环境）

| Workflow | 内容 | 触发 |
|---|---|---|
| `android.yml` | 模拟器 + 真 APK 协议闭环联调（`integration-test`） | push / 手动 |
| `windows-e2e.yml` | Windows 真机：构建 + 驱动安装 + 注音 + 录音取证 | push / 手动 |
| `tailscale-dual.yml` | 双机跨网：真 womic.exe ↔ 真 app 全链路 + 440Hz 取证 | push / 手动 |

---

## 常见问题（FAQ）

| 问题 | 解答 |
|---|---|
| 需要装 VB-Cable 之类的虚拟声卡吗？ | **不需要**。官方 `womic.sys` 本身就是一枚 WaveRT 捕获端点。 |
| 需要开测试签名模式吗？ | **不需要**。复用官方 WHCP 已签名驱动；自研驱动路线已弃用并归档（ADR 0001）。 |
| 有回声消除 / 降噪吗？ | Android 采集端用 `DEFAULT` 音频源（对齐官方 app 行为），**不自研 AEC/NS**；系统降噪开关走 `SystemAudioEffects` 委托系统实现。 |
| 手机采样率和 PC 不一致会怎样？ | 102 协商帧按采样率索引对齐；驱动端点固定 48 kHz/16 bit/单声道。 |
| 延迟大概多少？ | 20 ms 帧 + 网络抖动；PC 侧有抖动缓冲与丢包统计实时可见。 |
| 支持立体声吗？ | 当前驱动端点为 **16-bit 单声道**（48000 Hz）。 |
| WO Mic 链路：PC 连不上手机？ | 手机点"启动服务"后状态应为"**等待 PC 连接**"；PC 端要填状态卡里的"**手机地址**"（`IP:控制端口`，默认 8125）；确认手机与 PC 同网段、TCP 8125 未被防火墙拦截。 |
| WO Mic 链路：连上约 5 秒就断？ | 触发了手机侧**心跳看门狗**：PC 端需按约 1s 周期发 105 心跳帧，5s 收不到即强制断链（镜像官方行为）。 |
| WO Mic 链路：PC 端提示参数错误？ | 102 协商帧的 codec 必须为 2（Opus）、rateIdx 必须为 0/1/2，否则手机回 status 12/13；PC 媒体端口为 u32 大端。 |
| 驱动装不上 / 设备管理器黄色感叹号？ | 用 `womic-drvinst.exe /status` 查状态；确认是 2022 版（2.1.0.0）三件套（2017 版握手不兼容，不打包）。 |
| 能直接分发给别人用吗？ | 可以分发本仓库的两个 exe（自有代码）；**内置驱动的安装器**再分发合规状态未经法律确认，稳妥做法是引导用户从官方渠道获取驱动，用 `/driver-pack` 外置安装。 |

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/protocol/womic-wifi-protocol-v1.md`](docs/protocol/womic-wifi-protocol-v1.md) | WO Mic WiFi 协议规格 v1（官方 APK + PC 客户端双端逆向实证，附 35 个证据文件） |
| [`docs/driver-whitepaper.md`](docs/driver-whitepaper.md) | 驱动灌音协议白皮书：`\\.\WOMic` 握手链、帧规格、防逆向行为与驱动行为矩阵 |
| [`docs/BUILD.md`](docs/BUILD.md) | 构建说明：Android 采集端 + Windows 客户端的环境要求与构建步骤 |
| [`client/README.md`](client/README.md) | PC 客户端功能、体积预算、驱动包构建、已验证与限制 |
| [`android/README.md`](android/README.md) | Android 采集端构建、opus 源码准备、协议实现要点 |
| [`docs/windows-e2e-report.md`](docs/windows-e2e-report.md) | Windows 客户端真机端到端 CI 验证报告（三轮 run、根因与修复） |
| [`docs/android-interop-checklist.md`](docs/android-interop-checklist.md) | 与官方 WO Mic App / Client 互操作人工清单 |
| [`docs/VERIFICATION.md`](docs/VERIFICATION.md) | 验证状态总账（🗄 历史资产：自研线时期，见 docs/README.md 说明） |
| [`docs/devops/windows-interactive-debug.md`](docs/devops/windows-interactive-debug.md) | Windows Runner 交互式联调 CI 工具选型与作业分工 |
| [`.github/scripts/README.md`](.github/scripts/README.md) | CI shell 脚本说明、单元测试、外部看门狗用法 |
| [`docs/tailscale-channel.md`](docs/tailscale-channel.md) | CI Runner 与开发者机器的组网通道选型 |
| [`docs/github-api-push.md`](docs/github-api-push.md) | 受限网络下向 GitHub 推送的通道考古（Git Data API 四步法） |

---

## 许可证

仓库许可证见 [`LICENSE`](LICENSE) 与 [`NOTICE.md`](NOTICE.md)。
复用部分：官方 `womic.sys` 驱动二进制版权归 Wolicheng（WO Mic）所有，未经修改；
`android/` 构建需要自行下载的 libopus 有其独立许可证（BSD-3-Clause 风格），
分发时需一并保留。
