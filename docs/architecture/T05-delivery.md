# T05 交付说明 — Android：主题系统 + 抽屉导航

> 任务书：`docs/architecture/incremental-2026-10.md` §3 T05（第 358 行起）
> 工作目录：`android/`；构建：`JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 ./gradlew ...`

## 1. TL;DR

T05 完成：Android 侧落地「8 套种子调色板（4 活泼 + 4 低饱和）+ 系统动态取色 + AMOLED 纯黑 + 横向抽屉导航」。配色经 **material-kolor 4.0.5** 生成（版本选型见 §4.1，风险 R12 闭环），全部设置 4 键持久化、即时生效、重启保持。`:app:assembleDebug` / `:app:testDebugUnitTest` / `:core:test` / `:app:assembleRelease`（R8）全绿；新增 14 个 app 单测覆盖 AMOLED 梯度与 WCAG AA 等可机器验证的验收点。

## 2. 改动文件清单

### 新建（相对 `android/app/src/main/java/com/miclink/open/`）

| 文件 | 内容 |
|---|---|
| `theme/ThemeMode.kt` | `enum class ThemeMode { SYSTEM, LIGHT, DARK }` + `fromName()` 容错解析 |
| `theme/AppPalette.kt` | `sealed interface AppPalette`（`Dynamic` / `Seed(id, labelRes, seedColor, style)`）；预置 4 活泼 + 4 低饱和；`FALLBACK_SEED`（品牌蓝 #1E88E5，取自启动图标）；`resolveForDevice(dynamicSupported)` |
| `theme/ColorSchemes.kt` | `buildColorScheme(mode, palette, amoled, isDark[, dynamicScheme])` 纯计算版 + Context 版；`ColorScheme.withAmoled()` 纯黑覆写；`contrastRatio()` / `relativeLuminance()`（WCAG 2.1） |
| `theme/ThemePrefs.kt` | `ThemeState(mode, palette, amoled)` + 4 键持久化（`theme_mode` / `theme_palette` / `theme_style` / `theme_amoled`）+ `verifyDefaults()` 自检 |
| `ui/theme/Theme.kt` | `WomicTheme(mode, palette, amoled, content)`，取代 MainActivity 内联主题；含系统栏着色 SideEffect |
| `ui/navigation/AppNav.kt` | `ModalNavigationDrawer` + `NavHost`（主页/外观/关于与帮助）+ `TopAppBar` + `BackHandler` 关抽屉 |
| `ui/screen/HomeScreen.kt` | 从 MainActivity 搬出 `MainScreen` / `StatusCard` / `StatItem`（电平改用 VoiceMeter；底部"使用说明"卡移至 AboutScreen） |
| `ui/screen/AppearanceScreen.kt` | 主题模式单选 + 配色色板（磁贴色点 + 顶部实时预览卡）+ AMOLED 开关 |
| `ui/screen/AboutScreen.kt` | 使用说明 / 协议说明 / 版本（BuildConfig + libopus）/ 开源许可 |
| `ui/component/VoiceMeter.kt` | LevelBar 升级：RMS/Peak 双条 + dB 读数 + 状态徽标（有信号/无信号/过载）+ 单帧循环平滑（RMS 指数逼近、Peak 保持 1.2s 后 12dB/s 衰减） |
| `app/src/test/kotlin/com/miclink/open/theme/ThemeColorSchemeTest.kt` | 14 个纯 JVM 单测（AMOLED 梯度/对比度、预置表、动态取色回退、明暗契约） |
| `res/values/colors.xml`、`res/values/themes.xml`、`res/values-night/{colors,themes}.xml` | 启动窗口背景色（防止深色模式冷启动闪亮色；见 §4.2 决策 D3） |

### 修改

