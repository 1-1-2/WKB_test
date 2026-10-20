# Windows Runner 交互式联调：action-tmate 替代方案研究

> 结论先行：**`mxschmitt/action-tmate` 在 Windows runner 上已不可用**（2025-06 起损坏，
> 上游 tmate 项目本身已停止维护）。当前唯一在 Windows 上可靠工作的同类工具是
> **`WarpBuilds/action-debugger@v1.3`**（tmate 的定制分支，支持 Linux/macOS/Windows）。
> 本仓库的 `.github/workflows/android.yml` 已按其能力配置 `windows-interactive` 作业。

## 1. 背景：为什么需要交互式联调

安卓端真机验证清单（协议规格 v1 第 6 节 T1-T6）需要在**装好官方 WO Mic Windows 客户端
6.3** 的机器上抓包 / 观察驱动行为。GitHub Actions 的 windows-latest runner 可以提供：

- 干净的 Windows 环境（可安装官方 `WOMicClient.exe` 安装器）；
- 稳定的外网（下载安装器 / 驱动）；
- 与手机同一局域网？——**不能**。runner 在 Azure 机房，手机在办公网，
  因此 runner 里的客户端连不到手机。交互式会话的真正用途是：
  1. 在 runner 里安装官方客户端、核对安装器内容（驱动签名、adb.exe、Opus 库）；
  2. 把手机上抓到的 PCAP / 日志拉到 runner 里用官方客户端做对照回放；
  3. 需要人工判断的排查步骤（断点、环境变量、注册表）。

## 2. mxschmitt/action-tmate 现状（2026-10 核实）

| 项目 | 状态 |
|---|---|
| `mxschmitt/action-tmate` 仓库 | **已归档**（archived，只读） |
| Windows 支持 | 自 2025-06 起损坏，issue #229 "Invalid session token" 多人复现，无修复 |
| 上游 `tmate-io/tmate` | 已停止维护（tmate-io/tmate#322，2024 年起无发布） |
| `upterm` | 仅 Linux/macOS，无 Windows 构建 |
| `debug-via-ssh`（第三方） | 需 ngrok 账号 + 信用卡，不满足"开箱即用" |

**结论**：任何"在 workflow 里起 tmate 会话"的方案，只要追加上游 tmate，就继承
"上游已死"的风险；action-tmate 的 Windows 损坏只是第一个症状。

## 3. WarpBuilds/action-debugger（选定方案）

`WarpBuilds/action-debugger@v1.3` 是 mxschmitt/action-tmate 的社区定制分支，
自建 tmate 服务器（`gha.warp.build`），Windows 支持已修复。

**已验证的输入参数**（README 未给完整表格，以下经文档与示例整理）：

| 输入 | 默认 | 说明 |
|---|---|---|
| `detached` | `false` | 分离模式：打印连接信息后继续执行后续步骤，job 结束前等待会话退出 |
| `limit-access-to-actor` | `false` | `true` 时仅触发工作流的用户（GitHub 资料里的公钥）可连 |
| `install-dependencies` | `true` | 自建 runner 可设 `false` 跳过 tmate 安装 |
| `tmate-server-host` | `gha.warp.build` | 可换自建 tmate 服务器 |
| `msys2-location` | Windows 上 `C:\msys64` | 与 `msys2/setup-msys2` 集成用 |
| `sudo` | Linux 上 `true` | 环境无 sudo 时设 `false` |

**两种运行行为**：

- **阻塞（默认）**：启动 SSH 会话并等待用户连接、退出 shell 后继续；
- **分离（`detached: true`）**：立即继续执行后续步骤，post-job 阶段等待会话退出
  （10 分钟无人连接自动收摊）。输出 `ssh-command` / `ssh-address` / `web-url`
  可跨 job 传递。

**连接方式**：SSH 或 Web 终端；连接字符串打印在工作流日志底部 + Run 的 Checks 页底部。
在会话中 `touch continue` 可主动放行后续步骤。

## 4. 本仓库的配置

`.github/workflows/android.yml` 的 `windows-interactive` 作业：

```yaml
windows-interactive:
  if: github.event_name == 'workflow_dispatch' && inputs.interactive
  runs-on: windows-latest
  steps:
    - uses: actions/checkout@v4
    - uses: actions/setup-java@v4          # JDK 17
    - run: ./gradlew :core:test :app:assembleDebug --no-daemon
    - uses: WarpBuilds/action-debugger@v1.3
      with:
        limit-access-to-actor: true
      timeout-minutes: 120
```

要点：

1. **默认不跑**：只有 `workflow_dispatch` 勾选 `interactive` 才触发，push/PR 不受影响
   （否则每次 push 都挂一个等连接的作业，纯属浪费额度）；
2. **先构建后交互**：进会话时代码已构建过，直接排查不重复编译；
3. **限制访问**：`limit-access-to-actor: true`，防止连接串泄露后被任意人登入 runner；
4. **超时 120 分钟**：GitHub 单 job 上限 6h，120min 足够人工排查又不至于忘关。

## 5. 备选：自建 tmate 服务器

如果 `gha.warp.build` 不可用，可一键自建（任意有公网 IP 的 Linux 机器）：

```bash
# tmate 服务器需要 2222/tcp 对外开放
docker run -d -p 2222:2222 -p 22:22 \
  -e SSH_KEYS_PATH=/keys ghcr.io/tmate-io/tmate-ssh-server:latest
```

然后把 `tmate-server-host` / `tmate-server-port` / 两个指纹输入指向自建实例。
指纹在服务器日志里打印（`SHA256:...`）。

## 6. 安全须知

- runner 的 SSH 连接串等同于**免密登录凭证**：不要贴进 issue / PR / 群聊；
- `limit-access-to-actor: true` 只允许触发者连接，公开仓库上尤其要开；
- 会话里做的事全部留痕于 runner 日志；敏感操作（下载驱动、注册表改动）注意清理。
