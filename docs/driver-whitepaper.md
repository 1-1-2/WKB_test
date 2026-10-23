# WOMic 驱动灌音协议白皮书（公开版）

> **状态**：已完成实证（2026-10）。全部结论来自对官方 `womic.sys`（2022 x64 版，2.1.0.0，51,192 B）的静态反汇编，以及在 GitHub-hosted Windows Runner 上的端到端实测。
> **用途**：完整还原"如何把 PCM 灌进 `\\.\WOMic` 虚拟麦克风"的驱动协议，供任何实现「WO Mic 协议兼容 PC 端」的开发者使用。
> **关联文档**：手机侧 WiFi 控制/媒体协议见 [`protocol/womic-wifi-protocol-v1.md`](protocol/womic-wifi-protocol-v1.md)；为什么复用官方驱动而非自研，见 [`adr/0001-driver-route.md`](adr/0001-driver-route.md)。

---

## 1. 结论速览

WOMic 虚拟声卡的"灌音"（把 PCM 送进驱动，让系统里的录音应用采到）走的不是普通读接口，而是一套
**「握手 + 环形缓冲」IOCTL 协议**。协议链只有五步：

```
CreateFile("\\.\WOMic")          # 每次 open 都会清零门控
→ IOCTL 0x222004（out 1B）       # 读 1 字节握手 key（驱动全局随机值）
→ IOCTL 0x222008（in key^0x3B）  # 校验通过 → 置门控
→ IOCTL 0x222014（in 2B 大端）   # 设环形缓冲容量（驱动内上限 0x2D00 = 11520）
→ IOCTL 0x222010（in 1B = 1）    # 启动（清空环形缓冲 + 置 streamOn）
→ IOCTL 0x222001（in 裸 PCM）    # 循环写入环形缓冲
```

要点：

- 音频格式固定 **48kHz / 16bit / 单声道**，每 20ms 一帧 = **1920 字节裸 PCM，无头部**。
- `0x222001` 是**防逆向设计**：恒定返回 Win32 `FALSE` + `GetLastError()==1`
  （`STATUS_NOT_IMPLEMENTED`），但写入实际生效。判定灌音是否成功
  **只能看采集端音频，不能看 IOCTL 返回码**。
- 驱动**只搬运字节、不做重采样**：喂 44.1kHz 的数据会按 48kHz 被播出（变调且不报错）。
- 实测：灌入 1kHz 正弦（幅度 12000）→ 采集端 `rms=8485`（理论值 12000/√2 ≈ 8485，完全吻合）、
  Goertzel 主峰 1000Hz、snr=735 —— **PASS**。

---

## 2. 环境矩阵（实证边界）

以下结论均在下列环境实证，换环境需重新验证：

