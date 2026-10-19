# Windows 人工验证手册

> **这份文档是给你在 Windows 真机上逐条执行的。**
> 本仓库的开发环境是 Linux 沙箱，**驱动从未被编译、安装、运行过**，也**没有跑过录音机 / OBS / Zoom**。
> 本文所有“预期结果”都是依据设计与契约推导的**待验证结论**，不是已验证结论。
> 每节都有“预期 / 实际 / 通过与否”三栏，请如实填写；遇到不符请按文末的“失败处置”记录现场。

**建议全程开一个记事本，记录每一项的「实际结果」与截图。**

---

## 0. 前置清单

| # | 项目 | 状态 |
|---|---|---|
| 1 | **Windows 11 21H2（build 22000）+，x64**（INF 声明的目标 OS；源码下限是 Win10 2004/19041，但 INF 未为其声明模型，见 `README.md`） | ☐ |
| 2 | 已编译出 `MicLinkVirtualMic.inf` / `miclinkvirtualmic.sys`（见 `docs/BUILD.md`） | ☐ |
| 3 | 已编译出 `miclinkserver.exe` | ☐ |
| 4 | 手机与 PC 在**同一局域网**（建议同一 Wi-Fi，最好 PC 有线） | ☐ |
| 5 | 已知 PC 的局域网 IPv4（`ipconfig` 查看） | ☐ |
| 6 | Android 手机已安装采集端 APK（见 `android/README.md`） | ☐ |
| 7 | 建议准备另一台设备/账号用于验证 Zoom 通话（自听回放不够） | ☐ |

---

## 1. 开启测试签名并确认“测试模式”水印

> **仅测试签名路线需要。生产签名路线请跳过本节**（生产模式不需要开测试签名，也不会有水印）。

```powershell
# 管理员 PowerShell
bcdedit /set testsigning on
# 然后**必须重启**
shutdown /r /t 0
```

| 检查项 | 操作步骤 | 预期结果 | 实际 | 通过 |
|---|---|---|---|---|
| 1.1 设置生效 | 管理员 PowerShell 执行 `bcdedit /enum "{current}"` | 输出中有 `testsigning    Yes` | | ☐ |
| 1.2 **水印出现** | 重启后看**桌面右下角** | 出现“测试模式 / Test Mode”水印（含 Windows 版本信息） | | ☐ |
| 1.3 Secure Boot 状态 | 管理员 PowerShell 执行 `Confirm-SecureBootUEFI` | 返回 `False`（测试签名在 Secure Boot 开启时无法加载；若为 True 需进 UEFI 关闭） | | ☐ |
| 1.4 一键脚本 | `powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force` | 打印黄色 **TestSign** 横幅，提示需要重启和水印 | | ☐ |

> ⚠️ 水印是**预期现象**不是故障。它意味着这台机器当前允许加载测试签名驱动。
> **测试签名仅限本机开发自测，绝对不能这样分发给普通用户。**

---

## 2. 安装驱动并重启

```powershell
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force
shutdown /r /t 0        # 重启（新设备枚举、音频端点刷新需要）
```

| 检查项 | 操作 | 预期结果 | 实际 | 通过 |
|---|---|---|---|---|
| 2.1 pnputil 成功 | 看脚本输出 | 出现 `Driver package added successfully` / `已成功安装` 之类；退出码 0 | | ☐ |
| 2.2 设备枚举 | 脚本末尾等待枚举（默认 90 秒） | 打印 `设备已枚举：MicLink Virtual Microphone / ROOT\MicLinkVirtualMic...` | | ☐ |
| 2.3 无问题码 | 同上输出 | **没有** “设备有问题码” 的 FAIL 行（尤其不能是 52） | | ☐ |

若 2.2 超时未枚举：先重启，重启后跑 `tools\verify.ps1` 再看。

---

## 3. 设备管理器：无黄色感叹号、无代码 52

