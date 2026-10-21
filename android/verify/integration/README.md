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
`toybox nc -u -l -p 5004` 回收，字节流经 `adb shell` 的 stdout 回传脚本
（格式 C 自定长：`total = len + 4`，可流式重分帧，详见
`FormatCParser`）。

`--udp-tap host` 模式面向**直连拓扑**（如 host 直连模拟器 slirp 地址
`10.0.2.15:8125`）：此时 app 的 TCP 对端是 `10.0.2.2`，UDP 直接发到 host
loopback，脚本在 host 侧用 UDP socket 回收即可。

> **nc 的生命周期（两个实测教训）**：
> 1. 必须在 CONFIG/START 之前启动：app 的 UDP socket 未 connect，虽然 Linux 不会
>    把 ICMP port-unreachable 上报给发送方，但监听就绪再 START 更接近真实场景
>    （真实 PC 客户端总是先占住媒体端口）。
> 2. **重连会话必须换一个新监听**（脚本自动做，`AdbUdpTap.restart()`）：toybox
>    `nc -u -l` 收到首个数据报后会 `xconnect()` 到该对端（netcat.c:229），之后
>    内核只交付该对端的包；app 每个会话新建 DatagramSocket（新源端口），旧监听
>    会丢掉第二会话的全部包（run 39473353065 实证 C15 udp=none）。

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
| mock server 干跑（15/15） | 全绿 | 本机，`docs/VERIFICATION.md` A4c(2) |
| **CI 模拟器真 app 闭环（C01–C15）** | **全绿 15/15** | run [39523580260](https://github.com/1-1-2/WKB_test/actions/runs/39523580260)（2026-10-21T00:58:36Z，commit `58ab0f0`，job 118975309559） |
| 真机（物理手机）联调 | 待执行 | — |
| 真实麦克风采集（非调试音模式） | 待执行 | — |
