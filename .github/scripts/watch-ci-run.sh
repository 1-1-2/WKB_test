#!/usr/bin/env bash
# watch_ci_run.sh —— 主动看门狗：超过预期时间就取消，而不是干等。
#
# 背景：GitHub 的 job/step 级 timeout-minutes 只在超时那一刻介入。我们踩过两次：
#   (a) `adb wait-for-device` 挂住 25 分钟；
#   (b) 诊断脚本里的 adb 没 timeout，连 `gh run cancel` 都退不出来。
# 所以除 workflow 内的超时，还需要一个**外部**观察者：按"这一步正常该花多久"
# 判断，明显不合理就直接取消并打印证据。
#
# 用法：
#   GH_TOKEN=... bash tools/watch_ci_run.sh              # 分支最新 run
#   GH_TOKEN=... bash tools/watch_ci_run.sh 39453084847  # 指定 run
#
# 环境变量：REPO / BRANCH / POLL（轮询秒）/ GRACE（预算放大系数）/ MAX_WAIT（整体上限秒）
# 退出码：0 run 成功；1 run 失败/取消；2 看门狗主动取消或自身超时。
#
# 能力边界：GitHub 没有"运行中作业行级日志"的公开 API（/jobs/{id}/logs 在作业
# 结束前返回 404），所以这里给的是**步骤级**实时进度：哪一步在跑、跑了多久、
# 是否超预算。行级内容要等作业结束或依赖 artifact 里的诊断。

set -uo pipefail

REPO="${REPO:-1-1-2/WKB_test}"
BRANCH="${BRANCH:-dev/wifi-client}"
POLL="${POLL:-30}"
GRACE="${GRACE:-1.6}"
MAX_WAIT="${MAX_WAIT:-3600}"

api() { gh api "$@"; }

RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
  RUN_ID=$(api "/repos/$REPO/actions/runs?per_page=1&branch=$BRANCH" --jq '.workflow_runs[0].id') || exit 2
  echo "[watch] 分支 $BRANCH 最新 run：$RUN_ID"
fi

PARSE=/tmp/watch_parse.$$.py
cat > "$PARSE" <<'PY'
import json, sys, datetime

# 每一步的正常耗时预算（分钟）；未列出的用 DEFAULT
BUDGET = {
    "Set up job": 3, "Run actions/checkout@v4": 2, "Set up JDK 17": 2,
    "Set up Android SDK tools": 3, "Install Android SDK components": 12,
    "Install emulator components": 12, "Fetch Opus source": 6,
    "Assemble debug APK": 20, "Create AVD": 3, "Start emulator": 12,
    "Install and launch app": 8, "Interop closed loop": 6,
    "Collect diagnostics": 4, "Upload diagnostics": 5,
    "Run protocol unit tests": 10, "Run CI script tests": 3,
    "Prebuild artifacts": 20, "Restore WDK NuGet packages": 15,
    "Build solution": 25,
}
DEFAULT = 15
grace = float(sys.argv[1])

now = datetime.datetime.now(datetime.timezone.utc)
for j in json.load(sys.stdin).get("jobs", []):
    print("JOB\t%s\t%s\t%s" % (j["name"], j["status"], j.get("conclusion") or "-"))
    for s in j.get("steps", []):
        if s["status"] != "in_progress" or not s.get("started_at"):
            continue
        t0 = datetime.datetime.fromisoformat(s["started_at"].replace("Z", "+00:00"))
        elapsed = (now - t0).total_seconds()
        budget = BUDGET.get(s["name"], DEFAULT) * 60 * grace
        flag = "OVER" if elapsed > budget else "ok"
        print("STEP\t%s\t%s\t%d\t%d\t%s" % (j["name"], s["name"], elapsed, budget, flag))
PY

START_TS=$(date +%s)
echo "[watch] 轮询 ${POLL}s，预算放大 ${GRACE}×，整体上限 ${MAX_WAIT}s"

while true; do
  if [ $(( $(date +%s) - START_TS )) -gt "$MAX_WAIT" ]; then
    echo "[watch] 超过整体上限 ${MAX_WAIT}s → 取消 run $RUN_ID"
    api -X POST "/repos/$REPO/actions/runs/$RUN_ID/cancel" > /dev/null 2>&1 || true
    rm -f "$PARSE"
    exit 2
  fi

  RUN_JSON=$(api "/repos/$REPO/actions/runs/$RUN_ID" 2>/dev/null) || { sleep "$POLL"; continue; }
  STATUS=$(printf '%s' "$RUN_JSON" | python3 -c 'import json,sys;print(json.load(sys.stdin)["status"])' 2>/dev/null) || { sleep "$POLL"; continue; }
  CONC=$(printf '%s' "$RUN_JSON" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("conclusion") or "-")' 2>/dev/null)

  if [ "$STATUS" = "completed" ]; then
    echo "[watch] run $RUN_ID 结束：$CONC"
    rm -f "$PARSE"
    [ "$CONC" = "success" ] && exit 0 || exit 1
  fi

  JOBS=$(api "/repos/$REPO/actions/runs/$RUN_ID/jobs" 2>/dev/null) || { sleep "$POLL"; continue; }
  printf '%s' "$JOBS" | python3 "$PARSE" "$GRACE" > /tmp/watch_state.$$ 2>/dev/null || true

  echo "--- $(date -u +%H:%M:%SZ) run=$RUN_ID status=$STATUS"
  awk -F'\t' '$1=="JOB" {printf "  [job] %s => %s/%s\n", $2, $3, $4}' /tmp/watch_state.$$
  awk -F'\t' '$1=="STEP" {printf "     ▶ %s  %ss / 预算 %ss%s\n", $3, $4, $5, ($6=="OVER"?"  ← 超预算":"")}' /tmp/watch_state.$$

  OVER=$(awk -F'\t' '$1=="STEP" && $6=="OVER"' /tmp/watch_state.$$)
  if [ -n "$OVER" ]; then
    echo
    echo "[watch] 步骤耗时明显超出预期，主动取消 run $RUN_ID："
    printf '%s\n' "$OVER" | awk -F'\t' '{printf "  - %s / %s：已 %ss，预期上限 %ss\n", $2, $3, $4, $5}'
    echo "[watch] 取消后拉日志：gh run view $RUN_ID --log"
    api -X POST "/repos/$REPO/actions/runs/$RUN_ID/cancel" > /dev/null 2>&1 || true
    rm -f "$PARSE"
    exit 2
  fi

  sleep "$POLL"
done