| 步骤 | 操作 |
|---|---|
| 1 | `Win + X` → **设备管理器** |
| 2 | 展开 **“声音、视频和游戏控制器”** |
| 3 | 找到 **MicLink Virtual Microphone**，双击打开属性 |
| 4 | 看 **“设备状态”** 文本框 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 3.1 图标无黄色感叹号 / 无向下箭头 | 正常图标 | | ☐ |
| 3.2 设备状态文本 | **“该设备运转正常。”** | | ☐ |
| 3.3 **无代码 52** | 不出现 `代码 52`（Windows 无法验证此设备所需的驱动程序的数字签名） | | ☐ |
| 3.4 驱动程序选项卡 → 驱动程序详细信息 | 列出 `miclinkvirtualmic.sys` | | ☐ |
| 3.5 驱动程序选项卡 → 数字签名（若已签名） | 显示签名者名称 | | ☐ |

**出现代码 52 怎么办**（按概率排序）：

1. 没开测试签名或没重启 → 回到第 1 节，`bcdedit /enum "{current}"` 确认 `Yes`，**重启**；
2. Secure Boot 仍开启 → 进 UEFI 关闭；
3. `.cat` 缺失或未签名 → 跑 `tools\make-test-cert.ps1`（会同时把测试证书导入本机信任存储）；
4. INF 与 sys 不在同一目录 → `pnputil /add-driver` 找不到 sys。

---

## 4. 控制面板“录制”选项卡：设备状态正常

| 步骤 | 操作 |
|---|---|
| 1 | `Win + R` → 输入 `control` → 回车（打开旧版控制面板） |
| 2 | **硬件和声音** → **声音** → **管理音频设备**（或直接 `mmsys.cpl`） |
| 3 | 切到 **“录制”** 选项卡 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 4.1 列表中出现 **MicLink Virtual Microphone** | 出现在设备列表里 | | ☐ |
| 4.2 设备状态 | 显示 **“设备已启用” / “准备就绪”**（不是“已禁用”、不是“未插入”） | | ☐ |
| 4.3 设为默认设备 | 右键 → **设为默认设备**（同时建议设为默认通信设备） | | ☐ |
| 4.4 属性 → 高级 | 默认格式为 **1 通道, 16 位, 48000 Hz (DVD 音质)**（或 44100） | | ☐ |
| 4.5 属性 → 级别 | 麦克风音量不为 0，且**未静音** | | ☐ |
| 4.6 对着手机说话时的电平条 | 有绿色电平跳动（说明有数据流入） | | ☐ |

> ⚠️ 4.6 在**还没启动服务端**时通常不动，这是正常的（没有 PCM 写入驱动）。
> 请在第 7 节启动服务端后再回来看这一项。

---

## 5. 设置 → 系统 → 声音 → 输入设备

| 步骤 | 操作 |
|---|---|
| 1 | `Win + I` → **系统** → **声音** |
| 2 | 向下找到 **“输入”** 区域（Windows 11）/“输入设备” |
| 3 | 点 **“选择要用于讲话或录制的设备”** 下拉 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 5.1 下拉里出现 **MicLink Virtual Microphone** | 出现在可选列表中 | | ☐ |
| 5.2 选中它作为输入设备 | 可成功选中 | | ☐ |
| 5.3 **测试麦克风** 的音量条 | 启动服务端并对着手机说话时，音量条**明显跳动** | | ☐ |
| 5.4 设备属性 → 输入音量 | 不为 0 | | ☐ |
| 5.5 Windows 11：所有声音设置 → 更多声音设置 | 能回到第 4 节的“录制”选项卡 | | ☐ |

---

## 6. 三个真实应用的验证步骤

### 6.1 ① Windows 录音机（Voice Recorder / 录音机）

