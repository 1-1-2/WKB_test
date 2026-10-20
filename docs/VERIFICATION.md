# 验证状态（如实记录）

**最重要的一句话**：

> 本仓库的开发环境是 **Linux 沙箱**。
> **没有编译过 WDK 驱动、没有安装过 Windows 驱动、没有运行过 Windows 音频栈、没有跑过录音机 / OBS / Zoom。**
> 因此“虚拟麦克风出现在 Windows 输入设备列表、并能被 Zoom 采集到”这一**最终结论尚未被验证**，
> 需要你在 Windows 真机上按 [`docs/Windows-Manual-Verification.md`](Windows-Manual-Verification.md) 逐条执行。

本文分两类：

* **A 类**：已在 Linux 沙箱内**真实编译并运行**验证（有命令、有结果、可复跑）；
* **B 类**：**必须**在 Windows + WDK 环境执行（本沙箱做不了，一行都没跑过）。

---

## A 类：已在 Linux 沙箱内真实运行验证

复跑环境：Linux 容器，GCC/Clang + CMake + Ninja，libopus 已安装。

### A1. `shared/core` 单元测试（7 个用例，全部通过）

命令：

```bash
cd /workspace/miclink-open/tests && ./run_tests.sh
```

实测结果（`--no-clean` 增量复跑，2024 年本次会话内执行）：

| # | 用例 | 命令 | 耗时 | 结果 |
|---|---|---|---|---|
| 1 | `test_ring_buffer` | `ctest -R test_ring_buffer` | 0.02 s | **Passed** |
| 2 | `test_jitter_buffer` | `ctest -R test_jitter_buffer` | 0.13 s | **Passed** |
| 3 | `test_protocol` | `ctest -R test_protocol` | 0.00 s | **Passed** |
| 4 | `test_opus_roundtrip` | `ctest -R test_opus_roundtrip` | 1.51 s | **Passed** |
| 5 | `test_dsp` | `ctest -R test_dsp` | 0.00 s | **Passed** |
| 6 | `test_sample_rate` | `ctest -R test_sample_rate` | 0.00 s | **Passed** |
| 7 | `fuzz_protocol` | `ctest -R fuzz_protocol` | 1.27 s | **Passed** |

汇总：`100% tests passed, 0 tests failed out of 7`（Total Test time ≈ 2.94 s）。

其中包含的关键压力项：

| 压力项 | 内容 | 结果 |
|---|---|---|
| 无锁 SPSC 环形缓冲 | 1000 万帧并发读写压力测试 | **丢样本 0** |
| 协议 fuzz | 15 万轮随机畸形输入（随机长度/内容/篡改长度），在 **ASan + UBSan** 下 | 无崩溃、无越界读，全部返回负错误码 |
| Opus 编解码往返 | 16 组参数组合，比较解码后 SNR | 全部通过 |

### A2. `server` Linux 真实构建 + 端到端测试

命令：

```bash
cmake -S /workspace/miclink-open/server -B /workspace/miclink-open/server/build -DCMAKE_BUILD_TYPE=Release
cmake --build /workspace/miclink-open/server/build
ctest --test-dir /workspace/miclink-open/server/build --output-on-failure
```

实测结果：

| 项目 | 结果 |
|---|---|
| `miclinkserver` 构建 | 成功（产物 `server/build/miclinkserver`） |
| `test_pipeline` | **Passed**（16.22 s） |
| 汇总 | `100% tests passed, 0 tests failed out of 1` |
| 断言数 | **64 项断言，0 项失败** |

`test_pipeline` 覆盖的场景（节选，均为端到端）：

