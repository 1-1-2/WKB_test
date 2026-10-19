# Womic Open — Android 采集端

手机麦克风 → **UDP/Opus** → Windows 服务端（`womicserver`）→ 内核虚拟麦克风驱动。
本目录是一个**可直接用 Android Studio 打开并构建**的 Gradle 工程。

> 与服务端、驱动共用的协议契约来自 `shared/core/include/womic/womic_protocol.h`（**唯一事实来源**）。
> Android 端不复制结构体，只在 `Packetizer.kt` 里按同一份偏移表做**大端**封包。

---

## 1. 目录结构

```
android/
├── settings.gradle.kts
├── build.gradle.kts                      根工程（只声明插件版本）
├── gradle.properties
├── gradlew / gradlew.bat                 官方 Gradle wrapper 启动脚本
├── gradle/wrapper/gradle-wrapper.jar     wrapper 引导 jar（随脚本一起提供）
├── gradle/wrapper/gradle-wrapper.properties   Gradle 8.2（all 发行包）
├── app/
│   ├── build.gradle.kts                  AGP + CMake externalNativeBuild
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml           RECORD_AUDIO / INTERNET / FOREGROUND_SERVICE(_MICROPHONE)
│       ├── cpp/
│       │   ├── CMakeLists.txt            编译 opus_jni.cpp 并链接 libopus
│       │   ├── opus_jni.cpp              JNI：create / destroy / encode / plc
│       │   └── opus_jni.h
│       ├── java/com/womic/open/
│       │   ├── MainActivity.kt           UI：IP/端口/采样率/帧长/码率/开始停止/电平条/状态
│       │   ├── MicCaptureService.kt      前台服务：AudioRecord → Opus → UDP
│       │   ├── OpusEncoder.kt            JNI 封装
│       │   ├── UdpSender.kt              DatagramSocket 发送（含 SocketException 处理）
│       │   ├── Packetizer.kt             32 字节大端头封包（对齐 womic_protocol.h）
│       │   ├── Discovery.kt              广播发现服务端（UDP 5005）
│       │   ├── LevelMeter.kt             RMS / peak 电平
│       │   ├── Prefs.kt                  SharedPreferences 持久化
│       │   └── Permissions.kt            运行时权限
│       └── res/                          layout / values / xml / drawable
└── README.md                             本文件
```

---

## 2. 准备 Opus 源码（**必做，否则 CMake 直接报错**）

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

**没有放源码时的表现**：`CMake` 会直接 `FATAL_ERROR` 并打印上面这段下载命令，不会生成一个“能编译但跑不起来”的库。
放错版本（`include/opus.h` 缺失）也会有明确报错。

> `CMakeLists.txt` 已强制 `OPUS_BUILD_STATIC_LIBRARY=ON`、`OPUS_DISABLE_INTRINSICS=ON`、
> `OPUS_BUILD_TESTING/PROGRAMS=OFF`：只要一个静态 libopus，关掉 SIMD 内联汇编换取跨 ABI 稳定性。
> 需要性能可自行把 `OPUS_DISABLE_INTRINSICS` 改回 `OFF`。

---

## 3. 构建与运行

| 项 | 版本 |
|---|---|
| Android Studio | Iguana (2023.2) 或更新 |
| AGP | 8.1.4 |
| Gradle | 8.2（wrapper 已指定） |
| Kotlin | 1.9.22 |
| compileSdk / targetSdk | 34 |
| minSdk | 26（Android 8.0） |
| NDK | 25.x 或更新（AGP 会自动下载） |
| CMake | 3.22.1 |

```bash
# 命令行构建（也可直接用 Android Studio 打开 android/ 目录）
cd android
./gradlew :app:assembleDebug      # debug apk
./gradlew :app:assembleRelease    # release apk（R8 已开启）
```

> **关于 wrapper 的来历与边界**：`gradlew` / `gradlew.bat` / `gradle-wrapper.jar`
> 是用本机 Gradle 的 `gradle wrapper --gradle-version 8.2 --distribution-type all`
> 生成的**官方脚本**（未手写）。首次执行会联网下载 Gradle 8.2 发行包。
> 本仓库的 Linux 沙箱**没有 Android SDK / NDK**，因此 `./gradlew` 在沙箱里
> **一次都没有被真正执行过**（无网络、无 SDK，构建必然失败）；
> 它属于 `docs/VERIFICATION.md` 的 B 类“待本机验证”项。