> **预期**：录下的音频里能听到手机那边的声音。这是最基础的端到端验证。

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | 启动服务端：`miclinkserver.exe --port 5004 --rate 48000`，手机端填 PC 的 IP 后点“开始采集” | 服务端日志出现客户端接入、持续收包 |
| 2 | 打开 **录音机**（开始菜单搜“录音机”） | 打开即用 |
| 3 | 点录音机里的 **设置/设备**（部分版本在右上角 `...` → 设置） | 麦克风下拉里能看到 **MicLink Virtual Microphone**，选中它 |
| 4 | 点 **录制**，对着**手机**说话（数 1-2-3、念一段话） | 录制中 |
| 5 | 停止并保存，回放 | **能清楚听到刚才对手机说的话** |
| 6 | 再录一段：这次对着**PC 自带麦克风**说话 | 作为对照：说明录音确实来自手机而非 PC 麦 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 6.1.1 录音机能选中 MicLink 设备 | 可选中 | | ☐ |
| 6.1.2 回放能听到手机侧声音 | 清晰可辨 | | ☐ |
| 6.1.3 无明显爆音/断续 | 连续、无咔哒 | | ☐ |

### 6.2 ② OBS Studio

> **预期**：OBS 的混音器里能看到 MicLink 设备的电平条随手机侧说话跳动。

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | OBS → **设置** → **音频** → **全局音频设备** → **麦克风/辅助音频设备** | 下拉里出现 **MicLink Virtual Microphone** |
| 2 | 若无下拉项：主页 **来源** 面板 `+` → **音频输入采集** → 新建 → 设备选 **MicLink Virtual Microphone** | 添加成功 |
| 3 | 看 **混音器** 面板中该来源的电平条 | 静止时接近静音 |
| 4 | 对着手机说话 | **电平条明显跳动**（这是关键判据） |
| 5 | 点该来源的齿轮 → **属性** → 确认设备名；**高级音频属性** → 确认未被静音、音量 > 0 | 正确 |
| 6 | 开始录制 10 秒，停止后在文件里回放音轨 | 能听到手机侧声音 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 6.2.1 OBS 能列出 MicLink 设备 | 出现在设备下拉 | | ☐ |
| 6.2.2 混音器电平随手机侧声音跳动 | 跳动 | | ☐ |
| 6.2.3 录制文件里有手机侧声音 | 可听辨 | | ☐ |

> 排障：OBS 里设备显示为灰色/无电平 → 回第 4 节确认“录制”选项卡里状态是“准备就绪”，
> 并确认服务端正在运行且手机在发数据。

### 6.3 ③ Zoom

> **预期**：会议另一方能听到手机侧的声音。这是最有说服力的验证——**需要两个人配合**。

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | Zoom → 右上角头像/齿轮 → **设置** → **音频** | 打开音频设置页 |
| 2 | **扬声器** 区先随便选一个（如耳机），点“测试扬声器”确认能听到 | 有测试音 |
| 3 | **麦克风** 区 → 下拉选 **MicLink Virtual Microphone** | 可选中 |
| 4 | 看 **“输入电平”** 条：对着手机说话 | **电平条跳动**（Zoom 自带的电平条是很好的判据） |
| 5 | 点 **“测试麦克风”**（录制一小段后回放） | 回放能听到手机侧声音 |
| 6 | 取消勾选 **“自动调整麦克风音量”**，把滑块拉到 80% 左右 | 避免自动增益掩盖问题 |
| 7 | 加入一个真实会议（或用 Zoom 的“测试会议”），让对方确认 | **对方能听到手机侧说话** |
| 8 | 进阶：勾选“原始声音”（高级 → 音频）再测一次 | 排除 Zoom 自增益/降噪的干扰 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 6.3.1 Zoom 麦克风下拉能选到 MicLink | 可选 | | ☐ |
| 6.3.2 输入电平条随手机侧声音跳动 | 跳动 | | ☐ |
| 6.3.3 “测试麦克风”回放能听到 | 能 | | ☐ |
| 6.3.4 **会议对方确认能听到** | 能听清 | | ☐ |

> 腾讯会议 / Teams / 钉钉同理：**设置 → 音频 → 麦克风** 选 `MicLink Virtual Microphone`。
> 若某个应用选不到它，通常是该应用只枚举“通信设备”或做了缓存，尝试**重启该应用**。

---

## 7. 完整链路联调（服务端 + Android）

