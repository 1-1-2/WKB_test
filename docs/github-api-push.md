# GitHub API 推送通道方法论（受限网络 / 受限凭证环境）

> **状态**：已固化（2026-10）。本文记录在"`github.com` 直连被阻断 + 凭证只有细粒度 PAT"
> 的环境下，向 GitHub 推送代码的完整通道考古过程与最终结论。
> **实现**：[`../tools/api_push.py`](../tools/api_push.py)（本文方法论的参考实现，已在仓库中）。
> **关联**：各通道的完整对比见 [`tailscale-channel.md`](tailscale-channel.md)。

> **⚠ 2026-10-24 结论更新（优先阅读）**：阶段 1 的"git-over-HTTPS 认证被拒"结论**已不成立**。
> 经私有 **git 反代**（`ghgit-` 前缀）+ `oauth2:<PAT>` 内嵌 URL 可正常 push——建分支、
> 删分支、强推均实证通过，**秒级完成**（对比 API 重放每提交约 30-60s）。
> **当前通道分工**：git 代理（`ghgit-`）管推拉；API 代理（`ghapi-`）只管 Actions /
> metadata 查询。API 重放降级为无 git 协议环境下的备用通道，其代价与踩坑复盘见 §7。

---

## 1. 环境约束（问题定义）

| 约束 | 表现 |
|---|---|
| 网络 | 沙箱到 `github.com:443` 出网被阻断（45s 超时，curl exit 28）；`api.github.com` 可达 |
| 凭证 | 只有**细粒度 PAT**（fine-grained）；对 `api.github.com` 有效 |
| 目标 | 把本地 git 提交推送到公开仓库 `<owner>/<repo>` 的 `<branch>` |

关键观察：**不是全局断网**（REST API 通），是 `github.com` 这个 host 被阻断/黑洞。
所有通道选择都从这一条事实出发。

---

## 2. 通道考古时间线（每个方案为什么不行/行）

### 阶段 0：git-over-HTTPS 直连 —— 不通

```
git push https://oauth2:<YOUR_PAT>@github.com/<owner>/<repo>.git
→ Failed to connect to github.com port 443 after 134964 ms（硬阻断，非瞬时抖动）
```

### 阶段 1：git-over-HTTPS 认证被拒 —— 不通

绕过网络阻断后（走反代等方式），git-over-HTTPS 的认证环节仍被拒：
细粒度 PAT 对 `api.github.com` 有效，但 git-over-HTTPS 的认证策略不接纳该凭证。
**教训：PAT 的"有效"是分端点的**，先在小范围验证端点可用性再铺开。

### 阶段 2：反代 + URL 内嵌死凭证 —— 可用但埋雷

反代链路本身被证明完全可用（读写两侧均通，见 [`tailscale-channel.md`](tailscale-channel.md) §4），
但把 token 内嵌进 remote URL 的用法出了事故：**注入的是已过期/已吊销的死凭证**，
失败表现为认证错误，现象像网络/代理故障，排查被带偏了很久。

教训：

- 凭据一律走环境变量或单次指定，**不写进 remote URL / `.git/config`**；
- 遇到 401 先验证凭证本身有效性，再怀疑链路。

### 阶段 3：Contents API 逐文件写 —— 可行但不完整

`api.github.com` 可达，早期改用 Contents API 做写入。问题：

- 每次一个文件一次请求，多文件变更无法保证原子性；
- 不携带作者/提交者元数据，历史被抹平；
- 大仓库全量对账成本高。

**它掩盖了"其实该走反代"这个更简单的解**——两条路都存在时，先做 30 秒对照再绕路。

### 阶段 4：Git Data API 四步推送 —— 最终方案 ✅

不依赖 git 协议、不依赖反代，只用 `api.github.com` 的 Git Data API 完整复刻 git 推送语义。
详见下节。

---

## 3. Git Data API 四步法

对每个待推送提交，依次调用四个端点（父提交为远端当前分支头）：

```
1. POST /git/blobs    为每个变更文件建 blob（base64 内容）
2. POST /git/trees    把父树条目表应用 diff 后，整表建新 tree
3. POST /git/commits  建 commit（保留作者/提交者/日期与 message）
4. PATCH /git/refs/heads/<branch>  把分支 ref 快进到新 commit（force=false）
```

要点：

- **blob 内容与本地一致时 sha 天然一致**（Git 对象模型的内容寻址），这是后续校验的基础；
- **tree 整表重建**：diff 只决定哪些条目变化，但建 tree 时传全量条目表
  （GitHub 会按 path 里的 `/` 自动建中间树）；
- **diff 状态处理**：A/M/T（增改类型变更）→ 建 blob 更新条目；D → 删条目；
  R（重命名）→ 删旧路径 + 建新路径；
- **保留元数据**：commit 的 author / committer / date 从本地 `git log` 原样带过去，
  推送后历史与本地一致；
- **快进保护**：`force=false`，远端被别人推过时直接失败，不会覆盖；
- **推完必须校验**：取远端最终 commit 的 tree sha，与本地 `HEAD^{tree}` 比对，
  不一致即失败退出——"推成功"不等于"推对"。