| 场景 | 内容 | 结果 |
|---|---|---|
| 5% 丢包下的 PLC | 模拟丢包后用 Opus PLC 隐藏 | 通过 |
| 16 kHz → 48 kHz 重采样 | 采集率与端点率不同，由服务端重采样 | 通过 |
| 发现服务往返 | 发 `MICLINK-DISCOVER 1`（16 字节），收到 `MICLINK-HERE 1 <port> 48000 1 MicLink_Virtual_Microphone` | 通过 |
| 会话启动 / 注入 / 停止复位 | Fake 设备后端全流程 | 通过 |
| **E 带内 FEC（合规语义）** | 冗余包**复用**被保护帧的 seq。断言：产出帧数 == 真实帧数（96000 == 96000，容差 0）、`frames_decoded == 100`（冗余包一个都没被当成新帧解码）、逐帧频率校验 97 帧全对 | 通过 |
| **F 带内 FEC（非合规语义）** | 冗余包**占用新 seq**（修复前 Android 的行为）。实测 106 个 seq 槽 → 抖动缓冲（默认 7 槽）**驱逐 7 帧**、只产出 99 帧。用于锁定"服务端无法弥补发送端膨胀"这一能力边界 | 通过 |

#### D-01 回归记录（FEC 冗余包被当成新帧注入）

新增场景 E/F 在**修复前**的真实输出（`64 项断言，2 项失败`，均落在场景 E）：

```
  [FAIL][E FEC 开启（合规语义）] 无重复包（duplicate=6）
  [FAIL][E FEC 开启（合规语义）] 冗余包未被当成新帧解码：解码 106 帧 == 真实帧 100
  抖动缓冲：入队 100 / 产出 100 / 缺失 0 / 重复 6 / 迟到 0 / 驱逐 0
```

修复后同一场景：

```
  抖动缓冲：入队 100 / 产出 100 / 缺失 0 / 重复 0 / 迟到 0 / 驱逐 0
  [ ok ][E FEC 开启（合规语义）] 冗余包未被当成新帧解码：解码 100 帧 == 真实帧 100
  ==== 汇总：64 项断言，0 项失败 ====
```

修复两侧的改动：`android/.../MicCaptureService.kt`（冗余包复用 `frameSeq` 且不再 `seq++`）
与 `server/src/pipeline.cpp`（解码前读 `MICLINK_FLAG_FEC`，无缺口时直接拒收）。
**服务端的能力边界**：若发送端违反约定给冗余包分配新 seq，服务端在协议层面无法区分
它与"真实的新帧"，只能照单全收（场景 F 就是这条边界的刻画，修复前后结论相同），
那 6.25% 的膨胀只能靠发送端修复消除。

> 场景 E/F 的发送端是 test_pipeline 内部对 Android 发送逻辑的**镜像**，
> 不是 `MicCaptureService.kt` 本身（Kotlin 侧从未编译过，见 B 类声明）。

> 注：Linux 上 `test_pipeline` 使用内置 **Fake 设备后端**（`--backend fake`），
> **不等于** Windows 上走真实 `DeviceIoControl` 的路径。IOCTL 那一半在 Linux 上无法验证。

### A3. `driver` 静态一致性自校验（127 项）

命令：

```bash
python3 /workspace/miclink-open/driver/verify/check_driver_consistency.py            # 人读报告
python3 /workspace/miclink-open/driver/verify/check_driver_consistency.py --quiet    # 只打汇总
python3 /workspace/miclink-open/driver/verify/check_driver_consistency.py --json
```

实测结果：`TOTAL: 127   PASS: 127   FAIL: 0` → `RESULT: ALL GREEN`（退出码 0）。

**注意**：这是**静态检查**（解析源码/工程文件，断言“做错了必然 BSOD 或静默失效”的强约束），
**不是编译、更不是运行**。它无法替代 WDK 编译与真机安装。

### A4. Android 端封包字节布局交叉校验（与 `shared/core` 权威实现逐字节比对）

命令：

```bash
cd /workspace/miclink-open/android/verify/protocol_crosscheck && ./run.sh
```

实测结果：`PASS`（退出码 0）。

| 项 | 结果 |
|---|---|
| C 侧（真实 `shared/core/src/protocol.c` 的 `miclink_packet_encode`） | `len=37`，十六进制 `574F4D500001002012345678010203040000BB80000103C00005000100000000DEADBEEF01` |
| Java 侧（`Packetizer.kt` 的逻辑镜像 `PacketizerProbe.java`） | **与 C 侧完全相同** |
| 逐字节 diff | 一致 |
| 用权威解码器回读 | `decode=37 seq=305419896 ts=16909060 rate=48000 ch=1 frames=960 payload=5 flags=1`（自洽） |
| `Packetizer.selfCheck()` 等价校验 | `SELFFIELDS-OK` |

