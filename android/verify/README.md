# android/verify —— 在 Linux 上能做的 Android 端交叉校验

沙箱里**没有 Android SDK / NDK / Kotlin 编译器**，无法真正编译 `android/app` 工程。
但有两件"错了就必定联调失败"的事，可以脱离 Android 环境校验，就放在这个目录：

| 目录 | 校验什么 | 前置 | 命令 |
|---|---|---|---|
| `protocol_crosscheck/` | Android 端封包的**字节布局**是否与 `shared/core` 权威实现一致 | gcc + JDK | `cd protocol_crosscheck && ./run.sh` |
| `jni_smoketest/` | `opus_jni.cpp` 的 **JNI 符号名 / 参数签名 / 错误码**是否与 Kotlin 声明一致 | g++ + JDK | `cd jni_smoketest && ./run.sh` |

两者都在 Linux 沙箱内**真实跑通**（详见 `docs/VERIFICATION.md` 的 A4 / A5）。

---

## ⚠️ 这两个检查**不能**证明什么

请务必理解它们的边界，别把绿灯当成"Android 端可用"：

| 检查 | 能证明 | **不能**证明 |
|---|---|---|
| protocol_crosscheck | 常量、字段偏移、字节序、长度自洽性正确 | Kotlin 源码能编译；`Packetizer.kt` 与 `PacketizerProbe.java` 是否同步 |
| jni_smoketest | JNI 函数名/签名与 Kotlin 声明匹配；错误码契约成立 | Opus 编码质量（链接的是**假 Opus**）；CMake 能编过；真机 ABI 兼容 |

**维护约定**：

* 改了 `Packetizer.kt` 的常量或 `pack()` 逻辑 → **必须同步** `protocol_crosscheck/PacketizerProbe.java`，
  否则这里会给出失真绿灯；
* 改了 `OpusEncoder.kt` 的 native 方法名/签名，或 `opus_jni.cpp` 的 JNI 函数 →
  **必须同步** `jni_smoketest/OpusEncoder.java`（类名也叫 `OpusEncoder`，因为 JNI 符号名里含类名）；
* `jni_smoketest/` 下的 `stub_opus.h` / `stub_android_log.h` / `opus_stub_impl.cpp` 是**假实现**，
  **绝对不参与 Android 构建**（它们不在 `app/src/main/cpp` 下）。

---

## 仍然必须由真机验证的部分

* Gradle 工程能否打开并构建（AGP / NDK / CMake 版本匹配）；
* 放入真实 opus 源码后，CMake 能否编译 libopus 并链接；
* `AudioRecord` 在真机上的实际采样率与权限行为；
* 前台服务、通知渠道、`PARTIAL_WAKE_LOCK` 的实际效果；
* 与 Windows 服务端联调（见 `docs/Windows-Manual-Verification.md` 第 7 节）。
