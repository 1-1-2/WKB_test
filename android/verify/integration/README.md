# android/verify/integration —— 协议闭环联调（PC 客户端脚本 + CI 作业）

本目录是**自写 app 与 WO Mic 协议的端到端联调**入口：`pc_client.py` 扮演官方
Windows 客户端 6.3 的 PC 角色（TCP client + UDP receiver），与 app（TCP server +
UDP sender）跑完整会话闭环。

协议唯一事实来源：`docs/protocol/womic-wifi-protocol-v1.md`。
本目录所有断言逐条对应规格 v1 第 2/3 节与官方 6.3 反汇编实证。

## 文件

| 文件 | 作用 |
|---|---|
| `pc_client.py` | 联调脚本。纯 Python 标准库（无第三方依赖），CI 与本机均可跑 |
| `test_pc_client.py` | 离线单测（mock adb / mock Popen，不连设备）：bind 就绪探测、tap 死亡监控、C05 restart 自救、诊断降噪、exec-out 通道（PTY/ONLCR 根因）回归 |

## 运行方式

### CI（推荐，无需本机准备）

推送即触发 `.github/workflows/android.yml` 的 `integration-test` 作业
（ubuntu-latest + KVM 模拟器）：装 SDK → 下载 opus 源码 → 构建 APK
（`-PwomicAbis=x86_64`）→ 建 AVD → 起模拟器 → 装 app → 启动服务 →
跑本脚本 → 失败时上传 logcat/emulator 日志。也可手动触发：
`gh workflow run android.yml`。

### 本机（真机 / 模拟器）

前置：app 已安装、服务已启动（调试音模式即可，无需真麦克风）：

```bash
# 模拟器（adb forward 拓扑，默认）
adb shell am start -n com.miclink.open/.MainActivity
adb shell am start-service -n com.miclink.open/.MicCaptureService \
    -a com.miclink.open.action.START \
    --ei extra.control_port 8125 --ei extra.volume 5 --ez extra.debug_tone true
python3 pc_client.py --hold-seconds 12

# 真机同 IP 段（不走 adb forward）：PC 直接连手机 IP
python3 pc_client.py --control-host 192.168.x.x --control-port 8125 \
    --udp-tap host --media-port 5004
```

自测（不连设备，验证解析/断言逻辑本身）：

```bash
python3 pc_client.py --self-test
```

## 网络拓扑

默认 **adb forward 模式**：脚本把本机 `127.0.0.1:18125` forward 到设备
`8125`（app 的 TCP 控制端口）。关键点：**adb forward 下 app 看到的 TCP 对端
是设备侧 `127.0.0.1`**（UdpSender 的目标地址取自 TCP 对端），因此 app 的
UDP 媒体流发往**设备自己的 loopback**——必须在模拟器内用
`toybox nc -u -l -p 5004` 回收，字节流经 `adb exec-out`（无 PTY、不经
ONLCR，见下节"C05 的真正根因"）回传脚本（格式 C 自定长：`total = len + 4`，
可流式重分帧，详见 `FormatCParser`）。

`--udp-tap host` 模式面向**直连拓扑**（如 host 直连模拟器 slirp 地址
`10.0.2.15:8125`）：此时 app 的 TCP 对端是 `10.0.2.2`，UDP 直接发到 host
loopback，脚本在 host 侧用 UDP socket 回收即可。

> **nc 的生命周期（三个实测教训）**：
> 1. 必须在 CONFIG/START 之前启动：app 的 UDP socket 未 connect，虽然 Linux 不会
>    把 ICMP port-unreachable 上报给发送方，但监听就绪再 START 更接近真实场景
>    （真实 PC 客户端总是先占住媒体端口）。
> 2. **重连会话必须换一个新监听**（脚本自动做，`AdbUdpTap.restart()`）：toybox
>    `nc -u -l` 收到首个数据报后会 `xconnect()` 到该对端（netcat.c:229），之后
>    内核只交付该对端的包；app 每个会话新建 DatagramSocket（新源端口），旧监听
>    会丢掉第二会话的全部包（run 39473353065 实证 C15 udp=none）。
> 3. **"启动了"不等于"bind 成功了"**：必须以 `/proc/net/udp(6)` 出现媒体端口
>    hex 监听行为准（脚本自动做，见下节"已知故障模式与自愈"）。`sleep(0.6)` +
>    `poll()` 只能证明 adb 进程活着，adb fork 慢时会误判（run 40049236444
>    实证，C05 两次 attempt 全红的根因）。

## 已知故障模式与自愈（**C05 红轮 ≠ 代码 bug**）

