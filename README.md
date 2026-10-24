# MicLink Open —— 自研线归档（self-driver 分支）

> **本分支是历史归档，不再维护。** 第三方兼容线（WO Mic 协议：`android/` 采集端 +
> `client/` Windows 客户端）请使用 **`main`** 分支。

## 这个分支是什么

本分支保存了 miclink-open 早期的**自研内核虚拟麦克风全链路**：

```
+----------------------+          +------------------------------------------+
| 任意 WOMP 发送端      |   UDP    | miclinkserver.exe（用户态服务端）         |
| （手机麦克风来源）     | =======> |   UDP 5004 收包 -> Opus 解码 + PLC        |
|   libopus 编码        |  Opus    |   -> 抖动缓冲 -> 重采样 -> 环形缓冲       |
|   Packetizer 32B 头   |          |        | DeviceIoControl(WRITE_PCM)      |
+----------------------+          | +------------------------------------+   |
                                  | | miclinkvirtualmic.sys（内核驱动）     |   |
                                  | | 控制设备 \\.\MicLinkControl          |   |
                                  | |  WaveRT 捕获端点（虚拟麦克风）       |   |
                                  | +----------------+--------------------+   |
                                  |                  | Windows 音频栈        |
                                  |   设置 -> 系统 -> 声音 -> 输入设备        |
                                  |   "MicLink Virtual Microphone"           |
                                  +------------------------------------------+
```

说自研的 **MicLink/WOMP 协议**（UDP 5004，32 B 头 + Opus），与 `main` 分支的
**WO Mic 协议兼容线**（TCP 8125 + UDP，格式 C 头 11 B）是**两条独立的链路**，
互不兼容，也从未互联。

## 为什么归档

自研驱动需要自行签发内核签名，在未采购 EV 证书的前提下无法在客户机默认加载；
且与官方 `womic.sys` 行为存在差异。2026-10 实测后**弃用该路线**，改为复用 WO Mic
官方已签名驱动（`womic.sys`，未修改），项目只做用户态的安装/灌音/协议两端。
完整决策记录（背景/后果/备选/合规状态）见 `main` 分支
[`docs/adr/0001-driver-route.md`](../docs/adr/0001-driver-route.md)。

## 归档内容（112 个文件）

| 路径 | 内容 |
|---|---|
| `driver/` | Windows 内核虚拟麦克风驱动（WDM/KS + WaveRT，衍生自 Microsoft Windows-driver-samples `audio/simpleaudiosimple`，MIT） |
| `server/` | Windows 用户态服务端（UDP 接收 → Opus 解码 → IOCTL 灌音；CMake 构建） |
| `shared/core/` | 驱动/服务端共用的平台中立核心层（协议契约、DSP、环形/抖动缓冲，纯 C99） |
| `tests/` | `shared/core` 的单元测试与协议 fuzz（ctest） |
| `.github/workflows/build-driver.yml` | 驱动 CI（WDK NuGet 模式编译） |
| `tools/{install,uninstall,make-test-cert,verify}.ps1` | 驱动安装/卸载/测试签名/验证脚本 |
| `docs/Signing-Guide.md` | 驱动签名指南（测试签名 vs 生产签名） |

## 使用须知

1. **只作技术参考**：其中的结论（协议契约、构建方法、踩坑记录）对理解项目历史有效；
   但**不作为交付路径**，也不再接收修复。
2. **与 `main` 的关系**：`main` 是干净的单线历史（第三方兼容线）；本分支是单快照提交，
   **不继承**原仓库 70 提交的混合历史（自研线 CI 调试噪音与兼容线演进纠缠在一起）。
   原完整历史保留在备份分支 `backup/main-prerewrite-20261024` 与 tag
   `prerewrite-20261024`（重写前的 main 指向）。
3. **文档引用**：`main` 的 `docs/VERIFICATION.md`、`docs/Windows-Manual-Verification.md`、
   `docs/PRD.md`、`docs/ARCHITECTURE.md`、`docs/TEST_REPORT.md` 是自研线时期的历史资产
   （🗄 标记），其中指向 `driver/` `server/` `shared/` `tests/` 的路径以本分支为准。
4. **驱动基线**：`driver/` 衍生自 `microsoft/Windows-driver-samples` @
   `2dc3fd3a0cc84a2933f2194e7ec0871584979071`（`audio/simpleaudiosimple`，MIT），
   第三方许可声明见 `main` 分支根 `NOTICE.md`。

## 归档信息

- **归档日期**：2026-10-24
- **归档执行**：git 治理（#65），主理人齐活林
- **源状态**：重写前 main（e8694ec，备份分支 `backup/main-prerewrite-20261024`）
- **快照提交**：单提交，含上述 112 个文件 + 本 README
