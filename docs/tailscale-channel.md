# CI Runner 与开发者机器组网通道：方案对比与结论

> **状态**：已完成选型（2026-10）。本文沉淀本项目在"GitHub-hosted runner / 受限网络沙箱
> ↔ 开发者机器 ↔ GitHub"三条边上用过的全部组网与传输通道，含各自的踩坑与结论。
> **用途**：后续任何需要在 CI runner 上做交互调试、或从受限环境向 GitHub 推送代码的任务，
> 先读本文再选通道，不要重新试错。
> **关联文档**：向 GitHub 推送的最终通道方法论见 [`github-api-push.md`](github-api-push.md)；
> 驱动取证过程见 [`driver-whitepaper.md`](driver-whitepaper.md)。

---

## 1. 问题背景

本项目的取证与验证大量发生在 **GitHub-hosted Windows Runner**（临时 VM，作业结束即销毁）和
**受限网络沙箱**（到 `github.com:443` 的出网被阻断，但 `api.github.com` 可达）里。
由此产生三类通道需求：

| 需求 | 典型场景 |
|---|---|
| 交互访问 runner | 驱动取证、GUI 排查、长时间实验的现场操作 |
| 受限环境推代码 | 沙箱里 `git push` 到公开仓库 |
| 稳定可达 runner | 批量/可编程地操作 runner（不依赖一次性会话） |

下面三种通道分别覆盖这些需求，可以组合使用。

---

## 2. 方案总览

| 方案 | 解决什么 | 自建成本 | 稳定性 | 结论 |
|---|---|---|---|---|
| upTerm SSH 直连 | 交互访问 runner | 零（hosted runner 原生） | 低（会话一次性，自愈不可依赖） | 一次性交互首选 |
| 反代（Caddy 转发 GitHub） | 受限环境推代码 / 访问 GitHub | 低（一台反代服务器） | 高 | 可用；注意用法与凭据卫生 |
| Tailscale 组网 | 稳定、可编程地访问 runner | 中（tailnet 管理） | 高 | 长期/批量操作首选 |

---

## 3. 方案 A：upTerm SSH 直连（交互访问 runner）

### 3.1 原理

GitHub-hosted runner 的 workflow 里启动 upterm 会话，日志输出一个 `wss://` 会话地址，
本地用 SSH 通过该 WebSocket 隧道连入 runner，得到完整 PTY。

### 3.2 优点

- **零自建**：hosted runner 原生能力，不需要任何常驻服务；
- 完整 PTY，交互式跑什么都行。

### 3.3 踩坑与结论（均为实测教训）

| 坑 | 现象 | 结论/做法 |
|---|---|---|
| 会话自愈不可依赖 | upterm 会话死后 25 分钟无任何恢复工件 | **重要实验前先确认会话健康**；长实验改为 SYSTEM 计划任务承载，会话只做观察 |
| PTY 多层引号地狱 | 多层 bash→ssh→bash 后 `\"`/`$` 必乱 | 复杂操作一律写成 py/ps1 脚本，base64 分块上传（`base64 -w 1400` → 逐行 `echo` 追加），不要拼内联命令 |
| 大文件回传不可行 | 回传实测 60/899 行耗时 10 分钟 | 改**远端就地计算**，只回传几 KB 结论 |
| GUI 导出搞死会话 | 在 upterm 会话里跑 Procmon 的 GUI 导出，会话直接死 | GUI 类操作（如 Procmon 启停与导出）全部交给 SYSTEM 计划任务执行 |
| 远端 shell 卡在续行符 | 卡在 `>` 且所有输入被吞 | 发 Ctrl+C（`printf '\003'`）恢复；恢复后需重传文件 |
| 本地 shell 路径转换 | MSYS 环境把 `schtasks /run` 转成 Git 路径；zsh 的 `status` 是只读变量导致脚本无声失败 | 包 `powershell -Command` 调用；脚本避开 shell 保留字 |
| 主机密钥 | strict 校验失败 | 用 `-o StrictHostKeyChecking=accept-new` + 独立 known_hosts，先对比再信任 |