```powershell
# PC 端（先查 IP）
ipconfig                                  # 记下 IPv4，如 192.168.1.x
miclinkserver.exe --port 5004 --rate 48000 --discovery on
```

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | 手机打开 App，点 **“局域网发现服务端”** | 出现 `MICLINK-HERE ...` 解析出的服务端，IP 与端口自动回填 |
| 2 | （或手动）填 PC 的 IP，端口 5004，采样率 48000，帧长 20 ms，码率 64 kbps | — |
| 3 | 点 **开始采集**，授予麦克风与通知权限 | 状态变“采集中”，通知栏出现常驻通知 |
| 4 | 看 App 的电平条 | 对着手机说话时 RMS/峰值条跳动 |
| 5 | 看服务端日志 | 持续打印收包/电平/状态行；**若报“采样率链路致命错误”说明端点率与解码率不一致** |
| 6 | 息屏手机，继续说话 | 声音**不中断**（前台服务 + WakeLock 生效） |
| 7 | 切到其它 App / 回到桌面 | 采集持续 |
| 8 | 点通知栏的 **停止** | 采集停止，服务端日志显示客户端超时断开 |

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 7.1 发现功能可用（可选） | 能自动回填 IP | | ☐ |
| 7.2 服务端收到包 | 日志持续收包 | | ☐ |
| 7.3 息屏后不中断 | 持续有声音 | | ☐ |
| 7.4 回第 4 节 4.6 / 第 5 节 5.3 | 系统输入电平条跳动 | | ☐ |
| 7.5 手机端停止后 PC 端静音 | 电平归零 | | ☐ |

---

## 8. 反面检查：CreateFile 成功但列表里没有麦克风（**dispatch 覆盖坑**）

