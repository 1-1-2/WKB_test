# 构建说明（Android 采集端 + Windows 客户端）

> **状态提示**：本仓库的开发沙箱是 **Linux**。Android 端与 Windows 客户端的构建 /
> 运行均在 **GitHub Actions 真机 runner** 上验证过（`android.yml` 的
> `integration-test` 作业、`windows-e2e.yml` 全链路、`tailscale-dual.yml` 双机联调），
> 本文命令与 CI 实际使用的流程一致；本地首次构建如遇环境问题，可对照对应
> workflow 复现。自研线（driver/server/shared/tests）的构建说明已随该线归档至
> `self-driver` 分支。

---

## 1. 总览

| 组件 | 语言 / 工具链 | 产物 | 构建方式 |
|---|---|---|---|
| Android 采集端（`android/`） | Kotlin + Compose + NDK CMake（libopus JNI） | `miclink-open-debug.apk` | Gradle wrapper |
| Windows 客户端（`client/`） | C99 + Win32 + 自绘 UI（Opus 静态或运行时） | `womic.exe` + `womic-drvinst.exe` | mingw-w64 交叉编译或原生 `build.bat` |
| CI 脚本（`.github/scripts/`） | bash + python3 | — | 随 workflow 在 runner 上执行 |

---

## 2. 构建 Android 采集端

### 2.1 环境要求

| 组件 | 版本 | 说明 |
|---|---|---|
| Android Studio | 近新版 | 或命令行直接用 Gradle wrapper |
| AGP / Gradle | **9.1.0** / **9.3.1** | 版本目录见 `android/gradle/libs.versions.toml` |
| Kotlin | 2.2.21 | 同上 |
| JDK | **17+** | AGP 9 要求 |
| NDK | 27+（CI 用 28.2.13676358） | 编译 `opus_jni.cpp` |
| CMake | 3.22.1（随 SDK） | externalNativeBuild |
| compileSdk / minSdk | **37** / **26** | |

### 2.2 前置必做：准备 opus 源码

仓库**不内置** opus 源码（体积 + 许可证单独保留）。把 opus 1.4+ 的 release 源码放到
`android/app/src/main/cpp/opus/`，否则 CMake 会以清晰的 `FATAL_ERROR` 中止
（不会静默失败）。CI 上由 `.github/scripts/fetch-opus.sh` 现场下载。

### 2.3 构建

```bash
cd android
./gradlew :app:assembleDebug        # APK：app/build/outputs/apk/debug/
./gradlew :core:test                # 纯 JVM 协议模块 23 个字节级测试
./gradlew :app:assembleDebug :core:test   # CI 的 integration-test 作业即此组合 + 模拟器联调
```

调试音模式免麦克风即可闭环联调：启动 app 后 PC 端连接，断言清单见
[`android/verify/integration/README.md`](../android/verify/integration/README.md)。

---

## 3. 构建 Windows 客户端

### 3.1 环境要求

| 方式 | 要求 | 说明 |
|---|---|---|
| Linux 交叉编译（**推荐**，与 CI 产物一致） | `gcc-mingw-w64-x86-64` + `python3` | `x86_64-w64-mingw32-gcc` / `-windres` 在 PATH |
| Windows 原生 | mingw-w64 的 gcc 在 PATH 即可 | 跑 `client\build.bat`，**无需 VS/SDK** |

不用 MSVC：MSVC 产物依赖 `vcruntime140.dll`，正是要避免的"额外运行库"；
mingw 的 `-Os -s` 已足够小，且只链系统 DLL（`KERNEL32` `USER32` `GDI32`
`SHELL32` `UxTheme` `WS2_32` `msvcrt` `ADVAPI32` `ole32` `SETUPAPI` `newdev`
`CRYPT32`，全部为 Windows 自带）。

### 3.2 构建

```bash
cd client
./build.sh            # 默认 all：womic.exe + womic-drvinst.exe 都编
./build.sh app        # 只编 womic.exe（应用本体，零第三方二进制）
./build.sh drvinst    # 只编 womic-drvinst.exe（驱动安装器）
./build.sh test       # 驱动包 inflate 字节自检（wine 下 9/9 PASS）
```

| 产物 | 实测大小 | 子系统 / manifest | 说明 |
|---|---|---|---|
| `build/womic.exe` | 298,496 B（291.5 KiB） | GUI / `asInvoker` | 应用本体：GUI / CLI / 网络层 / Opus / 驱动调用，**不含任何第三方二进制** |
| `build/womic-drvinst.exe` | 214,016 B（内嵌三套驱动）／ 113,664 B（空包构建） | console / `requireAdministrator` | 驱动安装器：只负责装 / 卸 / 查驱动 |

### 3.3 驱动包（内置 / 外置 / 空包）

源码库**不含**第三方驱动二进制（`womic.sys` 等版权归 Wolicheng 所有，公开仓库
再分发有法律风险，架构评审 R8；`res/drvpkg.bin` 与 `src/drvpkg_data.h` 已列入
`.gitignore`）。`build.sh` 的处理顺序：