---

## 4. 方案 B：反代转发 GitHub（受限环境推代码）

### 4.1 原理

沙箱到 `github.com:443` 被阻断，但自建/自管的 Caddy v2 反代可达：
git 流量走 `<git-path-prefix>/`、REST API 流量走 `<api-path-prefix>/`，都转发到 GitHub 上游。

```caddyfile
reverse_proxy https://github.com {
    header_up Host github.com
    flush_interval -1
}
```

### 4.2 实测对照（决定性证据：真实 push）

| 测试项 | 走反代 | 直连 github.com |
|---|---|---|
| DNS + TLS 握手 | 连通（秒级） | **失败：45s 超时，exit 28** |
| 匿名读 refs | HTTP 200 | HTTP 000 |
| push 握手 git-receive-pack | HTTP 200，返回完整 refs | 不可达 |
| `git ls-remote` / `git fetch` | 成功 | 不可达 |
| **真实 `git push`（POST receive-pack）** | **成功** | 不可达 |

响应头含 `via: 2.0 Caddy`、Content-Type 为 `application/x-git-receive-pack-advertisement`、
响应体是合法 pkt-line——Smart HTTP 的读写两侧都被正确代理，**配置无缺陷**。

### 4.3 用法要点

```bash
# 方式 A：单次指定（凭据不落 .git/config）
git push "https://oauth2:<YOUR_PAT>@<your-proxy-domain>/<git-path-prefix>/<owner>/<repo>.git" <branch>

# 方式 B：透明重写（原 remote 不用改，git 自动走反代）
git config --global url."https://<your-proxy-domain>/<git-path-prefix>/".insteadOf "https://github.com/"
git config --global url."https://<your-proxy-domain>/<api-path-prefix>/".insteadOf "https://api.github.com/"
```

- PAT 走 Basic 认证：用户名任意（约定 `x-access-token`），密码填 PAT。
- 凭据写进 remote URL 会落 `.git/config`——**别把该目录同步走**，优先用单次指定或环境变量。

### 4.4 踩坑与结论

| 坑 | 结论 |
|---|---|
| "反代不通"的误判 | 实为**用法错误**：remote 配的是直连 URL。反代与 API 两条路都存在时，先花 30 秒做一次 `curl` 对照，判断是哪一侧不通，不要直接绕路 |
| **反代 URL 注入死凭证** | 把已过期/已吊销的 token 内嵌进 remote URL，失败现象（401）看起来像网络/代理问题，极易带偏排查方向。**凭据一律走环境变量或单次指定，不落配置** |
| 大 pack push 首字节延迟 | 反代加 `flush_interval -1`，避免 chunked 流式写入被缓冲 |
| 二次压缩 | git 路径**不要**启用 `encode`，pack 流被二次压缩可能导致流式传输异常 |
| 重定向 | 上游 3xx 原样返回给客户端，git 能自己处理；不要改成手动跟随，否则可能被绕回直连 |

---

## 5. 方案 C：Tailscale 组网（稳定、可编程）

### 5.1 原理

runner 与开发者机器都加入同一 **tailnet**（WireGuard 网状 VPN）：

- runner 获得 tailnet 内的 `100.x` 稳定地址，本地直接 `ssh` / HTTP 访问，
  不经过任何公网隧道，不受 GitHub 侧网络策略影响；
- 作业结束 runner 销毁，地址释放，无残留攻击面。

### 5.2 管理面（API 自动化）

用「管理用」OAuth client 换取**短命 access token**（约 1 小时），再调 Tailscale API：

- `GET /tailnet/-/devices`：列设备；
- `PUT /tailnet/-/acl`：改 ACL（需带 `If-Match: <ETag>`）；
- `POST /device/<id>/tags`、`DELETE /tailnet/-/keys/<keyID>`：标签与密钥管理。

### 5.3 踩坑与结论

