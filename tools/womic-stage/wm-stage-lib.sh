#!/usr/bin/env bash
# ============================================================================
# wm-stage-lib.sh —— workflow 侧读取 WOMic 阶段状态的助手（被 workflow source）
#
# 背景：agent_stage.py 把每个阶段的结论追加进 $WOMIC_WORK/state.jsonl。
# workflow 的 install-driver / wait（恢复模式）步骤需要读这些结论来决定
# 「继续等 / 自行验证 / 重 hosted 会话」。本文件只提供只读查询，不执行安装。
#
# 用法（在 workflow 的 run: 块里）：
#     source "$GITHUB_WORKSPACE/tools/womic-stage/wm-stage-lib.sh"
#     stage_status probe          # -> ok / fail / start / -
#     state_age_seconds           # -> state.jsonl 距上次修改的秒数（无文件 -> -1）
#     task_running                # -> 0/1：WomicStage 计划任务是否在跑
# ============================================================================

# stage_status <阶段名>：输出该阶段最后一次记录的状态
stage_status() {
  python -c '
import json, sys, os
last = "-"
base = os.environ.get("WOMIC_WORK") or r"D:\a\_temp"
path = os.path.join(base, "state.jsonl")
try:
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except Exception:
                continue
            if e.get("stage") == sys.argv[1]:
                last = e.get("status", "-")
except Exception:
    pass
print(last)
' "$1"
}

# state_age_seconds：state.jsonl 距上次修改的秒数；文件不存在输出 -1
state_age_seconds() {
  local st="${WOMIC_WORK:-D:\\a\\_temp}/state.jsonl"
  [ -f "$st" ] || { echo -1; return; }
  local now mt
  now=$(date +%s)
  mt=$(stat -c %Y "$st" 2>/dev/null || echo "$now")
  echo $(( now - mt ))
}

# task_running：WomicStage 计划任务是否正在执行（schtasks 状态或 state 新鲜度）
task_running() {
  if schtasks /query /tn WomicStage /fo LIST 2>/dev/null | grep -qi "Running"; then
    return 0
  fi
  # schtasks 在某些环境下对 Running 状态显示不一致，用 state.jsonl 新鲜度兜底：
  # 3 分钟内还在写状态，认为任务仍在推进
  local age
  age=$(state_age_seconds)
  [ "$age" -ge 0 ] && [ "$age" -le 180 ] && return 0
  return 1
}

# stage_table：一行打印所有阶段状态（供日志）
stage_table() {
  local s out=""
  for s in fetch pick_devcon install verify_device probe; do
    out="${out} ${s}=$(stage_status "$s")"
  done
  echo "${out# }"
}
