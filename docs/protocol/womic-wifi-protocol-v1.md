# WO Mic WiFi 协议规格 v1（黑盒逆向 + 双端交叉验证）

> **状态**：v1（2026-10-20）。证据来源 = 官方 Android APK 4.8 反编译（jadx 1.5.1）+ 官方 Windows Client 6.3 反汇编（objdump）+ 20kdc 第三方 Wireshark trace 佐证。
> **用途**：本项目自写 Android 采集客户端（扮演"手机端 server + UDP 发送方"）与官方 Windows Client 6.3（"PC 端 client + UDP 接收方"）对接。
> **证据目录**：`docs/protocol/evidence/`（35 个反编译源文件，与下文引用一一对应）。原始产物：APK `app-release-4_8.apk`（com.wo.voice2 4.8）、`WOMicClientSetup6.3.exe`（NSIS 内嵌 `WOMicClient.exe` + 签名驱动 `womic.sys`）。

## 0. 证据分级约定

| 标记 | 含义 |
|---|---|
| 【已证实】 | APK 反编译源码或 PC 客户端反汇编指令直接可读，或两端互证 |
| 【已证实-佐证】 | 第三方 trace 与官方源码一致（如 20kdc 笔记） |
| 【待实测】 | 需用户真机 + Wireshark golden trace 确认（见 §7） |

---

## 1. 角色与拓扑【已证实】

```
┌────────────────────┐   TCP 8125（控制）   ┌────────────────────┐
│  官方 Windows      │ ◄──────────────────► │  手机（本项目 app） │
│  Client 6.3        │   手机=server        │  = TCP server       │
│  = TCP client      │    PC=client         │  = UDP sender       │
│  = UDP receiver    │ ◄──────────────────► │                    │
└────────────────────┘   UDP（Opus 单向）   └────────────────────┘
                             手机→PC，PC 不回 UDP
```

- 手机端 `e2/j`（WiFi 服务端）：`new ServerSocket(port)` + `accept()`，持 `WifiLock`（mode 3 = WIFI_MODE_FULL_HIGH_PERF）。证据：`evidence/e2/j.java:31-52`。
- PC 端为连接发起方。证据：`evidence/e2/j.java` accept 循环由 `D0/a` case 13 驱动（`f1143h.b()` → `h(2,0,conn)`）。
- UDP **单向**：手机端媒体通道类 `e2/h` 仅有 `send()` 与 `close()`，无 receive。证据：`evidence/e2/h.java:20-36`。
- 手机端 UDP socket 为**本地临时端口 + connect(PC 地址, PC 媒体端口)**。证据：`evidence/e2/f.java` 方法 `c(I)` 中 `new DatagramSocket()` → `connect(getInetAddress(), port)`。

## 2. TCP 控制协议

### 2.1 帧格式【已证实】

```
[type: u8][length: u32 大端][payload: length 字节]
```

- 写入：`AbstractC0175c.e0()` = `DataOutputStream.write(type)` + `writeInt(length)`（DataOutputStream 即大端）。证据：`evidence/b2/AbstractC0175c.java`。
- PC 客户端构造同序：`movb $0x65` 后连写 4 个长度字节（`00 00 00 06`）。证据：WOMicClient.exe 0x4147be~0x4147fa。

### 2.2 消息总表【已证实】

| type | 方向 | 负载 | 语义 | 手机端处理 | PC 端行为 |
|---|---|---|---|---|---|
| 101 (0x65) | PC→手机 | 6B（见 §2.3） |  hello/版本协商 | `d2/a` | 6.3 客户端发 `65 00000006 04 06 03 00 00 00` |
| 101 | 手机→PC | 2B：`[status:u8][4:u8]` | hello 应答 | `d2/b`，status=0 成功 / 10=variant<2 | 校验应答第 2 字节==4 |
| 102 (0x66) | PC→手机 | 6B：`[codec:u8][rateIdx:u8][mediaPort:u32 BE]` | 音频参数+PC 媒体端口 | `d2/d` | 6.3 客户端发 `66 00000006 02 02 <port BE>` |
| 102 | 手机→PC | 1B：`[status:u8]` | 参数应答 | `d2/e(0,0)`=成功；12=codec 非法；13=rateIdx 非法 | 读 1 字节状态 |
| 103 (0x67) | PC→手机 | 0B | 开始推流 | `d2/f`：建 UDP 通道 connect(PC, mediaPort)，启动音频线程 | 6.3 客户端发 `67 00000000` |
| 103 | 手机→PC | 1B：`[0]` | 推流已启动 | `d2/e(0,1)` | 读应答 |
| 104 (0x68) | PC→手机 | 0B | 停止推流 | `d2/g`：停音频线程+UDP 通道 | — |
| 104 | 手机→PC | 1B：`[0]` | 已停止 | `d2.e(0, 默认type=104)` | — |
| 105 (0x69) | PC→手机 | 0B | 心跳（约 1s/次） | `d2/c`：置位 `f1142g` | **无应答**（发送后直接返回） |
| 0 | 手机→PC | 变长 | 错误串 | `b2.b`：`00 <len:u32> 01 <ascii>` | — |
| 1/2/3/10 | PC→手机 | 变长 | **旧版遗留**，4.8 app 不识别 → 回 "Unknown req" 错误 | `c2.a` | — |