| 坑 | 结论 |
|---|---|
| 管理 client scope 受限 | 只带 `policy_file` / `devices:core` 等 scope：**签不出设备、看不到密钥列表**（实测 `POST /keys` → 403，`GET /keys` → 空）。要签设备密钥需提升 scope |
| token 只有 1 小时 | 长任务要定时重换；长期凭证只是 client secret，泄露可随时 revoke |
| 凭据文件卫生 | client id/secret 落盘时 `chmod 600` 且**不入库** |

### 5.4 与 upTerm 的对比

| 维度 | upTerm SSH 直连 | Tailscale |
|---|---|---|
| 连接稳定性 | 会话一次性，断了就没了 | runner 在即地址在，可重连 |
| 可编程 | 弱（靠日志里读会话地址） | 强（API 列设备、打 tag、管 ACL） |
| 自建成本 | 零 | 需 tailnet 管理权 |
| 适用 | 一次性人工交互 | 长期/批量/自动化操作 |

### 5.5 直连调试：tailscale-dual 的 debug_hold

`tailscale-dual.yml`（双机联调 workflow）内置了本文通道的一次具体应用：
**调试窗口挂起（debug_hold）**——主理人从沙箱 SSH 直连两台 runner，在联调
关键步骤之前手工跑命令，到点后 workflow 自动继续。设计动机：E 步（跨网 TCP
hello 探活）的失败曾被 cp1252 编码崩溃掩盖，需要"链路原样挂在那里、人登进去
看现场"的能力，而不是靠一轮轮 CI 试错。

**触发方式**（仅 workflow_dispatch，不挂 push）：

- `debug_hold=true`（boolean 输入）；
- `debug_minutes`：窗口时长（分钟，默认 `'20'`；linux-app 侧 clamp ≤40、
  windows-client 侧 clamp ≤55，与两个 job 的总预算 50/65min 匹配；空值或
  非数字回落 20）。

**挂起位置与语义**（到点后 workflow 正常继续往下跑，不改变任何判据）：

| job | 挂起位置 | 挂起时现场 | 连接方式 |
|---|---|---|---|
| linux-app | socat TCP 中继就绪后、等 windows-client 之前 | socat 已起、app 在模拟器内运行、UDP 中继在转发；A 的 tailnet IP 在 Step Summary | `ssh -o StrictHostKeyChecking=accept-new root@<A_IP>` |
| windows-client | D 步（驱动就绪）后、E 步（跨网探活）前 | 驱动已装、womic.exe 就绪、tailscale 已入网；A_IP/CONTROL_PORT/womic.exe 路径在 Step Summary | `ssh -o StrictHostKeyChecking=accept-new runneradmin@<B_IP>` |

两端 SSH 均只监听 tailnet 内 22 端口（ACL：`tag:ops → tag:ci:22` 已放行），
公钥取自仓库变量 `vars.UPTERM_DEBUG_PUBKEY`（公钥非敏感，可入库变量）；
windows-client 侧为内联实现（不复用 `tailnet-debug` action——B 步已入网，
重复 up 会改变节点状态），Linux 侧 apt 兜底装 openssh-server。两台 runner 都是
ephemeral 节点，作业结束即注销，无残留攻击面。

---

## 6. 选型建议

1. **一次性交互调试**（驱动取证、现场实验）→ **upTerm**，零成本，但重要实验用 SYSTEM
   计划任务承载，会话只做观察；
2. **受限网络推代码到 GitHub** → 先 30 秒 `curl` 对照确认断的是哪一侧；
   能走反代就走反代（`insteadOf` 透明重写）；
   凭证只细粒度 PAT 时直接走 **Git Data API**（见 [`github-api-push.md`](github-api-push.md)）；
3. **需要稳定/批量操作 runner** → **Tailscale**，地址稳定、API 可编程；
4. 三条通道**可叠加**：Tailscale 保交互可达，反代/API 保代码上行。

---

## 7. 安全红线（写文档/写脚本时同样适用）

- 任何文档、脚本、日志中**不得出现真实 token / PAT 字符串**，一律 `<YOUR_PAT>` 占位；
- 不得出现真实反代域名、内部 IP、会话 URL；
- 凭据优先走环境变量；必须落盘时 `chmod 600` 且排除出版本库；
- token 用完即吊销。
