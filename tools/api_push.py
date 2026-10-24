#!/usr/bin/env python3
"""经 GitHub Git Data API 推送本地提交（git-over-HTTPS 认证被沙箱/ PAT 限制时的通道）。

做法：对每个待推送提交，先用 /git/blobs 建变更文件的 blob（内容相同 → sha 与本地
git blob sha 一致），再把父树条目表应用 diff 后整表 POST /git/trees，随后
/git/commits 建提交（保留作者/提交者/时间），最后 PATCH refs/heads/<branch> 快进。
推完后校验远端最终 tree sha 与本地 HEAD 一致。

通道：经主理人私有反代 gg.823333.xyz/ghapi-chiputaobutuputaopi 访问 GitHub API
（沙箱直连 api.github.com 会被重置/阻断）。

子命令（第一个参数固定为 PAT）：
  api_push.py <PAT> [branch] [--force]     推送（默认快进；--force 从根重放并强推，
                                           用于历史重写/孤儿分支；远端无该分支时自动创建）
  api_push.py <PAT> --create-ref <ref> [branch]
                                           把 branch（默认 main）的远端 tip 镜像为一个
                                           新 ref（备份保命：refs/heads/... 或 refs/tags/...）
  api_push.py <PAT> --delete <branch>      删除远端分支（危险操作，需显式指定）

网络层瞬时错误（连接重置/断开/超时）自动重试（指数退避）；HTTP 错误立即失败。
"""
import base64
import http.client
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

TOK = sys.argv[1] if len(sys.argv) > 1 else None
OWNER, REPO = "1-1-2", "WKB_test"
# 沙箱直连 api.github.com 不稳（连接被重置），改走主理人私有 API 反代
BASE = "https://gg.823333.xyz/ghapi-chiputaobutuputaopi"
REPO_API = f"{BASE}/repos/{OWNER}/{REPO}"


def usage():
    print(__doc__)
    sys.exit(2)


# ---------------------------------------------------------------- 参数解析
ARGS = sys.argv[2:]
BRANCH = "main"
FORCE = False
CREATE_REF = None
DELETE_BRANCH = None
_positional = []
_i = 0
while _i < len(ARGS):
    _a = ARGS[_i]
    if _a == "--force":
        FORCE = True
    elif _a == "--create-ref":
        _i += 1
        if _i >= len(ARGS):
            usage()
        CREATE_REF = ARGS[_i]
    elif _a == "--delete":
        _i += 1
        if _i >= len(ARGS):
            usage()
        DELETE_BRANCH = ARGS[_i]
    elif _a.startswith("--"):
        usage()
    else:
        _positional.append(_a)
    _i += 1
if not TOK or len(_positional) > 1:
    usage()
if _positional:
    BRANCH = _positional[0]
CREATE_REF_FROM = BRANCH  # --create-ref 的源分支（默认 main）