真机安装后：

1. 先启动 Windows 服务端：`womicserver.exe --port 5004 --rate 48000`；
2. 手机上打开 App → 填 PC 的局域网 IP（或点“发现服务端”，需服务端 `--discovery on`）；
3. 采样率 / 帧长 / 码率保持与服务端一致（**采样率不一致时服务端会拒绝启动**）；
4. 点“开始采集”，授予麦克风与通知权限。

---

## 4. 与服务端如何保证协议一致

### 4.1 UDP 包格式（32 字节头 + Opus payload，全部**大端**）

| 偏移 | 长度 | 字段 | Android 侧来源 |
|---|---|---|---|
| 0 | 4 | magic `0x574F4D50` ('WOMP') | `Packetizer.MAGIC` |
| 4 | 2 | version `1` | `Packetizer.VERSION` |
| 6 | 2 | header_size `32` | `Packetizer.HEADER_SIZE` |
| 8 | 4 | seq（每帧 +1） | 采集线程计数器 |
| 12 | 4 | timestamp_ms | `SystemClock.elapsedRealtime()` 相对值（单调） |
| 16 | 4 | sample_rate | **AudioRecord 实际采样率**（见下） |
| 20 | 2 | channels | 1 |
| 22 | 2 | frames_per_packet | `sampleRate * frameMs / 1000` |
| 24 | 2 | payload_bytes | `opus_encode()` 返回值 |
| 26 | 2 | flags（bit0 FEC / bit1 DTX） | 冗余重发包置 `FLAG_FEC` |
| 28 | 4 | reserved | 0 |

服务端 `womic_packet_decode()` 严格要求 `报文长度 == header_size + payload_bytes`，
因此 `Packetizer.pack()` 返回的 `ByteArray` **不带任何尾随字节**。
`Packetizer.selfCheck()` 用与 `protocol.c` 完全一致的规则做一次本地自洽校验（首包日志/排障用）。

### 4.2 采样率一致性（服务端会拒绝启动）

- 服务端 `shared/core/womic_sample_rate.h` 的规则：
  - **端点率 != 解码率 → 致命，拒绝启动**；
  - **采集率 != 解码率 → 非致命**，由服务端重采样消化（驱动侧绝不做重采样）。
- 部分手机在使用 `VOICE_COMMUNICATION` 时会把 48 kHz 强制降到 16 kHz。
  `MicCaptureService.openRecorder()` 会读取 `AudioRecord.getSampleRate()` 的**实际值**，
  按实际值重建编码器与帧长，并在状态栏提示；服务端会自动重采样，不会因此拒绝启动。
  UI 上采样率下拉的语义是“期望值”，实际值以状态栏为准。

### 4.3 端口

| 用途 | 默认端口 | 来源 |
|---|---|---|
| 音频 UDP | **5004** | `server/src/config.h: listen_port` |
| 发现 UDP | **5005** | `server/src/config.h: discovery_port` |

### 4.4 发现协议

```
手机 -> 广播 : "WOMIC-DISCOVER 1"
服务端 -> 单播: "WOMIC-HERE 1 <audio_port> <rate> <channels> <name>"
```
服务端需 `--discovery on`（默认关闭），否则不会有应答。
名字里的空格被服务端替换成 `_`；本端显示时还原成空格。

> ⚠️ **只有"主动发现"是可靠的**（缺陷 D-10，未改代码的现状）：
> 本 App 的工作方式是"发广播查询 + 收单播应答"（`Discovery.kt` 的 `scan()`），这条路不依赖
> 组播接收。但服务端 `discovery.cpp` 还会周期性向 `255.255.255.255` **主动广播公告**，
> 而本 App **未声明组播权限、也未获取 `WifiManager.MulticastLock`**，因此
> "被动接收服务端公告"在部分 ROM / Wi-Fi 省电策略下收不到，**不要依赖它**。
> 需要被动发现时的改法：`AndroidManifest.xml` 加
> `<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />`，
> 并在扫描期间 `acquire()/release()` 一个 `MulticastLock`。
> 该改动**未在真机上验证过**（B 类）。

---

