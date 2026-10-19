# tools/ —— 仓库维护脚本

| 脚本 | 作用 | 依赖 |
|---|---|---|
| `api_push.py` | 经 **GitHub Git Data API** 推送本地提交（沙箱内 git-over-HTTPS 认证受限时的推送通道） | python3 标准库 + 本地 git 仓库 + GitHub PAT |

> 早期的 Windows 驱动安装 / 卸载 / 签名 / 验证 PowerShell 脚本已随自研线归档至
> `self-driver` 分支；`tools/` 现在只放与**本仓库自身维护**相关的脚本。

---

## api_push.py

### 用途

在本机 `git push` 走不通（沙箱网络只放行 `api.github.com`、或凭证形态受限）时，
用 GitHub **Git Data API** 四步法把本地提交重放到远端：

```
待推送提交 N
  → ① /git/blobs      为变更文件建 blob（内容相同则 sha 与本地一致）
  → ② /git/trees      把父树条目表应用 diff 后整表提交
  → ③ /git/commits    建提交（保留作者/提交者/时间，父子关系按本地历史）
  → ④ PATCH /git/refs/heads/<branch>   快进远端引用
```

推完后**校验远端最终 tree sha 与本地 HEAD 一致**，不一致即报错退出——
这是"远端 SHA 与本地不同步也认 tree 一致"判据的由来。

### 用法

```bash
# PAT 提取（凭据文件在仓库外，整行自包含，必须带 github_pat_ 前缀）
export GH_TOKEN=$(grep -m1 '^github_pat_' /workspace/docs/SECRET-credentials.txt)

# 推送（默认 main）
python3 tools/api_push.py "$GH_TOKEN"
python3 tools/api_push.py "$GH_TOKEN" <branch>
```

### 行为要点

| 项 | 说明 |
|---|---|
| 远端 tip 非本地对象 | 自动在本地历史里找 tree 一致的 base 续推（重放起点对齐） |
| 作者 / 提交者 / 时间 | 全部按本地提交保留，远端看到的历史与本地一致 |
| 空提交 | 无文件变更的提交直接复用父 tree，不产生空 diff |
| 终态校验 | 远端 `tree.sha` == 本地 HEAD `tree.sha` 才退出 0 |

### 安全

- PAT **只从环境变量 / 参数传入**，脚本不落盘、不打印；
- 脚本只访问 `api.github.com`，不触发任何 Web 页面流程；
- 详细通道考古（为什么不用 git-over-HTTPS、为什么不用 gh CLI）见
  [`docs/github-api-push.md`](../docs/github-api-push.md)。