**边界（诚实声明）**：校验的是**逻辑**（常量 / 偏移 / 字节序 / 长度自洽），
**不是 Kotlin 源码本身**——沙箱没有 Kotlin 编译器。
若改了 `Packetizer.kt` 却忘了同步 `PacketizerProbe.java`，本检查会给出失真绿灯。

### A5. Android 端 JNI 接线冒烟测试（**链接假 libopus**）

命令：

```bash
cd /workspace/miclink-open/android/verify/jni_smoketest && ./run.sh
```

实测结果：`JNI SMOKE TEST OK`（退出码 0）。

| 项 | 结果 |
|---|---|
| 生成的 JNI 符号 | `Java_com_miclink_open_OpusEncoder_nativeCreate / nativeDestroy / nativeEncode / nativePlc / nativeSetBitrate / nativeSetComplexity / nativeSetInbandFec / nativeVersion`（8 个，与 Kotlin 声明一一对应） |
| `create(48k,1ch,64kbps,960)` | 成功 |
| `encode` / `plc` 往返 | 长度一致 |
| `setBitrate` / `setComplexity` / `setInbandFec` | 返回 0 |
| 错误码契约 | 输出缓冲过小 → `-3`；非法码率/复杂度 → `-2`；无历史帧时 `plc` → `-5`；非法帧长 `create` 被拒 |
| 16 kHz 场景 | `create(16k,320)` + `encode` 成功 |

**边界（诚实声明）**：链接的是**假的 libopus**（`opus_stub_impl.cpp`，固定写 10 字节），
因此本测试只证明 **JNI 接线正确**，**不证明** Opus 编码质量、CMake 能编过、或真机 ABI 兼容。

---

## B 类：必须在 Windows + WDK 环境执行（**本沙箱完全没做过**）

| # | 验证项 | 为什么沙箱做不了 | 在哪里做 | 判据 |
|---|---|---|---|---|
| B1 | **WDK 驱动编译**（x64，Debug/Release） | 无 WDK、无 MSVC | VS 2022 + WDK，见 `docs/BUILD.md` | 生成 `miclinkvirtualmic.sys` / `MicLinkVirtualMic.inf` / `.cat` |

> **状态更新（2026-10-20）**：B1 的 **x64 Release** 已由 GitHub Actions CI 闭环（run 39345304679 全绿，InfVerif/Inf2Cat/测试签名全链路，artifact `miclink-driver-x64`）。判据中的 Debug 配置尚未覆盖。详见 `docs/TEST_REPORT.md` R4 章。**B2–B17 维持从未执行。**
| B2 | **测试签名**（`make-test-cert.ps1` + `signtool`） | 无 Windows、无 signtool | 管理员 PowerShell | `Get-AuthenticodeSignature` = Valid |
| B3 | **驱动安装**（`install.ps1 -Mode TestSign`） | 无 Windows | 管理员 PowerShell | pnputil 成功，设备枚举出现 |
| B4 | **测试签名水印 / 重启生效** | 无 Windows | 真机 | `bcdedit` 显示 `testsigning Yes`、桌面出现“测试模式”水印 |
| B5 | **端点出现在输入设备列表** | 无 Windows 音频栈 | 设置 → 系统 → 声音 → 输入 | 出现 **MicLink Virtual Microphone** 并可选为默认 |
| B6 | **控制面板“录制”选项卡状态** | 同上 | `mmsys.cpl` → 录制 | 状态“准备就绪”，非“已禁用/未插入” |
| B7 | **设备管理器无感叹号、无代码 52** | 同上 | `devmgmt.msc` | “该设备运转正常”，无代码 52 |
| B8 | **`CreateFile("\\.\MicLinkControl")` 打开成功** | 无驱动 | `tools\verify.ps1` | 检查项 PASS |
| B9 | **IOCTL GET_VERSION 返回版本号** | 无驱动 | `tools\verify.ps1` | 返回 4 个 uint32 |
| B10 | **Windows 录音机采集到手机声音** | 无 Windows | 手册第 6.1 节 | 回放能听到手机侧说话 |
| B11 | **OBS 采集到手机声音** | 无 OBS | 手册第 6.2 节 | 混音器电平跳动、录制文件有声 |
| B12 | **Zoom 采集到手机声音** | 无 Zoom、无第二人 | 手册第 6.3 节 | 输入电平跳动 + **对方确认能听到** |
| B13 | **Android 端构建（Gradle + NDK + opus）** | 无 Android SDK/NDK | Android Studio | APK 能装、能采集 |
| B14 | **Android 端与服务端联调** | 无真机 | 手册第 7 节 | 服务端持续收包、息屏不中断 |
| B15 | **卸载无残留** | 无 Windows | `tools\uninstall.ps1` | 7 项残留检查全 PASS |
| B16 | **PowerShell 脚本实际可执行性** | 无 PowerShell | 真机 | `install/uninstall/verify/make-test-cert` 均按预期退出码返回 |
| B17 | **24 小时稳定性**（无掉设备、无爆音、无内存增长） | 无真机 | 长跑 | 端点始终存在、无异常日志 |