关键证据：
- 消息分派与处理：`evidence/V1/l.java:42-149`（`a(e2.f)` 主循环）。
- 各消息类字段定义：`evidence/d2/a.java`（101 仅读 payload[0]）、`evidence/d2/d.java`（102 读 codec/rateIdx/port）、`evidence/d2/f.java`/`evidence/d2/g.java`/`evidence/d2/c.java`（103/104/105 仅跳过负载）。
- 应答写码：`evidence/d2/b.java`（101 应答）、`evidence/d2/e.java`（102/103/104 应答，按 f12092q 选 type）。
- PC 端 102 端口大端序：WOMicClient.exe 0x414a56~0x414b05（`shr $0x18/$0x10/$0x8/0` 逐字节）。
- PC 端 103/105 构造：WOMicClient.exe 0x414ce5（`67 00 00 00 00`）、0x414f05（`69 00 00 00 00`，send 后无 recv）。
- 心跳无应答：`evidence/V1/l.java:136-139`（case 105 → `new C0173a()`，`d0()` 为空）。证据：`evidence/b2/C0173a.java`。

### 2.3 101 报文负载详解【已证实+佐证】

PC 客户端 6.3 实际发送（反汇编实证）：`04 06 03 00 00 00`
- `payload[0] = 04`：**UDP 头格式版本号**（唯一被手机端读取的字节，`d2/a.f12085q`）。取值必须 ≥2，否则手机端回 status=10。映射：2→头格式 A（§3.1）、3→头格式 B（§3.2）、4→头格式 C（§3.3）。
- `payload[1..5] = 06 03 00 00 00`：手机端**跳过不读**（`d2/a.c0` 中 5 个 `readByte()` 后 `skipBytes(len-6)`）。含义未知（疑为客户端版本/特性标志），**兼容策略：原样复刻官方值**。
- 佐证：20kdc trace 为 `04 04 06 02 00 00`（旧版客户端），首字节同为 4 —— 与"仅首字节有效"的源码结论自洽。

### 2.4 102 报文参数取值【已证实】

| 字段 | 取值 | 含义 |
|---|---|---|
| codec | 2 | Opus（`OpusEncoder`，libcodec-o.so） |
| codec | 100 | Speex（`s1.i(23)`，libcodec-s.so） |
| codec | 其他 | 手机端回 status=12 |
| rateIdx | 0 / 1 / 2 | 采样率 8000 / 16000 / **48000** Hz |
| rateIdx | 其他 | 手机端回 status=13 |
| mediaPort | u32 大端 | **PC 侧 UDP 媒体端口**；手机端随即以此端口 connect UDP socket |

证据：`evidence/V1/l.java:97-121`（`iArr = {8000, 16000, 48000}`）、`evidence/V1/m.java:44-57`（codec 2→OpusEncoder，100→Speex）。
官方客户端默认：codec=2（Opus）、rateIdx=2（48000）。证据：WOMicClient.exe 0x414aa1/0x414ab0（`movb $0x2` 两次）。

### 2.5 会话状态机【已证实】

手机端 `V1/n` 状态字段 `f1138a`：

```
0 空闲 ──w(0,启动)──► 1 启动中 ──listen成功──► 2 监听中
   ▲                                          │ accept连接 (w(2,conn))
   │                                          ▼
   │                                        3 已连接（协议线程 V1/l 运行）
   │                    ┌── 103 开始推流 ──► 3+流（音频线程 V1/m + 5s 看门狗）
   └── 停止/错误 ◄─────┘（104 或异常）
```

- 音频线程启动：`V1/l` case 103 → `f1153r.h(10, 0, f1144i.c(f1152q))` → `A/g.w(10,...)` 起 `V1/m` + `Timer(5000ms)` 跑 `V1/k`。证据：`evidence/V1/l.java:122-130`、`evidence/A/g.java:389-402`。
- **5 秒心跳看门狗**：`V1/k` 每 5s 检查 `f1142g`，未被 105 置位则 `g(50)` 强制断链。证据：`evidence/V1/k.java`。→ **自写 app 必须实现同一看门狗**（PC 端约 1s 发一次 105，5s 为上限）。
- 状态广播：`VoiceService.a(state, reason)` 发 `com.wo.voice.VoiceService.ACTION_STATE` intent。证据：`evidence/com/wo/voice2/VoiceService.java:39-61`。