| 文件 | 改动 |
|---|---|
| `MainActivity.kt` | `setContent` 改用 `WomicTheme` + `AppNav`；删除内联 `WomicWifiClientTheme` / `MainScreen` / `StatusCard` / `StatItem` / `LevelBar` / `statusColors` 及其 Preview；`onCreate` 增补 `vm.loadTheme(prefs)` |
| `MainViewModel.kt` | 新增 `themeState: ThemeState`、`loadTheme(prefs)`、`onThemeModeChanged/onThemePaletteChanged/onThemeAmoledChanged`（统一走 `updateTheme()`：先改状态即时生效，再落盘） |
| `Prefs.kt` | `FILE_NAME` 改 `internal`（ThemePrefs 复用同一文件）；新增 `themePrefs` 委托实例 + `loadTheme()/saveTheme()`；`verifyDefaults()` 末尾调用 `ThemePrefs.verifyDefaults()` |
| `res/values/strings.xml` | 新增导航 4 条、外观 22 条、电平表 6 条、关于 7 条（共 39 条，全部中文） |
| `gradle/libs.versions.toml` | 新增 `navigationCompose = 2.9.8`、`materialKolor = 4.0.5` 及对应 library 条目 |
| `app/build.gradle.kts` | 新增 `navigation-compose`、`material-kolor`（排除 `org.jetbrains.compose` 组）、`testImplementation(junit)` |
| `AndroidManifest.xml` | `android:theme` 由平台主题改为 `@style/Theme.Womic`（仅多一个 windowBackground） |

未触碰：`core/`、`cpp/`（opus 源码）、协议实现、`client/`（Windows 侧）。

## 3. 验收标准逐条达成证据

### #1 8 套配色 × 明暗 + 动态取色，切换即时生效无闪烁

- **8 套 × 明暗**：`AppPalette.PRESETS`（AppPalette.kt:70-127）4 活泼（vibrant_coral/magenta/amber/teal）+ 4 低饱和（muted_mist/sage/rose/stone）。单测 `everyPreset_producesDistinctScheme_inBothLightAndDark`：明/暗各 8 个 primary 互不相同，且同一套明暗 primary 不同（生成器有分化，非简单反转）。
- **动态取色**：Android 12+ 走 `dynamicDarkColorScheme/dynamicLightColorScheme`（ColorSchemes.kt:63-76）。
- **即时生效无闪烁**：`MainViewModel.updateTheme()`（MainViewModel.kt:129-132）先改 `themeState`（Compose 状态）再落盘；`WomicTheme` 用 `remember(mode, palette, amoled, dark)` 重建 ColorScheme（Theme.kt:42-51），状态驱动重组、**不重建 Activity**，因此无重建闪烁。切换路径：`AppearanceScreen` 点击 → `vm.onTheme*Changed` → 状态变化 → `WomicTheme` 重组。

### #2 AMOLED：surface == background == #000000，卡片可辨，onSurface 达 WCAG AA

`ColorScheme.withAmoled()`（ColorSchemes.kt:100-112）+ 单测（ThemeColorSchemeTest.kt）：

| 断言 | 测试 | 结果 |
|---|---|---|
| `surface == background == surfaceDim == #000000` | `amoled_surfaceAndBackgroundArePureBlack`（8 套逐一验） | 通过 |
| `surfaceContainerLowest→Highest` 亮度**单调递增**，且 Low 及以上层与纯黑亮度差 ≥ 0.002（卡片可辨），最亮层不亮于 #303030 | `amoled_containersFormPerceptibleGradient` | 通过 |
| `onSurface` / `onSurfaceVariant` 对纯黑对比度 ≥ 4.5:1（AA），不足时 `ensureAaForeground()` 兜底替换 #ECECEC | `amoled_onSurfaceMeetsWcagAa` | 通过 |
| 未开 AMOLED 的深色主题不被污染（surface 非纯黑）；浅色 + amoled=true 被忽略 | `nonAmoled_darkScheme_keepsMaterialSurface` / `lightMode_amoledIsIgnored` | 通过 |

AMOLED 开关仅在深色可用（浅色会文字背景一起变黑），AppearanceScreen 给出禁用态与说明文案。

### #3 重启 App 主题设置保持

`ThemePrefs`（ThemePrefs.kt:41-73）4 键写入与 `Prefs` 同一 SharedPreferences 文件（`womic_wifi_client_prefs`，经 `Prefs.FILE_NAME` 共享，无第二处硬编码）；`save()` 用 `apply()` 异步落盘。启动路径：`MainActivity.onCreate` → `Prefs.verifyDefaults()`（含主题默认值自检）→ `vm.loadTheme(prefs)`。`style` 与 `id` 解耦持久化的理由见 ThemePrefs.kt:34-37 注释（预置表将来调风格不静默覆盖用户偏好）。

### #4 抽屉边缘滑出、三目的地切换、返回键先关抽屉