**C05（`udp_first_packet_format_c`）失败不代表 app 有 bug。** 偶发失败的
真正根因是 harness 侧 UDP tap 的**回传通道被 PTY 破坏**（2026-10-24 沙箱
实验钉死）：

- `adb shell` 会在设备端为命令分配 **PTY**，PTY 默认开启 **ONLCR**：输出
  中的每个 `0x0A` 字节被改写成 `0x0D 0x0A`（一个字节变两个，后续内容全部
  错位）；
- Opus 是二进制流：单个 160 字节帧不含 `0x0A` 的概率仅约
  (255/256)^160 ≈ **53%**——即约 **47% 的首包必然含 `0x0A`**，帧的长度
  前缀与实际内容错位，`FormatCParser` 解析失败；
- 沙箱实测（host `nc -u -l` + python pty，发包 `0a 0d 41 0a 42 00 ff`）：
  PTY 后 `0d0a0d410d0a4200ff`（字节数增加、内容错位），PIPE 后
  `0a0d410a4200ff`（字节一致）。

这解释了历史上所有的"C05 抖动"：10-21 全绿、10-23 连续红、10-24 一红一绿
——同代码一红一绿，**与 android/ 的代码改动毫无关系**（UI 改造是巧合，
不是原因）；"链路抖动""模拟器麦克风无声"等解释均为错误方向。

> CI 全程调试音模式（`--ez extra.debug_tone true`，**不开 AudioRecord**），
> "模拟器麦克风无声"是**错误**解释，不要往这个方向排查。

**修复（2026-10-24）**：tap 的 adb 调用从 `shell` 改为 **`exec-out`**（官方
为二进制流设计的通道，不在设备端分配 PTY，字节原样回传）。文本命令
（`ps` / `cat /proc/net/udp` / `pkill`）仍走 `shell`：其输出按空白分词，
ONLCR 附加的 `\r` 无害，且不扩大验证面。

**回归判据（旧实现红 / 新实现绿）**：

| 用例 | 位置 | 断言 |
|---|---|---|
| tap 走 exec-out 而非 shell | `test_pc_client.py` `ExecOutChannelTest.test_tap_starts_with_exec_out_not_shell` / `run-tests.sh` T20 | Popen argv / stub 记录的子命令必须是 `exec-out`；nc 绝不走 shell；文本命令仍走 shell |
| ONLCR 端到端毁帧 | `test_pc_client.py` `ExecOutChannelTest.test_pty_channel_destroys_frame_exec_out_survives` / `run-tests.sh` T21 | 假通道按 argv 决定是否开 ONLCR：喂含 `0x0A` 的合法格式 C 帧，exec-out 必须字节级还原；通道若是 PTY（旧实现）必毁帧 |
| parser 级机制证明 | `test_pc_client.py` `ExecOutChannelTest.test_parser_on_onlcr_channel_fails_on_raw_channel_succeeds` | ONLCR 假通道下 parser 毁帧（内容级错位 + 长度前缀被读坏时直接等不齐字节），原样通道下字节级还原 |

`_start_process()` 一旦回退到 `shell`，前两类用例立即转红（本地已实测：
旧实现副本上 T20c/T20d/T21a/T21b 与两个 unittest 转红，新实现全绿）。

> 历史上还有一个**已修复**的 harness 缺陷（与本根因相互独立，勿再混为一谈）：
> tap 启动竞态——`sleep(0.6)` + `poll()` 只能证明 adb 进程活着，不能证明
> nc 已 bind 媒体端口，shell fork 慢时会误判"启动成功"随后 tap 静默死亡
> （run 40049236444）。已由 bind 就绪探测（`/proc/net/udp(6)` 轮询）修复，
> 见下节。两个缺陷都只会让 C05 假红，都不是 app 问题。

### 内置的自愈与定向手段（`pc_client.py`）