def api(method, path, data=None, _retries=4):
    """GitHub REST 调用。HTTP 错误（4xx/5xx 带响应）立即抛出；
    连接层瞬时错误（重置/断开/超时）指数退避重试——长任务重放中途
    被网络抖断时不至于整体报废。"""
    req = urllib.request.Request(REPO_API + path, method=method)
    req.add_header("Authorization", f"Bearer {TOK}")
    req.add_header("Accept", "application/vnd.github+json")
    # 私有反代前置 Cloudflare，按浏览器签名拦截 Python-urllib 默认 UA（Error 1010）
    req.add_header("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
    payload = None
    if data is not None:
        req.add_header("Content-Type", "application/json")
        payload = json.dumps(data).encode()
    for attempt in range(_retries):
        try:
            with urllib.request.urlopen(req, payload, timeout=60) as r:
                body = r.read()
                return json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            print(f"!! {method} {path} -> {e.code}: {e.read().decode()[:400]}")
            raise
        except (urllib.error.URLError, http.client.RemoteDisconnected,
                ConnectionError, TimeoutError) as e:
            if attempt == _retries - 1:
                raise
            wait = 2 ** attempt
            print(f".. {method} {path} 网络错误 {type(e).__name__}: {e}，{wait}s 后重试（{attempt + 1}/{_retries}）")
            time.sleep(wait)


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def remote_head(branch=None):
    """远端分支 tip commit sha；分支不存在返回 None。"""
    b = branch or BRANCH
    try:
        return api("GET", f"/git/ref/heads/{b}")["object"]["sha"]
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def remote_tree_entries(tree_sha):
    """取远端某 tree 的递归扁平条目表：{path: (mode, type, sha)}"""
    out = []
    page_tree = tree_sha
    while True:
        d = api("GET", f"/git/trees/{page_tree}?recursive=1")
        out.extend(d["tree"])
        if not d.get("truncated"):
            break
        # 超大仓库才会走到这里；本仓库 271 个文件不会触发
        raise SystemExit("远端 tree 被截断，需要分页处理")
    return {e["path"]: (e["mode"], e["type"], e["sha"]) for e in out if e["type"] == "blob"}


def make_blob(path, commit):
    content = subprocess.run(["git", "show", f"{commit}:{path}"], capture_output=True, check=True).stdout
    d = api("POST", "/git/blobs", {
        "content": base64.b64encode(content).decode(),
        "encoding": "base64",
    })
    return d["sha"]


def local_file_mode(path, commit):
    out = git("ls-tree", commit, "--", path).strip()
    if not out:
        return None
    mode = out.split()[0]
    if mode.startswith("040"):
        raise SystemExit(f"路径 {path!r} 在 {commit[:9]} 是目录而非文件（diff 缺少 -r？）")
    return mode


def commit_diff(commit, parent):
    """本地 commit 相对 parent 的 name-status 行表；parent=None（孤儿根）时
    用 diff-tree --root 输出全量 A。注意 diff-tree 默认不递归，必须加 -r，
    否则新增目录只报目录名（如 "A tools"）而非其下文件。"""
    if parent is None:
        return git("diff-tree", "--no-commit-id", "--root", "-r", "-M", "--name-status", commit).strip().split("\n")
    return git("diff", "--name-status", "-M", parent, commit).strip().split("\n")


def push_commit(commit, parent_remote_sha, entries):
    """把本地 commit 推到远端 parent 之上，返回 (新 commit sha, 新 entries 表)。
    parent_remote_sha=None 表示无父（孤儿根 / 新分支首提交）。"""
    meta = git("log", "-1", "--format=%an%n%ae%n%aI%n%cn%n%ce%n%cI", commit).strip().split("\n")
    an, ae, ad, cn, ce, cd = meta
    message = git("log", "-1", "--format=%B", commit)

    # ---- 1. diff：相对本地父提交（孤儿根无父，用 --root 全量）
    try:
        parent = git("rev-parse", f"{commit}^").strip()
    except subprocess.CalledProcessError:
        parent = None  # 本地历史根提交（孤儿分支 / 重写后首个提交）
    diff = commit_diff(commit, parent)
    new_entries = dict(entries)
    blob_calls = 0
    for line in diff:
        if not line.strip():
            continue
        parts = line.split("\t")
        status = parts[0][0]
        if status in ("A", "M", "T"):
            path = parts[-1]
            mode = local_file_mode(path, commit)
            sha = make_blob(path, commit)
            blob_calls += 1
            new_entries[path] = (mode, "blob", sha)
        elif status == "D":
            new_entries.pop(parts[-1], None)
        elif status.startswith("R"):
            new_entries.pop(parts[1], None)  # 旧路径
            path = parts[-1]
            mode = local_file_mode(path, commit)
            sha = make_blob(path, commit)
            blob_calls += 1
            new_entries[path] = (mode, "blob", sha)
        else:
            raise SystemExit(f"未处理的 diff 状态: {line}")

    # ---- 2. 建 tree（整表；GitHub 会按 path 里的 / 自动建中间树）
    tree = api("POST", "/git/trees", {
        "tree": [{"path": p, "mode": m, "type": t, "sha": s} for p, (m, t, s) in new_entries.items()],
    })
    # ---- 3. 建 commit（保留作者与提交者信息；无父时 parents=[]）
    payload = {
        "message": message,
        "tree": tree["sha"],
        "author": {"name": an, "email": ae, "date": ad},
        "committer": {"name": cn, "email": ce, "date": cd},
    }
    if parent_remote_sha:
        payload["parents"] = [parent_remote_sha]
    else:
        payload["parents"] = []
    c = api("POST", "/git/commits", payload)
    # ---- 4. 更新 ref：PATCH 快进（force 模式强推）；分支不存在则 POST 创建
    try:
        api("PATCH", f"/git/refs/heads/{BRANCH}", {"sha": c["sha"], "force": FORCE})
    except urllib.error.HTTPError as e:
        if e.code in (404, 422):
            api("POST", "/git/refs", {"ref": f"refs/heads/{BRANCH}", "sha": c["sha"]})
        else:
            raise
    print(f"  pushed {commit[:9]} -> {c['sha'][:9]}  (blobs={blob_calls}, files={len(new_entries)})")
    return c["sha"], new_entries


def commits_to_push(remote0):
    """待推送提交列表。三种情况：
    1. remote0 是 HEAD 的祖先 → 正常快进，rev-list remote0..HEAD；
    2. remote0 不是本地对象（Git Data API 重放产生的 sha，git 里不存在）；
    3. remote0 是本地对象但不是 HEAD 的祖先（曾 fetch 过远端重放史：对象已
       拉下来，但父链与本地史完全不相交）——此时 rev-list 不报错，却会把
       整个本地史当作待推送，叠在重放史之上形成污染历史。
    2/3 统一走 tree 匹配回退：以本地历史中 tree 与远端 tree 一致的提交为
    base（内容同步点），diff 链不受影响。"""
    is_ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", remote0, "HEAD"],
        capture_output=True,
    ).returncode == 0
    if is_ancestor:
        return [c for c in git("rev-list", "--reverse", f"{remote0}..HEAD").strip().split("\n") if c]
    rt = api("GET", f"/git/commits/{remote0}")["tree"]["sha"]
    for c in git("rev-list", "HEAD").strip().split("\n"):
        if c and git("rev-parse", f"{c}^{{tree}}").strip() == rt:
            print(f"远端 tip {remote0[:9]} 非本地史祖先（API 重放 sha），以 tree 一致的本地提交 {c[:9]} 为 base")
            return [x for x in git("rev-list", "--reverse", f"{c}..HEAD").strip().split("\n") if x]
    raise SystemExit("远端 tip 既非本地史祖先，也未找到 tree 一致的本地 base")


