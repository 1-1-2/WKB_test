# android/verify —— 在 Linux 上能做的 Android 端交叉校验

沙箱里**没有 Android SDK / NDK / Kotlin 编译器**，无法真正编译 `android/app` 工程。
但有两类"错了就必定联调失败"的事，可以脱离 Android 环境校验：

| 校验 | 位置 | 前置 | 命令 |
|---|---|---|---|
| WO Mic 协议**字节布局**（TCP 帧 / hello / config / stop 怪癖 / UDP 格式 C） | `android/core` 的 `:core:test` 单元测试（ProtocolConformanceTest + UdpPacketizerTest，23 个用例，向量全部来自官方 6.3 反汇编） | JDK 17 + Gradle | `cd android && gradle :core:test` |
| `opus_jni.cpp` 的 **JNI 符号名 / 参数签名 / 错误码**是否与 Kotlin 声明一致 | `jni_smoketest/` | g++ + JDK | `cd jni_smoketest && ./run.sh` |

两者都在 Linux 沙箱内**真实跑通**。

> 协议切换到 WO Mic 后，旧的 `protocol_crosscheck/`（校验自研 WOMP 32 字节头的
> Kotlin↔Java↔C 三方比对）已删除：字节级验证改由 `:core` 单元测试承担，
> 测试向量即官方客户端反汇编摘录，比三方镜像更直接。

---

## ⚠️ 这些检查**不能**证明什么

请务必理解它们的边界，别把绿灯当成"Android 端可用"：

| 检查 | 能证明 | **不能**证明 |
|---|---|---|
| `:core:test` | 协议编解码/会话状态机/包化的字节行为与官方一致 | Kotlin/Compose UI 能编译；真机网络行为 |
| jni_smoketest | JNI 函数名/签名与 Kotlin 声明匹配；错误码契约成立 | Opus 编码质量（链接的是**假 Opus**）；CMake 能编过；真机 ABI 兼容 |

**维护约定**：

* 改了 `OpusEncoder.kt` 的 native 方法名/签名，或 `opus_jni.cpp` 的 JNI 函数 →
  **必须同步** `jni_smoketest/OpusEncoder.java`（类名也叫 `OpusEncoder`，因为 JNI 符号名里含类名）；
* `jni_smoketest/` 下的 `stub_opus.h` / `stub_android_log.h` / `opus_stub_impl.cpp` 是**假实现**，
  **绝对不参与 Android 构建**（它们不在 `app/src/main/cpp` 下）。

---

## 仍然必须由真机 / CI 验证的部分

* Gradle 工程能否打开并构建（AGP 8.13 / NDK / CMake 版本匹配，Compose 编译）；
* 放入真实 opus 源码后，CMake 能否编译 libopus 并链接；
* `AudioRecord` 在真机上的实际采样率与权限行为；
* 前台服务、通知渠道、`PARTIAL_WAKE_LOCK` 的实际效果；
* 与官方 Windows 客户端 6.3 的真机联调（协议规格 v1 第 6 节待实测清单 T1-T6）。