| # | 手段 | 落点 |
|---|---|---|
| 1 | **bind 就绪探测**：启动 nc 后轮询 `/proc/net/udp(6)`，出现媒体端口 hex 监听行（5004 → `138C`）且 nc 进程存活才算就绪；累计 10s 未就绪 → 换候选命令/抛 `EnvError`（exit=2 环境错误，不混为断言失败 exit=1） | `AdbUdpTap._start_process()` / `_wait_bind_ready()` |
| 2 | **tap 死亡监控**：`_pump` 读到 EOF（nc 退出）即置线程安全标志并记录退出码；C05 失败信息按 tap 健康态定向——tap 已死 = "harness 问题，不是 app 问题"；tap 活着没收到 = "app 发包但 tap 未收到" | `AdbUdpTap._pump()` / `_mark_dead()` / `describe_tap_health()` |
| 3 | **C05 失败 restart 自救**：C05 未收到首包时，先 restart tap（换一个全新的、未 connect 的 nc 监听，toybox connect-to-first-peer 语义要求新 socket）再重试首包等待（默认 2 次，`--c05-tap-retries` 可调），比重跑整个脚本更快更对症；workflow 层（`.github/workflows/android.yml`）的整脚本重试**保留**为双保险 | `c05_wait_first_packet()` |
| 4 | **诊断降噪**：C05 现场抓取用 `ps -A \| grep -E '(^| )nc( \|$)\|nc -u'` 精确匹配 nc 进程（裸 `grep nc` 会被 incidentd/gnss-service/launcher3 等含 "nc" 子串的系统进程淹没），并同时查 `/proc/net/udp` 与 `/proc/net/udp6`（端口 hex 大小写不敏感） | `capture_c05_diagnostics()` / `filter_nc_ps_lines()` / `parse_udp_listen_lines()` |

离线回归（不连设备）：

```bash
python3 pc_client.py --self-test      # 9/9
python3 test_pc_client.py             # mock adb 单测 29 项（含 exec-out/ONLCR 根因回归）
bash ../../../.github/scripts/tests/run-tests.sh   # CI 脚本单测（T14~T21，含 exec-out 回归）
```

## 断言清单（C01–C15）

| # | 断言 | 规格依据 |
|---|---|---|
| C01 | TCP 控制通道可连接 | §1 角色拓扑 |
| C02 | hello 应答 = `65 00000002 00 04`（status=0, level=4） | §2.2/§2.3 |
| C03 | config 应答 = `66 00000001 00` | §2.2 |
| C04 | start 应答 = `67 00000001 00` | §2.2 |
| C05 | 首包格式 C 字节级自洽（magic 04 00 / len=payload+7 / total=len+4 / vol=5） | §3.3 |
| C06 | seq 逐帧 +1（u16 回绕感知） | §3.3 |
| C07 | ts 单调递增（elapsedRealtime，u32 回绕感知） | §3.3 |
| C08 | 音量字段恒为 5（单位增益） | §3.3/§4 |
| C09 | 全部包结构合法 | §3.3 |
| C10 | 推流速率 ≥ 15 包/秒（20ms/帧 → ~50/s） | §3.4 |
| C11 | 心跳跨过 5s 看门狗，连接与推流持续 | §2.5（V1/k.java） |
| C12 | **stop 应答是 type 102 帧**（官方 d2/e 无参构造怪癖） | §2.2 |
| C13 | 停止后静默（无残留包） | §2.6 断链清理 |
| C14 | 停心跳后 5~10s 内被看门狗断链 | §2.5 |
| C15 | 断链后重连，完整会话再来一轮全绿 | §2.5/§2.6 |

## 与其它验证层的关系

```
:core:test（23 用例，字节级向量）      本目录（真协议闭环，网络/线程/时序）
        ▲                                        ▲
   协议编解码正确性                        端到端行为正确性
```

`:core:test` 证明"字节编对了"，本目录证明"app 在设备上真的把流推出来了"。
两者互补，都不能替代对方。CI 中 `protocol-tests` 作业跑前者，
`integration-test` 作业跑后者。

## 验证状态

| 层级 | 状态 | 证据 |
|---|---|---|
| `--self-test`（9 项，合成字节流） | 全绿 | 本机 `python3 pc_client.py --self-test` |
| mock adb 单测（29 项，`test_pc_client.py`） | 全绿 | 本机 `python3 test_pc_client.py`（覆盖 bind 探测两个 CI 实证场景 + exec-out/ONLCR 根因回归；顺带修复 `MockAdb.shell` 返回 None 与 `_probe_udp_listen` 需要 proc 对象的脱节——该脱节曾让 8 个用例 error，README 旧版"26 项全绿"为过期声明） |
| CI 脚本单测（63 项，`.github/scripts/tests/run-tests.sh`） | 全绿 | 本机 `bash .github/scripts/tests/run-tests.sh`（T14~T21；T20/T21 为 exec-out 回归：旧实现红、新实现绿） |
| mock server 干跑（15/15） | 全绿 | 本机，`docs/VERIFICATION.md` A4c(2) |
| **CI 模拟器真 app 闭环（C01–C15）** | 待重跑（exec-out 修复后） | 历史全绿 run [39523580260](https://github.com/1-1-2/WKB_test/actions/runs/39523580260)（2026-10-21，彼时 C05 约 47% 概率被 PTY/ONLCR 毁帧，属 harness 缺陷而非 app 缺陷） |
| 真机（物理手机）联调 | 待执行 | — |
| 真实麦克风采集（非调试音模式） | 待执行 | — |