1. 尝试真实打包：`tools/pack_driver.py` 按 `--driver-dir` 参数 > `WOMIC_DRIVER_DIR`
   环境变量 > 内置候选路径列表查找官方驱动目录（需要 2022 版 2.1.0.0 三件套：
   `womic.inf` / `womic_1.inf` / `womic_2.inf` 及同名 `.sys` / `.cat`，
   硬件 ID `Root\WOMic`；2017 版握手不兼容，一律不打包）；
2. 找不到则 `--empty` 生成合法空包（magic `MICLDRV1`、ver=1、count=0，16 B）兜底，
   只打 warned 日志、不中断构建。

空包形态下：`womic.exe` 与驱动包完全无关（本来就不带）；`womic-drvinst.exe` 的
`/install` 会明确报「未内置驱动包（空包构建）」并返回退出码 4，**绝不静默成功**。
空包安装器经外置驱动目录工作：

```
womic-drvinst.exe /driver-pack D:\drivers\womic2022 /install
womic.exe --driver-pack D:\drivers\womic2022 --install-driver
```

### 3.4 自检（每个产物都要过）

`build.sh` 对每个产物自动执行三项断言，任一不过即构建失败：

- **体积预算**：应用 320 KiB / 安装器 256 KiB（`SIZE_BUDGET_APP` / `SIZE_BUDGET_DRVINST` 可覆盖）；
- **导入表只允许系统 DLL**：`libgcc` / `libwinpthread` / `vcruntime` / `msvcp` / `api-ms-win-crt` / opus 出现即为失败；
- **资源面断言**（`tools/check_pe.py`）：应用 `.rsrc` 内**不许**出现 `MICLDRV1`，安装器**必须**内置且头合法。

### 3.5 Opus 依赖

`build.sh` 检测到 `deps/opus/lib/libopus.a` 时传 `-DMICLINK_HAVE_OPUS` 静态链接
（`-Wl,--gc-sections` 把 encoder 全部裁掉），没有 `opus.dll` 也能出声；
干净环境无静态库时回退运行时 `LoadLibrary("opus.dll")`，功能不缺。
静态库由 `tools/build_opus.sh` 交叉编译生成（首次需要，产物在 `client/deps/`，不入库）。

---

## 4. 本地快速自测（无需手机）

| 工具 | 用途 |
|---|---|
| `client/tools/fake_server.py` | 服务端参考实现：无手机也能端到端联调（101/102/103 全接受、按 20 ms 下发格式 C 帧） |
| `client/tools/test_drvpkg.c`（`./build.sh test`） | 驱动包 inflate 字节级自检（wine 下 9/9 PASS） |
| `android/verify/integration/pc_client.py` | Android 侧联调脚本：自测 9 项 + mock 服务端 15 项闭环干跑（C01–C15 断言） |
| `client/verify/media_port_check.py` | `--media-port` 固定媒体端口的行为校验 |

真机侧（Windows runner）的端到端与双机联调由 CI 承担，见下节。

---

## 5. CI 作业

| Workflow | Runner | 内容 | 触发 |
|---|---|---|---|
| `android.yml` | ubuntu + KVM | SDK 安装 → 模拟器 → 装配 APK → 调试音模式协议闭环（C01–C15） | push 影响 `android/**` / 手动 |
| `windows-e2e.yml` | windows | 交叉构建 → 驱动安装器装官方驱动 → SYSTEM 计划任务跑 `e2e_probe`（waveIn 录音取证）→ GUI 冒烟 → R3 权限矩阵 | push 影响 `client/**` / 手动 |
| `tailscale-dual.yml` | windows + ubuntu 双机 | 真 `womic.exe` ↔ 真 app 跨 Tailscale 全链路 + 440 Hz 音频取证 | push 影响 `client/**` `android/**` / 手动 |

报告：[`windows-e2e-report.md`](windows-e2e-report.md)（真机端到端三轮 run 记录）；
双机联调结论见会话交付报告（私有交付包）。

---

## 6. 常见构建问题

| 现象 | 原因 / 处理 |
|---|---|
| CMake 报 opus 源码缺失 | 按 §2.2 放好 `android/app/src/main/cpp/opus/`（CI 上自动下载） |
| `build.sh` 提示空包 warned | 未找到官方驱动目录；要内置驱动按 §3.3 自备目录后重跑 `pack_driver.py` |
| 产物导入表自检失败 | 链了非系统库；检查是否误开 MSVC 或漏掉 `-Wl,--gc-sections` 之外的静态库 |
| 应用启动无界面输出 | GUI 子系统用 `AttachConsole` 兜 CLI 输出；排障请加 `--help` 或用 CLI 参数直连 |
| wine 下 UI 异常 | 确认 Xvfb 在跑；DPI/DWM 相关仅代码层保证，真机首跑先看 `--light` |