### 2.6 断链清理【已证实】

`V1/n.b()`：停音频线程 → 停 Timer → `c(2,0)`（回状态 2）→ 关 UDP 通道 → 关 TCP 连接。证据：`evidence/V1/n.java:80-100`。TCP 连接设 `setKeepAlive(true)`。证据：`evidence/e2/f.java`（`<init>(Le2/g; Socket)` 分支）。

---

## 3. UDP 媒体协议

三种头格式，由 101 报文 `payload[0]` 选定。**官方客户端 6.3 使用格式 C（variant=4）**。

### 3.1 格式 A（variant=2，`s1/i` case 25）【已证实】

```
[seq: u32 BE][len: u16 BE][Opus 负载]
```
无魔数。证据：`evidence/s1/i.java:237-256`。

### 3.2 格式 B（variant=3，`s1/i` case 26）【已证实】

```
[xor: u8][00: u8][seq: u16 BE][len: u16 BE][Opus 负载]
```
`xor` = 负载所有字节异或。证据：`evidence/s1/i.java:257-282`。

### 3.3 格式 C（variant=4，default 分支）【已证实】★本项目采用

```
偏移  字段            宽度    值/语义
00   魔数              2B     固定 04 00
02   长度              2B BE  = Opus负载长 + 7（含 seq2+ts4+vol1）
04   序列号            2B BE  每帧 +1（u16 回绕）
06   时间戳            4B BE  SystemClock.elapsedRealtime() 毫秒（单调时钟）
0A   音量/衰减档位     1B     5 = 原始音量（官方默认）；0=静音; 1-4=右移衰减; 6-9=左移放大
0B   Opus 负载         变长   单个完整 Opus 包
```

证据：`evidence/s1/i.java:283-304`（`writeByte(4); writeByte(0); writeShort(d5+7); writeShort(seq); writeInt(ts); writeByte(vol)`）。
佐证：20kdc trace 记录 `04 00`/len16/seq16/未知32bit 递增/固定 `05` —— 与源码逐字段吻合；"未知 32bit"实为毫秒时间戳（故"递增不规则"）。
音量语义：`evidence/X1/a.java`（`a(int)` 方法：0=清零、1-4=>>1..>>4、6-9=<<1..<<4，5=原样）。默认 5 来源：`evidence/V1/f.java`（`volume` 默认 5）→ `VoiceService.onCreate` 传入 `f1148m`。

### 3.4 帧参数【已证实】

- 帧长 20ms（`V1/m` 中 `opusEncoder.f11818g = 20` 硬编码；`OpusEncoder.a()` 亦允许 40ms）。证据：`evidence/V1/m.java:50`、`evidence/com/wo/voice2/audio/OpusEncoder.java`。
- 48kHz 单声道 16bit：`AudioRecord(src, rate, CHANNEL_IN_MONO=16, ENCODING_PCM_16BIT=2, bufSize)`。证据：`evidence/X1/b.java`（`c(int)`）。
- 采集缓冲 = 10 帧（`(frameMs*rate/1000)*10`），队列深 5。证据：`evidence/X1/b.java`、`evidence/V1/m.java:38`。
- 设备不支持目标采样率时回退 44100 并重采样（`MutableResampler`）。证据：`evidence/V1/m.java:31-35`、`evidence/com/wo/voice2/audio/a.java`。
- AEC/NS 可用即启用。证据：`evidence/X1/b.java`（`c(int)` 尾部）。

---

## 4. 默认值与配置【已证实】

| 配置项 | SharedPreferences 键 | 默认值 | 证据 |
|---|---|---|---|
| 传输方式 | `transport` | 2（WiFi） | `evidence/V1/f.java` |
| **TCP 控制端口** | `controlPort` | **8125** | `evidence/V1/f.java` |
| 音量 | `volume` | 5（原样） | `evidence/V1/f.java` |
| 音频源 | `audioSource` | 0（DEFAULT） | `evidence/V1/f.java` |
| 自动静音 | `autoMute` | false | `evidence/V1/f.java` |
| PC 侧 UDP 媒体端口 | —（102 报文传入） | WikiHow 文档称 8126，【待实测】 | `evidence/V1/l.java:124` |

注：dex 全量字符串扫描确认 **8125/8126 未硬编码于 dex**（端口全部经 settings 与 102 报文流转）。PC 客户端 exe 中亦无 8125/8126 常量（默认值来源不在二进制内，疑为 UI/配置文件默认）。

## 5. 官方 Windows 客户端 6.3 行为实证（objdump）