`AppNav.kt`：
- 边缘滑出：`ModalNavigationDrawer(gesturesEnabled = true)`（AppNav.kt:88-90），另可点左上角菜单按钮（AppNav.kt:124-133）；
- 三目的地：`NavHost` + `popUpTo(HOME){saveState} + restoreState` 扁平切换，不堆栈、主页状态可恢复（AppNav.kt:137-155）；
- 返回键：`BackHandler(enabled = drawerState.isOpen) { drawerState.close() }`（AppNav.kt:84-86）——抽屉展开时先收抽屉，不回退导航栈/不退 App。

### #5 Android 12+ 动态取色；Android 11 及以下自动回退 Seed 档且不崩

- `supportsDynamicColor()`（ColorSchemes.kt:23，SDK ≥ 31）为唯一判据，UI 提示与配色回退共用；
- Context 版 `buildColorScheme` 在 SDK < 31 时传 `dynamicScheme = null`，纯计算版回退 `FALLBACK_SEED`（品牌蓝 #1E88E5，与桌面图标一致）；
- 单测：`dynamicPalette_fallsBackToSeedWhenDynamicSchemeUnavailable`（回退方案 primary/surface 与品牌蓝种子方案一致）、`dynamicPalette_usesProvidedDynamicSchemeWhenAvailable`、`resolveForDevice_switchesDynamicToFallbackOnOldAndroid`；
- "且不崩"：低版本不实例化任何 `dynamic*ColorScheme` API；预置表/默认值由 `ThemePrefs.verifyDefaults()` 在启动期兜底（对齐 D-11 的"崩在启动期"思路）。

### #6 构建与测试全绿

```console
$ JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 ./gradlew \
    :app:assembleDebug :app:testDebugUnitTest :core:test :app:assembleRelease --console=plain
...
> Task :app:testDebugUnitTest
> Task :app:assembleDebug
> Task :app:assembleRelease
BUILD SUCCESSFUL in 1m 59s
```

测试结果（`build/test-results/**/TEST-*.xml`）：

| 模块 | 用例数 | 失败 | 错误 | 跳过 |
|---|---|---|---|---|
| `:app:testDebugUnitTest`（ThemeColorSchemeTest，本轮新增） | 14 | 0 | 0 | 0 |
| `:core:test`（ProtocolConformanceTest 16 + UdpPacketizerTest 7） | 23 | 0 | 0 | 0 |

`assembleRelease` 为额外回归（R8 + 资源收缩）：通过，无 missing-class 错误，说明主题/导航新代码与现有 ProGuard 规则兼容。

**构建产物**：

| 产物 | 路径 | 体积 |
|---|---|---|
| Debug APK | `android/app/build/outputs/apk/debug/app-debug.apk` | 20,977,711 B（20.0 MiB） |
| Release APK（未签名，额外回归） | `android/app/build/outputs/apk/release/app-release-unsigned.apk` | 3,495,082 B（3.3 MiB） |

## 4. 风险与决策

### 4.1 material-kolor 兼容性（R12）—— 选 4.0.5，不选 5.0.1

任务书预警的 R12 真实存在，且比"编译不过"更隐蔽：**Kotlin 元数据版本不匹配**。

- 最新 5.0.1 的类文件元数据是 **mv=[2,4,0]**（Kotlin 2.4 编译），本项目编译器是 Kotlin **2.2.21**（libs.versions.toml），直接报 "class was compiled with an incompatible version of Kotlin"；其 POM 还传递 `kotlin-stdlib:2.4.10`，同样读不了。
- 逐一核实候选版本后选定 **4.0.5**：元数据 **mv=[2,0,0]**（2.2.21 可读），`kotlin-stdlib` 依赖正好 **2.2.21**（零版本冲突），`material-color-utilities`（mv 2.0）、`colormath-jvm`（mv 2.1）均在可读范围。
- 兼容面核实：4.0.5 的 android AAR **不引用任何 `android/` 平台类**（125 个 class 全量 grep 验证），只依赖 `androidx.compose.material3.ColorScheme` 构造器（41 参全参构造，1.9→1.12 仅追加带默认值参数，二进制兼容）——这也是 app 单测能在纯 JVM 跑通种子生成的原因。
- 传递依赖裁剪：其 POM 里的 `org.jetbrains.compose.*`（Compose Multiplatform 重分发）在 Android 侧全部由 composeBom 提供，`build.gradle.kts` 用 `exclude(group = "org.jetbrains.compose")` 排除，避免拉入一整套 JB 变体（体积与类冲突风险）。
- **未走**"手写 ColorScheme"的回退方案：既然 4.0.5 验证可用，按任务书优先级先用库。API 用法以 sources jar 为准（`dynamicColorScheme(seedColor, isDark, style)`），未使用其自带 `isAmoled` 参数——那个实现只把 background/surface 涂黑、onBackground 强制纯白，**不给 surfaceContainer* 梯度**，不满足验收 #2，故 AMOLED 覆写为自研（`withAmoled()`）并配单测。