**B13/B14 补充**：Android 工程是本次新增的，**没有编译过**（沙箱无 Android SDK/NDK/Kotlin 编译器）。
已做的只有：

* 人工自审：import 完整性、Activity/Service 生命周期、权限申请与前台服务通知渠道、
  `AudioRecord` 参数合法性、Java getter 的空安全（避免 Kotlin 智能转换失败）；
* A4：封包字节布局与服务端权威实现的逐字节比对（**已跑通**，但校验的是逻辑镜像而非 Kotlin 源码）；
* A5：JNI 接线冒烟测试（**已跑通**，但链接的是假 Opus）。

真机构建、opus 源码编译、真机联调**仍全部待执行**。

---

## 明确声明（请原样引用）

1. 本仓库的验证工作全部在 **Linux 容器**内完成；
2. **没有编译过 WDK 驱动**（沙箱无法安装 WDK）；
3. **没有安装或运行过 Windows 驱动**；
4. **没有跑过 Zoom / OBS / Windows 录音机**；
5. 因此，“虚拟麦克风出现在 Windows 设备列表并可被 Zoom 采集”这一**最终结论尚未在真实 Windows 上验证**；
6. 驾驶式验收清单见 [`docs/Windows-Manual-Verification.md`](Windows-Manual-Verification.md)，
   请按其执行并把结果回填；
7. 驱动签名方面：本项目只交付“**测试签名可安装**”的状态，
   **生产签名（EV + WHQL / Attestation）需要证书持有者自行完成**，见 [`docs/Signing-Guide.md`](Signing-Guide.md)。

---

## 复跑命令汇总

```bash
# ---- A 类：Linux 上可复跑 ----

# A1: shared/core（7 个用例）
cd /workspace/miclink-open/tests && ./run_tests.sh

# A2: server（构建 + 端到端 64 项断言）
cmake -S /workspace/miclink-open/server -B /workspace/miclink-open/server/build -DCMAKE_BUILD_TYPE=Release
cmake --build /workspace/miclink-open/server/build
ctest --test-dir /workspace/miclink-open/server/build --output-on-failure

# A3: driver 静态一致性（127 项）
python3 /workspace/miclink-open/driver/verify/check_driver_consistency.py --quiet

# A4: Android 封包字节布局 vs shared/core 权威实现（逐字节比对）
cd /workspace/miclink-open/android/verify/protocol_crosscheck && ./run.sh

# A5: Android JNI 接线冒烟测试（链接 stub opus）
cd /workspace/miclink-open/android/verify/jni_smoketest && ./run.sh
```

```powershell
# ---- B 类：只能在 Windows 上跑（均未执行过） ----
powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force
# 重启
powershell -ExecutionPolicy Bypass -File tools\verify.ps1
# 人工：docs\Windows-Manual-Verification.md
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
```

```bash
# ---- B13：Android（未执行过） ----
# 先按 android/README.md 放入 opus 源码到 android/app/src/main/cpp/opus/
cd /workspace/miclink-open/android && ./gradlew :app:assembleDebug
```
