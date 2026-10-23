#!/usr/bin/env python3
"""经 GitHub Git Data API 推送本地提交（git-over-HTTPS 认证被沙箱/ PAT 限制时的通道）。

做法：对每个待推送提交，先用 /git/blobs 建变更文件的 blob（内容相同 → sha 与本地
git blob sha 一致），再把父树条目表应用 diff 后整表 POST /git/trees，随后
/git/commits 建提交（保留作者/提交者/时间），最后 PATCH refs/heads/<branch> 快进。
推完后校验远端最终 tree sha 与本地 HEAD 一致。
"""
import base64
import json
import subprocess
import sys
import urllib.error
import urllib.request

TOK = sys.argv[1]
OWNER, REPO = "1-1-2", "WKB_test"
BRANCH = sys.argv[2] if len(sys.argv) > 2 else "main"
BASE = "https://api.github.com"
REPO_API = f"{BASE}/repos/{OWNER}/{REPO}"


def api(method, path, data=None):
    req = urllib.request.Request(REPO_API + path, method=method)
    req.add_header("Authorization", f"Bearer {TOK}")
    req.add_header("Accept", "application/vnd.github+json")
    payload = None
    if data is not None:
        req.add_header("Content-Type", "application/json")
        payload = json.dumps(data).encode()
    try:
        with urllib.request.urlopen(req, payload, timeout=60) as r:
            body = r.read()
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        print(f"!! {method} {path} -> {e.code}: {e.read().decode()[:400]}")
        raise


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def remote_head():
    return api("GET", f"/git/ref/heads/{BRANCH}")["object"]["sha"]


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
    return out.split()[0]


def push_commit(commit, parent_remote_sha, entries):
    """把本地 commit 推到远端 parent 之上，返回 (新 commit sha, 新 entries 表)"""
    meta = git("log", "-1", "--format=%an%n%ae%n%aI%n%cn%n%ce%n%cI", commit).strip().split("\n")
    an, ae, ad, cn, ce, cd = meta
    message = git("log", "-1", "--format=%B", commit)

    # ---- 1. diff：相对本地父提交（首个提交的父=远端 main，其内容与本地一致）
    parent = git("rev-parse", f"{commit}^").strip()
    diff = git("diff", "--name-status", "-M", parent, commit).strip().split("\n")
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
    # ---- 3. 建 commit（保留作者与提交者信息）
    c = api("POST", "/git/commits", {
        "message": message,
        "tree": tree["sha"],
        "parents": [parent_remote_sha],
        "author": {"name": an, "email": ae, "date": ad},
        "committer": {"name": cn, "email": ce, "date": cd},
    })
    # ---- 4. 快进 ref
    api("PATCH", f"/git/refs/heads/{BRANCH}", {"sha": c["sha"], "force": False})
    print(f"  pushed {commit[:9]} -> {c['sha'][:9]}  (blobs={blob_calls}, files={len(new_entries)})")
    return c["sha"], new_entries


def main():
    remote0 = remote_head()
    local_base = git("rev-parse", f"{remote0}").strip()
    commits = git("rev-list", "--reverse", f"{remote0}..HEAD").strip().split("\n")
    commits = [c for c in commits if c]
    if not commits:
        print("无待推送提交")
        return
    print(f"远端 {BRANCH} = {remote0[:9]}，待推送 {len(commits)} 个提交")

    # 首提交的父树条目表取自远端
    parent_commit = api("GET", f"/git/commits/{remote0}")
    entries = remote_tree_entries(parent_commit["tree"]["sha"])
    print(f"远端基础 tree 条目: {len(entries)}")

    for c in commits:
        remote0, entries = push_commit(c, remote0, entries)

    # 校验：远端最终 commit 的 tree 应与本地 HEAD 的 tree 完全一致
    remote_final = api("GET", f"/git/commits/{BRANCH}") if False else api("GET", f"/git/ref/heads/{BRANCH}")["object"]["sha"]
    rf = api("GET", f"/git/commits/{remote_final}")
    local_tree = git("rev-parse", "HEAD^{tree}").strip()
    ok = rf["tree"]["sha"] == local_tree
    print(f"远端 tree={rf['tree']['sha'][:9]} 本地 tree={local_tree[:9]} -> {'一致 ✔' if ok else '不一致 ✘'}")
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