| 项 | 值 |
|---|---|
| Runner | GitHub-hosted `windows-latest` |
| OS | Windows Server 2025 Datacenter，Build 26100 |
| 驱动 | 官方 2022 x64 套件（`Root\WOMic`，DriverVer 01/13/2022，2.1.0.0，WHCP 签名） |
| 设备路径 | `\\.\WOMic`（控制设备），wave 端点 `WOMicInWave` |
| 执行上下文 | SYSTEM 计划任务（Session 0）——交互会话下 WASAPI 初始化被拒，见 §7 |
| 采集判读 | WASAPI 共享模式 / waveIn，`GetMixFormat` = 48000Hz / 单声道 / 32-bit float |
| 完整回归 | 全新 runner 上 `装驱动 → 端点出现 → 握手 → 灌音 → 采集判频` 一次通过（run [39815350920](https://github.com/1-1-2/WKB_test/actions/runs/39815350920)） |

> 注：2017 版驱动（35,184 B，`*WOMic`）与 2022 版协议不同。社区流传的灌音 IOCTL 编号
> `0x222000` 只存在于 2017 版，2022 版上返回 `ERROR_INVALID_FUNCTION(1)`（51/51 全失败实证）。

---

## 3. 协议还原

### 3.1 握手链

```
CreateFile("\\.\WOMic")          # IRP_MJ_CREATE：清零门控字节
→ IOCTL 0x222004（out 1B）       # 读 1 字节 key（驱动全局随机值，每次 open 重新生成）
→ IOCTL 0x222008（in key^0x3B）  # 握手：input[0] == key ^ 0x3B 且 OutputBufferLength==1
                                #   通过 → 门控置 1；失败计数 +1 且全程只允许失败 1 次
→ IOCTL 0x222014（in 2B 大端）   # 设环形缓冲容量，驱动内上限 0x2D00 = 11520
→ IOCTL 0x222010（in 1B = 1）    # 启动：清空环形缓冲、置 streamOn（0 = 停止）
→ IOCTL 0x222001（in 裸 PCM）    # 写入环形缓冲（48k/16bit/mono，无头部）
```

### 3.2 关键细节（均有反汇编地址佐证）

| 事实 | 证据 |
|---|---|
| 环形缓冲结构 `[read u32][write u32][data]`，创建控制设备时按 `0x2D08` 一次性分配（tag `'CMOW'`） | `IoCreateDevice(type=0x22)` + `IoCreateSymbolicLink` 之后调用 |
| `0x222001` **永远返回 `STATUS_NOT_IMPLEMENTED`**（Win32 表现为 `FALSE` + `gle=1`） | 分发函数对 `sub 0x222001` 的默认分支无条件 `mov ebx, 0xC0000002` |
| 因此 IOCTL 全表面扫描会把 `0x222001` 误判为"不存在"——这正是早期扫描漏掉它的原因 | 扫描按 `gle != 1` 过滤 IOCTL 面 |
| 门控字节在每次 `IRP_MJ_CREATE` 被清零 | MajorFunction[0] 分支写 0 |
| 握手比较式：`input[0] == key ^ 0x3B`，且要求 `OutputBufferLength == 1` | 0x222008 处理分支 |
| `IRP_MJ_WRITE` 也能灌音（同一环形缓冲），但**要求 MDL**（即以 `FILE_FLAG_NO_BUFFERING` 打开），否则请求挂死 | Write handler 检查 `Irp->MdlAddress` → `MmGetSystemAddressForMdlSafe` → 同一写入函数 |
| 采集端（WASAPI/waveIn）拉取的就是这个环形缓冲 | 采样全零 ↔ 环空；灌音后立刻有信号 |

### 3.3 实测结果（SYSTEM / Session 0，waveIn 48kHz 单声道）

```
基线（不灌音，2s）      rms=0.5    peak=1
灌音（1kHz 正弦，5s）   rms=8485   peak=12001
Goertzel 主峰            1000 Hz    snr=735
结论                    PASS
```

`rms=8485` 与灌入幅度 12000 的正弦理论 RMS（12000/√2 ≈ 8485）**完全吻合**；
握手 key 每次打开由驱动重新生成（实测取到 `0x0D`、`0xB5` 两个值），客户端必须动态读取。

---

## 4. 防逆向行为与判读方法论

### 4.1 `0x222001` 恒定 FALSE + gle=1

驱动对灌音 IOCTL `0x222001` 的返回值是写死的 `STATUS_NOT_IMPLEMENTED`
（Win32：`DeviceIoControl` 返回 `FALSE`，`GetLastError()` 恒为 1），
**无论写入是否成功**。这是故意的防逆向设计：

- 让"扫 IOCTL 面"（过滤 `gle != 1`）的自动化探测漏掉真正的灌音口；
- 让"看返回码判成功"的直觉判断失效。

### 4.2 判读方法论：看效果，不看状态码

> **判定灌音是否成功，唯一可靠的标准是采集端是否出现预期音频。**

本项目采用的判读手段：

1. **灌入已知信号**：1kHz 正弦（或 440Hz 调试音），而非噪声或语音——可判频；
2. **采集端同期录音**：waveIn/WASAPI 同步录，按时间窗归因；
3. **Goertzel 算法判主频 + snr**：主峰落在灌入频率且 snr 足够高 → PASS；
4. **RMS / peak 与理论值对照**：正弦 RMS = 幅度/√2，可验证幅值保真度。

同理，IOCTL 全表面扫描（`dev=0x22, func=0x000–0xFFF, method∈{BUFFERED, IN_DIRECT, OUT_DIRECT}`）
命中过 5 个编号（`0x222004/0x222008/0x22200c/0x222010/0x222014`），但功能验证
（轮流灌正弦 + 同步录音）证明它们**全部是控制/查询类**——扫描只能告诉你"编号存在"，
不能告诉你"这是灌音口"。

---

## 5. 驱动行为矩阵（对客户端设计的硬约束）

以下四组实验（同一 runner、2022 驱动、SYSTEM/Session 0）构成客户端设计约束：

| # | 实验 | 结果 | 对客户端设计的约束 |
|---|---|---|---|
| E1 | `0x22200C` 语义 | 在 open / 握手 / 设容量 / 启动 / 灌音 / 停止六个阶段返回值恒为 3 | **不是状态查询**，无法用于"驱动是否就绪"自检；健康检查只能回到采集端 RMS |
| E2 | 双句柄门控 | h1 握手→灌音 rms=2545 ✅；打开 h2 后 h1 再灌音立刻变静音（rms=0.48）；h2 握手后又能灌（rms=2466） | **门控是全局的，任何新的 `CreateFile` 都会清零** → 客户端必须**强制单实例**；打开设备后立即完成握手，全程不得再开第二个句柄 |
| E3 | 采样率 | 灌 48k/1000Hz → 录到 1000Hz；灌 44.1k/1000Hz → 录到 1088Hz（= 1000 × 48/44.1） | 驱动**只搬运字节、不做重采样**。客户端必须解码成 **48kHz / 16bit / 单声道**再灌；喂 44.1k 会变调且不报错 |
| E4 | 容量档位 | 0x2D00(120ms) / 0x1680(60ms) / 0x0A00(26.7ms) 三档实测都能出声；0x0400(10.7ms) 失效（rms=0.48） | **延迟档位可用**：26ms（低延迟）/ 60ms / 120ms（稳健）三档可选，低于约 11ms 失效；环形缓冲满会静默截断 |

### 客户端设计约束汇总（写码前必读）

| # | 约束 | 依据 |
|---|---|---|
| 1 | 强制单实例（进程互斥 + 句柄不重复打开） | E2 |
| 2 | 打开设备后立即完成握手，中途不得再 `CreateFile` | E2 |
| 3 | 输出固定 48kHz / 16bit / 单声道裸 PCM，自行保证重采样 | E3 |
| 4 | 按 20ms 节奏自行推送，不可依赖 IOCTL 返回码（`0x222001` 恒定失败） | §4 |
| 5 | 环形缓冲满会静默截断，容量档位 26/60/120ms 三档可选 | E4 |
| 6 | 健康自检只能靠采集端 RMS，没有驱动级状态查询 | E1 |

---

## 6. 会话隔离实证（SYSTEM/Session 0 vs 交互会话）

同一份代码、同一台机器、同一个设备，仅会话上下文不同：

| 上下文 | WASAPI 共享模式 `Initialize` | 结论 |
|---|---|---|
| SYSTEM 计划任务（Session 0） | `0x0`（S_OK），5 秒采 498 包 / 239,040 帧 | **全链路 PASS** |
| upterm SSH 交互会话（Session 2，runneradmin） | `E_ACCESSDENIED (0x80070005)` | 失败（麦克风隐私开关为 Allow 也一样） |

- 无音源注入时采样**全零**（rms=0, peak=0, nonzero=0/239040），且 `SILENT` 标志从不置位
  ——与"采集链路成立、环空"的预期一致。
- **工程结论：一切音频探针必须由 SYSTEM 计划任务（Session 0）执行**。
  CI 里的做法：`schtasks /create /tn X /ru SYSTEM /rl HIGHEST /sc once /st 23:59 /tr 'cmd /c ...'` + `/run`；
  输出一律用绝对路径（相对路径会落进 `C:\Windows\System32`）。
- 另注：灌音（DeviceIoControl）本身在交互会话未被专门验证过是否需要提权——本白皮书的灌音实验
  均在 Session 0 完成。

---

## 7. 方法论：为什么静态反汇编赢了动态抓包

1. **能静态反汇编就不要动态抓包**：51KB 驱动 ≈ 8067 条指令，十几分钟读清全部语义；
   动态侧（Procmon + 客户端 GUI 自动化）受 GUI、权限、会话影响，成本高一个量级。
   （本次动态侧的最大教训：Procmon CSV 显示官方客户端 0 TCP / 0 UDP / 0 DeviceIoControl
   ——假手机端日志里的"TCP 已连接"其实是自己的健康检查污染，差点被误导。）
2. **反汇编不必在 Windows 上做**：`pefile` + `capstone` 是纯静态分析，Linux 直接跑；
   关键是拿到二进制（下载官方安装包，`7z x -aou` 保留多架构副本）。
3. **RVA 必须转文件偏移**（`pe.get_offset_from_rva`），否则反汇编输出为空。
4. **rip 相对寻址要手算目标地址**（下一条指令地址 ± displacement），才能把全局变量串起来
   （门控 / streamOn / g_ring 三个全局一旦对齐，协议就自洽了）。
5. **状态码会骗人**：驱动为防逆向故意返回 NOT_IMPLEMENTED 但实际执行写入；
   判定必须看"效果"（采集端音频），不能看返回值。
6. 入口常是 thunk：本驱动 EP 只是 `call/jmp` 到真实 DriverEntry，深挖要从那里继续；
   MajorFunction 表在 x64 的 `DRIVER_OBJECT+0x70`（28 项 × 8B），重点看
   `IRP_MJ_CREATE(0)` / `IRP_MJ_WRITE(4)` / `IRP_MJ_DEVICE_CONTROL(14)`。

---

## 8. 证据索引

| 论断 | 来源 |
|---|---|
| 协议链五步 + 全局变量（门控/streamOn/g_ring） | `womic_1.sys` 静态反汇编（ImageBase 0x140000000，真实 DriverEntry 经 thunk 定位） |
| `0x222001` 恒定 NOT_IMPLEMENTED | 分发函数默认分支无条件 `mov ebx, 0xC0000002` |
| 灌音 PASS（rms/peak/Goertzel） | run [39815350920](https://github.com/1-1-2/WKB_test/actions/runs/39815350920) 完整回归 |
| 会话隔离（Session 0 vs Session 2） | WASAPI 独立采集对照实验（同代码同设备） |
| `0x222000` 不存在于 2022 驱动 | IOCTL 灌音 51/51 失败，首个错误码 `ERROR_INVALID_FUNCTION(1)` |
| 行为矩阵 E1–E4 | 同一 runner 上的四组对照实验 |
| 手机侧协议（TCP 8125 + UDP/Opus） | [`protocol/womic-wifi-protocol-v1.md`](protocol/womic-wifi-protocol-v1.md) |
