# MicLink Open — Android 采集端（WO Mic 协议兼容实现）

手机麦克风 → **TCP 控制 + UDP/Opus 单向推流** → PC 端 WO Mic 兼容客户端
（官方 Windows Client，或任何实现同一协议的第三方 PC 端）。
本目录是一个**可直接用 Android Studio 打开并构建**的 Gradle 工程。

> **定位说明**：本目录是一份面向协议研究与个人自测的 WO Mic 手机端**协议兼容实现**——
> 协议规格由官方 APK / PC 客户端逆向获得（见
> [`docs/protocol/womic-wifi-protocol-v1.md`](../docs/protocol/womic-wifi-protocol-v1.md)，
> 附 35 个反编译证据文件）。本实现**不是**官方应用，与官方无关联；
> 仅用于开发联调与协议学习，请勿用于任何商业用途。

---

## 1. 角色与拓扑

```
┌────────────────────┐   TCP 8125（控制，PC 发起连接）  ┌────────────────────┐
│  PC 端客户端        │ ◄──────────────────────────────► │  手机（本 app）     │
│  = TCP client      │   手机 = TCP server               │  = TCP server      │
│  = UDP receiver    │ ◄──────────────────────────────► │  = UDP sender      │
└────────────────────┘   UDP（Opus 单向，手机→PC）       └────────────────────┘
```

- **控制通道**：手机在 `0.0.0.0:8125`（可改）监听，PC 连接后按
  `[type:u8][len:u32 大端][payload]` 帧格式交互：101 版本协商 → 102 参数协商
  （采样率索引 / PC 媒体端口）→ 103 开始推流 / 104 停止 / 105 心跳。
- **媒体通道**：收到 103 后手机以**本地临时端口** `connect(PC 地址, PC 媒体端口)`，
  单向发送 Opus 包（格式 C 头，11 字节），**PC 不回 UDP**。
- **心跳看门狗**：PC 约 1s 发一次 105（无应答）；手机侧 5s 未收到即强制断链
  （镜像官方 `V1/k.java` 行为）。

完整消息表、错误码（10/12/13）、104 停止应答怪癖（官方实际写 type 102）、
UDP 三种头格式（A/B/C）见协议规格文档，本文不复述。

---

## 2. 目录结构

```
android/
├── settings.gradle.kts                  插件/依赖仓库（腾讯镜像置首）
├── build.gradle.kts                     根工程（只声明插件版本）
├── gradle.properties
├── gradlew / gradlew.bat                Gradle wrapper（Gradle 9.3.1）
├── gradle/wrapper/gradle-wrapper.properties
├── gradle/libs.versions.toml            版本目录（AGP/Kotlin/Compose 唯一来源）
├── core/                                纯 JVM 协议模块（无 Android 依赖）
│   ├── build.gradle.kts
│   └── src/
│       ├── main/java/dev/womic/wifistream/protocol/
│       │   ├── WomicProtocol.kt         常量 / 错误码 / 官方 HELLO 负载
│       │   ├── TcpFrameCodec.kt         帧编解码 + 应答构造（含 104→102 怪癖）
│       │   ├── UdpPacketizer.kt         UDP 格式 A/B/C 包化
│       │   └── WomicSession.kt          会话状态机 + 5s 心跳看门狗
│       └── test/java/...                23 个字节级一致性测试（:core:test）
├── app/
│   ├── build.gradle.kts                 AGP + Compose + CMake externalNativeBuild
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml          RECORD_AUDIO / INTERNET / FOREGROUND_SERVICE(_MICROPHONE)
│       ├── cpp/
│       │   ├── CMakeLists.txt           编译 opus_jni.cpp 并链接 libopus
│       │   ├── opus_jni.cpp             JNI：create / destroy / encode / plc
│       │   └── opus_jni.h
│       ├── java/com/miclink/open/
│       │   ├── MainActivity.kt          Compose UI（平铺设置 + 状态卡 + 电平条）
│       │   ├── MainViewModel.kt         UI 状态（mutableStateOf，无协程依赖）
│       │   ├── MicCaptureService.kt     前台服务：TCP server + 会话 + 采集推流
│       │   ├── OpusEncoder.kt           JNI 封装
│       │   ├── UdpSender.kt             DatagramSocket 发送（connect 到 PC 媒体端口）
│       │   ├── Packetizer.kt            :core 适配层（UDP 格式 C + selfCheck）
│       │   ├── Prefs.kt                 SharedPreferences 持久化
│       │   ├── Permissions.kt           运行时权限
│       │   └── LevelMeter.kt            RMS / peak 电平
│       └── res/                         values / drawable（无 layout，纯 Compose）
└── verify/                              Linux 上可跑的交叉校验
```