这是本项目最容易踩、也最难自查的一个坑，请**单独确认**。

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify.ps1
```

| 现象 | 含义 |
|---|---|
| `控制设备 \\.\MicLinkControl 可打开` = **PASS** | 说明**驱动已加载**，用户态能拿到句柄、能下发 IOCTL |
| 但第 4/5 节（录制选项卡 / 设置输入设备）里**看不到** `MicLink Virtual Microphone` | 说明**音频端点（KS filter / capture pin）没有被音频栈枚举到** |

即：**控制设备活着 ≠ 麦克风存在**。前者是 `MicLinkControl` 这个 `FILE_DEVICE_UNKNOWN` 设备对象，
后者是 KS（Kernel Streaming）过滤器 + WaveRT 微型端口暴露的捕获引脚，两条路径**彼此独立**。
能打开控制设备只能证明驱动加载成功，**不能证明端点被枚举**。

### 排查指引（按概率排序）

| # | 排查点 | 怎么看 / 怎么改 |
|---|---|---|
| 1 | **IRP_MJ_* dispatch 例程被覆盖** | 检查 `driver/Source/Main/` 中设备对象的 `MajorFunction[]` 赋值：KS 驱动的 `IRP_MJ_CREATE / CLOSE / DEVICE_CONTROL / PNP / POWER / SYSTEM_CONTROL` 必须由 KS 的分发函数（`KsDispatch*`）或正确转发的包装处理。<br>若 `\\.\MicLinkControl` 的 dispatch 表被“统一”套到了音频过滤器设备对象上，KS 的属性请求（如 `KSPROPERTY_PIN_*`）就到不了 miniport，端点不会被枚举。<br>**对照**：控制设备与适配器设备对象各自独立的 `MajorFunction` 赋值。 |
| 2 | **捕获引脚（capture pin）描述符与数据范围** | `driver/Source/Filters/micarraytopo.h / micarraywavtable.h`：确认存在 capture（不是只有 render）的 `KSPIN_DESCRIPTOR`，且 `KSDATARANGE` 里声明了 16-bit / 单声道 / 44100 与 48000。声明缺失 → 音频栈认为该过滤器没有可用捕获格式。 |
| 3 | **拓扑与波形过滤器的连接** | `driver/Source/Filters/minipairs.h`：确认 mic array 的 topology + wave 过滤器成对注册（`PCPIN_DESCRIPTOR` / `PCFILTER_DESCRIPTOR` 配对），且 `KSFILTER_DESCRIPTOR` 的 `Categories` 包含 `KSCATEGORY_CAPTURE` 与 `KSCATEGORY_AUDIO`。 |
| 4 | **PnP 接口未启用** | INF 的 `[MICLINK_VM.NT.Interfaces]` 段必须 `AddInterface` 注册 `KSCATEGORY_CAPTURE`（以及 `KSCATEGORY_AUDIO`）。接口没注册 → 音频栈看不到新设备。 |
| 5 | **设备接口类 GUID 与端点发现** | 音频端点需要被 `AudioEndpointBuilder` 服务识别。确认该服务在运行：`Get-Service AudioEndpointBuilder`；可尝试重启它，或重启系统。 |
| 6 | **驱动返回了错误的属性** | `minwavertstream.cpp` / `mintopo.cpp`：KS 属性处理返回 `STATUS_NOT_SUPPORTED` 会让枚举提前中止。用 `Tools\TraceView` 或 WPP 抓驱动日志（若已启用）看哪个属性失败。 |
| 7 | 设备管理器里在**“音频输入和输出”**而不是“声音、视频和游戏控制器” | 在两处都找一遍；端点设备名可能显示为 `MicLink Virtual Microphone` 的另一个实例。 |

### 现场记录（如遇到请填）

| 项 | 值 |
|---|---|
| `tools\verify.ps1` 中 `控制设备可打开` 结果 | |
| 设备管理器中出现在哪个类别 | |
| `pnputil /enum-devices /class Media` 输出中有无该设备 | |
| `Get-Service AudioEndpointBuilder` 状态 | |
| 驱动日志 / 事件查看器（`Windows 日志 → 系统`）中的相关错误 | |

---

## 9. 卸载与非残留检查

```powershell
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
# 按提示重启后再跑一次确认
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
```

| 检查项 | 预期 | 实际 | 通过 |
|---|---|---|---|
| 9.1 设备实例已移除 | `pnputil /enum-devices` 中无 `ROOT\MicLinkVirtualMic*` | | ☐ |
| 9.2 驱动包已删除 | `pnputil /enum-drivers` 中无 `MicLinkVirtualMic` | | ☐ |
| 9.3 驱动服务不存在 | `Get-Service MicLinkVirtualMic` 报找不到 | | ☐ |
| 9.4 注册表服务项已清理 | `HKLM:\SYSTEM\CurrentControlSet\Services\MicLinkVirtualMic` 不存在（通常重启后自动清） | | ☐ |
| 9.5 DriverStore 目录已清理 | `C:\Windows\System32\DriverStore\FileRepository\miclinkvirtualmic*` 不存在 | | ☐ |
| 9.6 声音设备无残留 | `Get-CimInstance Win32_SoundDevice` 中无 MicLink | | ☐ |
| 9.7 控制设备打不开 | `\\.\MicLinkControl` CreateFile 失败 | | ☐ |
| 9.8 输入设备列表里消失 | 设置 → 系统 → 声音 → 输入 中不再出现 | | ☐ |
| 9.9 测试签名可关闭（可选） | `bcdedit /set testsigning off` + 重启 → 水印消失 | | ☐ |

---

## 10. 失败处置：请把这些信息一起记录下来

任何一项不符，请记录：

1. Windows 版本（`winver`）、系统语言、x64 与否；
2. `tools\verify.ps1` 的完整输出；
3. 设备管理器的设备状态原文（含问题码）；
4. 服务端完整日志（用 `--log-level verbose` 再跑一次）；
5. Android 端状态栏文本与 logcat（`adb logcat -s miclink-capture miclink-opus`）；
6. 手机机型、Android 版本、实际生效的采样率（App 状态栏会显示）。

> 本文所有“预期结果”均为**设计推断**，**尚未在真实 Windows 上验证**。
> 请把实际结果填回本文档并提交 issue 或补丁，让后来者少踩一次坑。