## 5. 采集端的关键实现点

| 项 | 做法 | 原因 |
|---|---|---|
| 音频源 | `MediaRecorder.AudioSource.VOICE_COMMUNICATION` | 复用平台既有 AEC/NS 通路；**本项目不自研 AEC/NS**（PRD 非目标） |
| 读取方式 | 独立线程 + `AudioRecord.read()` 阻塞读 | 不做轮询，CPU 占用最低 |
| 缓冲 | `max(getMinBufferSize()*2, frameBytes*4)` | 降低欠载；`getMinBufferSize()<=0` 直接判为设备不支持 |
| 息屏采集 | 前台服务（`foregroundServiceType=microphone`）+ `PARTIAL_WAKE_LOCK` | 切后台/息屏不被系统回收 |
| 硬件热切换 | 注册 `AudioDeviceCallback`，置位后重建 `AudioRecord` | 插拔耳机/USB 声卡/蓝牙后能自愈 |
| 麦克风被抢占 | `read()` 返回 `ERROR_DEAD_OBJECT` → 退避重建，超过 5 次才报错 | 与系统相机/电话等抢占麦克风时能恢复 |
| 网络异常 | `SocketException` / `IOException` 全部捕获并计入 `failureStreak` | 一次 Wi-Fi 抖动不应中断整个采集 |
| 丢包对策 | 每 16 帧附一份上一帧 payload 的冗余包（`FLAG_FEC`） | 给服务端 PLC 多一份补救材料；**真正的 PLC 在服务端** |

前台服务的通知栏有“停止”按钮；`stopWithTask=true`，从最近任务划掉时会一起清理。

---

## 6. 常见问题

| 现象 | 排查 |
|---|---|
| Gradle 报 `未找到 Opus 源码` | 按第 2 节放入 opus 源码后重新 `Sync` |
| App 启动显示“native 库未加载” | `libwomic_opus.so` 没打进 APK：确认 ABI 在 `abiFilters` 内、CMake 无报错 |
| 点开始立刻报 `AudioRecord 初始化失败` | 未授予 `RECORD_AUDIO`，或麦克风被其他应用（电话/录音/会议）独占 |
| 服务端日志无包 | PC 防火墙放行 UDP 5004；确认手机填的是 PC 的**局域网** IP；确认手机与 PC 同网段 |
| 服务端拒绝启动 | 采样率链路致命错误：端点率与解码率不一致，改 `--rate` 或手机端采样率 |
| 发现不到服务端 | 服务端加 `--discovery on`；部分路由器隔离了广播，直接手填 IP |
| 有声音但断续 | 提高抖动缓冲：`--jitter-min-ms 60 --jitter-max-ms 120`；或换 5G Wi-Fi / 有线 |

---

## 7. 在 Linux 上能先跑的两个交叉校验

沙箱没有 Android SDK/NDK，但下面两项可以脱离 Android 环境跑，**都已在沙箱内真实通过**：

```bash
# 1) 封包字节布局：与 shared/core 权威实现逐字节比对（gcc + JDK）
cd android/verify/protocol_crosscheck && ./run.sh

# 2) JNI 接线：JNI 符号名/签名/错误码是否与 Kotlin 声明一致（g++ + JDK，链接假 opus）
cd android/verify/jni_smoketest && ./run.sh
```

| 校验 | 能证明 | **不能**证明 |
|---|---|---|
| `protocol_crosscheck` | 常量、偏移、字节序、长度自洽正确 | Kotlin 源码能编译；`Packetizer.kt` 与镜像是否同步 |
| `jni_smoketest` | JNI 函数名/签名与 Kotlin 匹配、错误码契约成立 | Opus 编码质量（链接的是**假 Opus**）；CMake 能编过；真机 ABI 兼容 |

细节与维护约定见 `android/verify/README.md`。

## 8. 验证状态（务必先看 `docs/VERIFICATION.md`）

本工程在本仓库的 Linux 沙箱内**没有编译过**（沙箱无 Android SDK/NDK/Kotlin 编译器）。
Kotlin 与 JNI 代码经过人工自审（import、生命周期、权限、前台服务通知渠道、协议偏移），
**属于“待真机构建验证”项**。首次联调建议先跑 `womicserver --dry-run` 并观察首包日志。