**协议唯一事实来源**：`docs/protocol/womic-wifi-protocol-v1.md` + `docs/protocol/evidence/`。
`:core` 模块是该规格的 Kotlin 实现，`app` 只做 Android 装配（AudioRecord / 前台服务 / UI），
不重复实现协议逻辑。

---

## 3. 准备 Opus 源码（**必做，否则 CMake 直接报错**）

本仓库**不内置** libopus 源码（体积大，且需保留其独立许可证文件）。请先手动获取：

```bash
# 版本要求：1.4 及以上（1.4+ 自带 CMakeLists.txt，构建最省事）
curl -L -o opus-1.5.2.tar.gz https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
tar -xzf opus-1.5.2.tar.gz

# 放到 CMake 期望的位置
mkdir -p android/app/src/main/cpp/opus
cp -R opus-1.5.2/* android/app/src/main/cpp/opus/
```

期望结构（缺一不可）：

```
app/src/main/cpp/opus/
├── CMakeLists.txt          （推荐；有它就走 opus 自带构建）
├── include/opus.h
├── include/opus_*.h
├── src/*.c
├── celt/*.c
├── silk/*.c
├── silk/float/*.c
└── silk/fixed/*.c
```

**没有放源码时的表现**：`CMake` 会直接 `FATAL_ERROR` 并打印上面这段下载命令，不会生成一个
"能编译但跑不起来"的库。放错版本（`include/opus.h` 缺失）也会有明确报错。

> `CMakeLists.txt` 已强制 `OPUS_BUILD_STATIC_LIBRARY=ON`、`OPUS_DISABLE_INTRINSICS=ON`、
> `OPUS_BUILD_TESTING/PROGRAMS=OFF`：只要一个静态 libopus，关掉 SIMD 内联汇编换取跨 ABI 稳定性。
> 需要性能可自行把 `OPUS_DISABLE_INTRINSICS` 改回 `OFF`。

---

## 4. 构建与运行

### 4.1 工具链版本

| 项 | 版本 | 说明 |
|---|---|---|
| JDK | **17** | AGP 9.x 最低要求；本地开发 17/21 均可 |
| AGP | **9.1.0** | 要求 Gradle ≥ 9.3.1、compileSdk ≥ 37（Compose 1.12 要求） |
| Gradle | **9.3.1** | wrapper 已指定（`gradle-9.3.1-bin.zip`） |
| Kotlin | 2.2.21 | AGP 9 起 Kotlin 支持内置（运行时依赖 KGP），**无需**再应用 `org.jetbrains.kotlin.android` 插件 |
| compileSdk / targetSdk | **37** | 注意 API 37 平台的次版本命名是 `platforms;android-37.0` |
| minSdk | 26（Android 8.0） | AudioRecord.Builder / 前台服务通知渠道 / Opus 均需要较新的 API |
| NDK / CMake | 27.x / 3.22.1 | 随 Android Studio 安装即可 |

> **AGP 9.x 迁移要点**（本工程已处理）：
> ① 移除 `org.jetbrains.kotlin.android` 插件（AGP 9 内置 Kotlin，显式应用会直接报错）；
> ② BuildConfig 生成改为 per-module 开关（`buildFeatures { buildConfig = true }`），
> 全局 `android.defaults.buildfeatures.buildconfig` 已移除；
> ③ `android.nonFinalResIds` 已废弃（默认 true），不再设置；
> ④ Kotlin 编译选项用顶层 `kotlin { compilerOptions { jvmTarget } }`。

### 4.2 依赖仓库镜像

`s settings.gradle.kts` 将**腾讯云镜像**置首位（国内速度：234KB/s vs Central 36KB/s）：

- 依赖与 AGP：`https://mirrors.tencent.com/nexus/repository/maven-public/`
- Gradle 发行版：`https://mirrors.cloud.tencent.com/gradle/`（wrapper 仍用官方源，CI 通用）