| 行为 | 地址 | 实证 |
|---|---|---|
| 101 构造 `65 00000006 04 06 03 00 00 00` | 0x4147be~0x414860 | 11 字节顺序写入后 send |
| 101 应答校验 | 0x4148ae | `cmp $0x65`（type 必须 101）；应答第 2 字节须为 4 |
| 102 构造 `66 00000006 02 02 <port:u32 BE>` | 0x414a56~0x414b05 | 端口大端 4 字节 |
| 103 构造 `67 00000000` | 0x414ce5 | 5 字节 |
| 105 构造 `69 00000000` 且无 recv | 0x414f05~0x414f5f | send 后 `xor eax,eax; jmp` 返回 |
| 日志串 | — | "Connecting via Wifi. Host: %s, control port: %d, media port: %d"；"UDP port %d is not available" |

## 6. 官方驱动（随 6.3 安装器分发）【已证实】

- `womic.sys`（35,696B，2017-11-25）+ `womic.inf` + `womic.cat` + `devcon.exe`，NSIS 内 `driver/` 目录。
- **Authenticode 签名在位**：证书表 offset 0x6800 size 0x2370；签名者 **Beijing Wolicheng Technology Co., Ltd.**；链 = DigiCert High Assurance EV Root CA → DigiCert EV Code Signing CA (SHA2) → Microsoft Code Verification Root。
- INF：`Class=MEDIA`，ClassGUID `{4d36e96c-e325-11ce-bfc1-08002be10318}`，DriverVer 05/06/2017 **1.5.0.0**，设备 `WOVAD_MicArray`，硬件 ID `*WOMic`，wave 采集接口（KSCATEGORY_AUDIO/CAPTURE）。
- 意义：EV 签名的内核驱动在 Win10 1607+ **免测试模式直接加载** —— 本方向"开箱即用安装"的前提成立。x64 only（`ntAMD64`）。

## 7. 待实测清单（Phase 1 真机 + Wireshark）

| # | 项 | 验证方法 |
|---|---|---|
| T1 | PC 客户端默认 media port（疑 8126） | PC 客户端 UI media port 截图 + trace 中 UDP 目的端口 |
| T2 | 101 应答第 2 字节（假定恒为 4） | trace 抓 `65 00000002 00 04` |
| T3 | 105 心跳实际间隔（假定 ~1s） | trace 中 105 帧时间差 |
| T4 | 48kHz/20ms 下 Opus 包大小分布 | trace 中 len 字段统计 |
| T5 | PC 防火墙对入站 UDP 媒体端口的行为 | 首次推流是否被拦 |
| T6 | 时间戳字段是否确为 elapsedRealtime | 与手机 `adb shell cat /proc/uptime` 对齐 |

## 8. 自写 Android 客户端对接要点（派生结论）

1. **角色**：TCP server（默认 8125）+ UDP sender（connect 到 PC 媒体端口）。
2. **必须实现**：§2 全消息应答（含错误码 10/12/13）、5s 心跳看门狗、§3.3 头格式、20ms/48kHz/单声道 Opus。
3. **可简化**：不实现 Speex（codec 100 可拒应答 12）、不实现头格式 A/B、不实现自动重采样（锁定 48000；不支持则报错）。
4. **复刻官方字节**：101 负载用 `04 06 03 00 00 00`（官方 6.3 实证值，非 20kdc 旧值）。
5. **音量字节恒发 5**。

## 9. 证据索引

| 论断 | 证据文件（本目录 `evidence/` 下） |
|---|---|
| TCP 帧格式 | `b2/AbstractC0175c.java` |
| 消息分派/处理逻辑 | `V1/l.java` |
| 101 字段 | `d2/a.java`；应答 `d2/b.java` |
| 102 字段/错误码 | `d2/d.java`；应答 `d2/e.java` |
| 103/104/105 | `d2/f.java`、`d2/g.java`、`d2/c.java`；应答 `d2/e.java` |
| 心跳看门狗 | `V1/k.java` |
| 状态机 | `V1/n.java`、`A/g.java`（证据外引，jadx 全量产物）、`com/wo/voice2/VoiceService.java` |
| WiFi/BT server | `e2/j.java`、`e2/c.java`、`e2/g.java`、`e2/e.java` |
| 连接封装/UDP 通道 | `e2/f.java`、`e2/h.java`、`e2/d.java`、`e2/b.java` |
| UDP 头三格式 | `s1/i.java` |
| 采集/编码 | `X1/b.java`、`X1/a.java`、`com/wo/voice2/audio/OpusEncoder.java`、`com/wo/voice2/audio/a.java` |
| 默认配置 | `V1/f.java` |
| 传输选择 | `com/wo/voice2/MainActivity.java` |
| PC 客户端构造 | `WOMicClient.exe` 反汇编（§5 地址表） |
| 驱动签名 | `womic.sys` PE 证书表解析（§6） |