### 4.2 其他决策

- **D1 4 键的构成**：任务书只写"4 键持久化"，未列键名。定为 `theme_mode` / `theme_palette` / `theme_style` / `theme_amoled`：`AppPalette.Seed` 有 (id, style) 两个数据字段，分别持久化可在预置表将来调整风格时不静默覆盖用户已存偏好（ThemePrefs.kt:34-37）。
- **D2 ThemeState 定义位置**：放在 `theme/ThemePrefs.kt` 而非 MainViewModel——ThemePrefs 要读写它，放根包会形成 MainViewModel ↔ ThemePrefs 循环依赖（ThemePrefs.kt:11-16 注释）。
- **D3 启动窗口背景**（超出任务书文件表的小改）：新增 `values/themes.xml` + `values-night/` 变体，只覆盖 `windowBackground`。理由：主题是 T05 的主题，深色模式冷启动若先闪亮色窗口，"无闪烁"只做了一半。forced 模式与系统深色不一致时（用户强制浅色而系统深色）仍会有一次过渡，属可接受残留。
- **D4 主页移除"使用说明"卡**：导航重构后该内容移入 AboutScreen，避免两处维护同一份文案（strings 未删，AboutScreen 复用 `usage_title/usage_body/protocol_note`）。
- **D5 音量/端口等运行参数不动**：严格最小变更，只搬代码不改行为。
- **D6 未引入新图标依赖**：material-icons-extended 本就是既有依赖（app/build.gradle.kts 原有），仅多用 Menu/Home/Palette/Info 四个图标，不新增依赖。

### 4.3 环境备注（供后续复跑）

- `./gradlew` 首次运行需从 services.gradle.org 拉 gradle-9.3.1-bin.zip，沙箱直连被阻断；已从腾讯镜像（`mirrors.tencent.com/gradle/`）下载并放入 wrapper 缓存，后续 `./gradlew` 正常。依赖解析走 settings.gradle.kts 既有腾讯镜像 + google()，无需额外代理。
- 构建用 JDK 21（`JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64`），默认 java 是 Zulu 20。

## 5. 遗留事项

1. **真机/模拟器 UI 验收未做**（沙箱无显示设备）：抽屉边缘滑出手感、三目的地转场、VoiceMeter 平滑衰减的肉眼效果、动态取色在 Android 12+ 真机的实际取色，需装 APK 过一遍；代码层约束（BackHandler、gesturesEnabled、帧循环）已按验收 #4 实现。
2. **AMOLED 对比度为计算值**：WCAG AA 由单测按 WCAG 2.1 公式计算（onSurface/onSurfaceVariant 对纯黑 ≥ 4.5:1），未做真机亮度实测；`surfaceContainerLowest`（#050505）与纯黑的亮度差约 0.0015，属"最凹陷层"的刻意淡梯度，卡片实际使用的 Low 及以上层差 ≥ 0.002（有测试断言）。
3. **Debug APK 体积 +723 KB**（20,254,212 → 20,977,711 B，+3.6%）：增量来自 navigation-compose 与 material-kolor 依赖树在无 R8 的 debug 构建中全量入包，属任务书已说明的既有成因（debug 无 R8）的线性延伸；release（R8）为 3.3 MiB。若后续要压 debug 体积，方向是给 debug 也开 R8/缩小，与本轮需求无关。
4. **forced 模式 × 系统深色不一致时有一次窗口过渡**（见 D3）：如用户强制浅色而系统深色，会先展示深色启动窗口再切浅色内容；默认（跟随系统）场景无此问题，收益大于残留，未进一步处理。
5. 发布签名仍为空（build.gradle.kts 既有约定，留待使用者配置）。

## 6. 复跑命令

```bash
cd android
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
./gradlew :app:assembleDebug :app:testDebugUnitTest :core:test
# 额外 R8 回归：./gradlew :app:assembleRelease
```