保留 `google()` / `mavenCentral()` / `gradlePluginPortal()` 兜底。

### 4.3 构建

```bash
cd android
./gradlew :core:test                 # 协议字节级一致性（23 个用例，纯 JVM，无需 SDK）
./gradlew :app:assembleDebug         # debug apk（需先按第 3 节放 opus 源码）
./gradlew :app:assembleRelease       # release apk（R8 已开启）
```

### 4.4 真机联调

1. 手机与 PC 接入**同一局域网**；
2. 手机上打开 App → 点"启动服务"（授予麦克风 / 通知权限）→ 状态变为"**等待 PC 连接**"；
3. 看状态卡里的"手机地址"（形如 `192.168.x.x:8125`），把它填进 PC 端客户端的连接地址；
4. PC 端点"开始推流"：手机自动开始采集并以 Opus 经 UDP 发往 PC 协商的媒体端口；
5. PC 端点"停止"或断开连接：手机自动停止采集（104 或 TCP 断开均触发）。

**调试音模式**：无麦克风权限 / 无麦克风设备时，打开设置里的"调试音"开关，
app 发送 440Hz 正弦波而不采集麦克风，用于验证链路连通性。

### 4.5 自动化联调（无需真机 / 无需 PC 客户端）

仓库自带一个**协议对端脚本**，扮演官方 Windows 客户端 6.3 的 PC 角色，把整条链路
（TCP 控制 + UDP 媒体）跑成 15 条断言：

```bash
python3 android/verify/integration/pc_client.py --self-test   # 不连设备，先自检脚本本身
python3 android/verify/integration/pc_client.py --hold-seconds 12
```

CI 上由 `.github/workflows/android.yml` 的 `integration-test` 作业自动执行：
ubuntu-latest + KVM 模拟器（android-34 x86_64）→ 装 APK → `am start-service` 以
**调试音模式**启动服务 → 跑脚本 → 失败时上传 logcat / dumpsys / emulator 日志。
断言清单与拓扑说明见 [`verify/integration/README.md`](verify/integration/README.md)。

---

## 5. 关键实现点

| 项 | 做法 | 原因 |
|---|---|---|
| 协议核心 | 独立 `:core` 纯 JVM 模块 + 字节级单元测试 | 协议逻辑可在 Linux CI 上验证，不依赖 Android SDK |
| 角色 | 手机 = TCP server（8125）+ UDP sender | 镜像官方拓扑（`evidence/e2/j.java`）；PC 为连接发起方 |
| 会话状态机 | `WomicSession` + `FrameDecoder` 增量解码 | 与官方 `V1/n` 状态字段、`V1/l` 主循环一一对应 |
| 心跳看门狗 | 每 `HEARTBEAT_TIMEOUT_MS`（5s）检查，超时关 socket | 镜像官方 `V1/k.java`：标志位初值 false，**首个周期无心跳即断** |
| 104 应答 | 回复 type 102 帧（`66 00000001 00`） | 官方 app 无参构造怪癖（`d2/e` f12092q=0），必须复刻 |
| 音频源 | `MediaRecorder.AudioSource.DEFAULT` | 对齐官方 `audioSource=0`（不用 VOICE_COMMUNICATION，避免平台 AEC 改变链路特性） |
| 编码参数 | Opus 20ms 帧、64kbps、48000Hz（rateIdx=2） | 官方客户端默认值（WOMicClient.exe 反汇编实证） |
| UDP 封包 | 格式 C（11 字节头：`04 00` magic + len + seq + ts + volume） | PC 客户端 6.3 使用 variant=4；`:core` 同时实现 A/B 两种旧格式 |
| 音量字段 | UI 可调 0-255，默认 5（单位增益） | 写入 UDP 头音量字段（官方 `f.java` 默认值） |
| 息屏采集 | 前台服务（`foregroundServiceType=microphone`） | 切后台/息屏不被系统回收；启动时按类型可用性降级 |
| 采集线程 | 独立线程阻塞读 `AudioRecord.read()` | 不做轮询，CPU 占用最低 |
| 网络异常 | `SocketException` / `IOException` 全部捕获并计入 `failureStreak` | 一次 Wi-Fi 抖动不应中断整个采集 |