def do_create_ref():
    """把 branch 的远端 tip 镜像为一个新 ref（备份保命）。"""
    src = CREATE_REF_FROM
    sha = remote_head(src)
    if sha is None:
        raise SystemExit(f"远端分支 {src} 不存在，无从镜像")
    ref = CREATE_REF if CREATE_REF.startswith("refs/") else f"refs/heads/{CREATE_REF}"
    d = api("POST", "/git/refs", {"ref": ref, "sha": sha})
    print(f"created {ref} -> {d['object']['sha'][:9]}（镜像自远端 {src}）")


def do_delete():
    """删除远端分支（危险操作）。"""
    if remote_head(DELETE_BRANCH) is None:
        print(f"远端分支 {DELETE_BRANCH} 不存在，无需删除")
        return
    api("DELETE", f"/git/refs/heads/{DELETE_BRANCH}")
    print(f"deleted refs/heads/{DELETE_BRANCH}")


def main():
    if CREATE_REF:
        do_create_ref()
        return
    if DELETE_BRANCH:
        do_delete()
        return

    remote0 = remote_head()
    if FORCE:
        # 历史重写 / 孤儿分支：从本地根全量重放，远端 ref 强推
        commits = [c for c in git("rev-list", "--reverse", "HEAD").strip().split("\n") if c]
        print(f"!! --force 模式：从根重放 {len(commits)} 个提交并强推 refs/heads/{BRANCH}")
        if remote0:
            print(f"!! 远端 {BRANCH} 当前 tip {remote0[:9]} 将被覆盖（历史重写）")
        entries = {}
    else:
        if remote0 is None:
            print(f"远端无分支 {BRANCH}，将创建（首提交为无父根）")
            commits = [c for c in git("rev-list", "--reverse", "HEAD").strip().split("\n") if c]
            entries = {}
        else:
            commits = commits_to_push(remote0)
            if not commits:
                print("无待推送提交")
                return
            # 首提交的父树条目表取自远端
            parent_commit = api("GET", f"/git/commits/{remote0}")
            entries = remote_tree_entries(parent_commit["tree"]["sha"])
    print(f"远端 {BRANCH} = {(remote0 or '（不存在）')[:9]}，待推送 {len(commits)} 个提交")
    print(f"远端基础 tree 条目: {len(entries)}")

    for i, c in enumerate(commits):
        # force 模式从根重放：首提交必须是真·无父根（parents=[]），不能挂在旧 tip 之下
        parent_for_commit = None if (FORCE and i == 0) else remote0
        remote0, entries = push_commit(c, parent_for_commit, entries)

    # 校验：远端最终 commit 的 tree 应与本地 HEAD 的 tree 完全一致
    remote_final = remote_head()
    if remote_final is None:
        raise SystemExit("推送后远端分支仍不存在，异常")
    rf = api("GET", f"/git/commits/{remote_final}")
    local_tree = git("rev-parse", "HEAD^{tree}").strip()
    ok = rf["tree"]["sha"] == local_tree
    print(f"远端 tree={rf['tree']['sha'][:9]} 本地 tree={local_tree[:9]} -> {'一致 ✔' if ok else '不一致 ✘'}")
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