> 参考实现：[`../tools/api_push.py`](../tools/api_push.py)。
> 用法：`python3 tools/api_push.py <YOUR_PAT> [branch]`（PAT 从命令行参数传入，
> 脚本内不落盘、不打印）。

---

## 4. 安全规范（硬性）

1. **所有凭证一律占位符 `<YOUR_PAT>`**：本文档、脚本、日志、CI 配置中不得出现任何真实
   token 字符串——PAT 的各种前缀形式（细粒度/经典）一出现即应按泄露处理并吊销；
2. **最小权限**：细粒度 PAT 只勾选本仓库的 `Contents: Read and write`，
   推送完成后立即吊销；
3. **不落盘**：不写 `.git/config`、不配 `credential.helper store`、不写任何凭据文件；
4. **日志卫生**：失败响应体只打印前 400 字符，且确认不含 `Authorization` 头内容。

---

## 5. 已知边界

| 边界 | 说明 |
|---|---|
| 单 blob 大小 | GitHub blob API 有大小上限，超大文件（二进制产物）不应走此路，走 Release Assets 等通道 |
| 特殊 git 对象 | 不传输 annotated tag、git notes、submodule 指针语义 |
| 并发推送 | 依赖快进保护（force=false），多人推同一分支时后推者失败重跑即可 |
| 空仓库 | 远端无任何提交时，首提交的"父"需要特殊处理（实现中按远端 main 为空分支的场景验证） |

---

## 6. 结论

- 网络断的是 `github.com` 而不是 `api.github.com` 时，**Git Data API 是凭据约束下最稳的推送通道**：
  只用 HTTPS REST、只用细粒度 PAT、推送后可校验；
- 有反代且凭证允许 git-over-HTTPS 时，反代 + `insteadOf` 透明重写更简单（通道对比见
  [`tailscale-channel.md`](tailscale-channel.md) §4）；
- 无论走哪条路：**先分清是网络问题还是凭证问题**（30 秒 curl 对照），
  **凭据永不落配置**，**推完校验远端 tree**。

---

## 7. 2026-10-24 复盘：API 重放的代价与通道分工

一次 25 提交历史重写中，API 重放通道被完整压测，结论修正如下。

### 7.1 两条通道对照（同一仓库、同一网络环境实测）

| 维度 | git 反代直推（`ghgit-`） | API 重放（`ghapi-` / Git Data API） |
|---|---|---|
| 单次推送耗时 | **秒级**（一次 pack 上传） | 15-25 分钟（每提交 30-60s，226 次 blob POST） |
| 原子性 | 原子（一个 ref 更新） | 非原子（每提交一次 PATCH，中途断则半途） |
| 远端 SHA | **与本地一致**（对象原样传输） | 与本地不同（重放产生新对象，仅 tree 可比对） |
| 失败模式 | 连接层错误自动重试即可 | 连接重置、Cloudflare 拦 UA、半途静默中断 |
| 适用 | 常规推拉、建/删分支、force push | 无 git 协议通道时的兜底 |

### 7.2 API 重放踩坑实录（均已修复，`tools/api_push.py` 内固化）

1. **`diff-tree` 默认不递归**：孤儿根的全量 diff 只报目录名（`A tools`）而非其下文件，
   目录条目（mode `040000` + type `blob`）导致 `422 InvalidObject`。修复：`--root -r`，
   且 `local_file_mode` 对目录模式直接报错而非静默造垃圾 blob。
2. **重放 sha 与本地史不相交**：`git fetch` 远端重放 tip 后，`rev-list remote0..HEAD`
   **不报错**却返回整个本地史（两套对象无共同祖先），若直接推送会把全史叠在重放史上
   形成污染历史（中间树全错、终态树却可能碰巧正确，极具迷惑性）。
   修复：以 `merge-base --is-ancestor` 判定快进资格，非祖先一律走 tree 匹配回退。
3. **Cloudflare 按浏览器签名拦 Python-urllib**（Error 1010）：API 走私有反代时需带
   浏览器 UA；curl 能过不代表 urllib 能过。
4. **直连 `api.github.com` 不稳定**：长任务中途 `RemoteDisconnected`；重试 + 换反代后消失。
5. **长任务不能只看 tee 的退出码**：`python ... | tee log` 的退出码是 tee 的，python
   崩溃会被掩盖；必须 `set -o pipefail` 或检查日志尾部。

### 7.3 通道分工（团队约定）

| 操作 | 通道 |
|---|---|
| push / fetch / 建删分支 / force push / 打 tag | git 反代 `https://oauth2:$PAT@gg.823333.xyz/ghgit-<key>/<owner>/<repo>.git` |
| Actions（dispatch/取消/看 run）、refs 与 commit 元数据查询 | API 反代 `https://gg.823333.xyz/ghapi-<key>/repos/<owner>/<repo>/...`（带浏览器 UA） |
| 无 git 协议可用时的最后兜底 | `tools/api_push.py`（Git Data API 重放） |