UI 为 **Jetpack Compose 平铺风格**（无嵌套菜单）：顶部状态卡（状态 / 手机地址 / 收发统计 /
RMS+Peak 双电平条）→ 主操作按钮 → 设置区（控制端口 / 推流音量 / 调试音）→ 使用说明卡。
支持 S+ 动态取色（深/浅色分开）。

---

## 6. 常见问题

| 现象 | 排查 |
|---|---|
| Gradle 报 `未找到 Opus 源码` | 按第 3 节放入 opus 源码后重新 Sync |
| App 启动显示"native 库未加载" | `libmiclink_opus.so` 没打进 APK：确认 ABI 在 `abiFilters` 内、CMake 无报错 |
| 状态一直"等待 PC 连接" | 确认手机与 PC 同网段；PC 端填的是状态卡里的"手机地址"；防火墙未拦截 TCP 8125 |
| 连上就断（约 5 秒） | 心跳看门狗触发：PC 端需按约 1s 周期发 105；自写 PC 端最容易漏掉心跳 |
| PC 端提示参数错误（status 12/13） | 102 报文的 codec/rateIdx 非法：codec 必须为 2（Opus），rateIdx 必须为 0/1/2 |
| 有声音但断续 | 换 5G Wi-Fi / 有线；检查 PC 端媒体端口 UDP 是否被防火墙拦截（手机→PC 单向） |
| 电平条不动但 PC 有数据 | 处于"调试音"模式；关闭设置里的调试音开关即可恢复麦克风采集 |

---

## 7. 可脱离 Android 环境运行的验证

沙箱没有 Android SDK/NDK 时，下面几项仍可运行，**均已在 Linux 沙箱真实通过**：

```bash
# 1) 协议字节级一致性：帧编解码 / UDP 三格式 / 会话状态机 / 看门狗（23 个用例）
cd android && ./gradlew :core:test --configure-on-demand

# 2) JNI 接线：JNI 符号名/签名/错误码是否与 Kotlin 声明一致（g++ + JDK，链接假 opus）
cd android/verify/jni_smoketest && ./run.sh

# 3) 联调脚本自测：格式 C 解析 / 流式重分帧 / seq·ts·音量断言（9 项，不需要设备）
cd android/verify/integration && python3 pc_client.py --self-test
```

| 校验 | 能证明 | **不能**证明 |
|---|---|---|
| `:core:test` | 帧字节序、长度自洽、消息分派、看门狗语义与官方证据一致 | Kotlin 源码能编译；真机 ABI 兼容 |
| `jni_smoketest` | JNI 函数名/签名与 Kotlin 匹配、错误码契约成立 | Opus 编码质量（链接的是**假 Opus**）；CMake 能编过 |
| `pc_client.py --self-test` | 脚本自身的解析/断言/时序逻辑正确 | 真实 app 的行为（需 `integration-test` 作业或真机） |

细节与维护约定见 `android/verify/README.md`；联调脚本见
`android/verify/integration/README.md`。

## 8. 验证状态（务必先看 `docs/VERIFICATION.md`）

Kotlin 与 JNI 代码经过 `:core:test` 字节级验证 + 人工自审（import、生命周期、权限、
前台服务通知渠道、协议偏移）。**APK 组装已在 Linux 沙箱真实跑通**
（AGP 9.1.0 + Gradle 9.3.1 + NDK 28.2.13676358 + CMake 3.22.1 + opus 1.5.2，
产物含 `lib/x86_64/libmiclink_opus.so`），协议闭环由 CI 的 `integration-test`
作业在模拟器上验证。**模拟器闭环已全绿**：C01–C15 共 15 项断言全部通过
（run [39523580260](https://github.com/1-1-2/WKB_test/actions/runs/39523580260)，
2026-10-21，含 826 包 seq 连续、心跳跨 5s 看门狗、stop 的 type-102 怪癖、
看门狗断链、断链重连完整会话）。

**仍未验证**：真实手机（arm64）上的 `AudioRecord` 采集质量、前台服务在 ROM 上的
保活表现、以及与本项目 `pc_client.py` 之外的真实官方客户端（WOMicClient.exe 6.3）
对接 —— 后者需按
[`docs/Windows-Manual-Verification.md`](../docs/Windows-Manual-Verification.md) 在
Windows 真机确认。
