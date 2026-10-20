#!/usr/bin/env bash
# 交互式调试会话里的一键状态快照。
#
# SSH 进 runner 后先跑它：一次打印模拟器/设备/app/服务/端口/日志的现状，
# 省得逐个敲命令。可从任意目录调用。

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

echo "==================== 交互式调试状态 ===================="
echo "repo: $REPO_ROOT"
echo "time: $(date -u +%H:%M:%SZ)  |  已运行会话 $(( SECONDS ))s"

echo
echo "--- adb / 设备 ---"
adb devices -l 2>&1 | head -5
echo "boot_completed: $(timeout 10 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || echo '<无>')"
echo "  SDK: $(timeout 10 adb shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r' || echo '?')  ABI: $(timeout 10 adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || echo '?')"

echo
echo "--- app / 服务 ---"
adb shell pm list packages 2>/dev/null | grep -i miclink || echo "（未安装 com.miclink.open）"
echo "pid: $(timeout 10 adb shell pidof com.miclink.open 2>/dev/null | tr -d '\r' || echo '<无>')"
timeout 15 adb shell dumpsys activity services com.miclink.open 2>/dev/null \
  | grep -iE "ServiceRecord|isForeground|app=" | head -5 || echo "（无服务记录）"
echo "监听 8125(0x1FBD): $(timeout 10 adb shell cat /proc/net/tcp 2>/dev/null | awk 'NR>1{print $2}' | grep -ci ':1FBD' || echo 0)"

echo
echo "--- 最近与本项目相关的 logcat（20 条）---"
timeout 15 adb logcat -d 2>/dev/null | grep -iE "miclink|womic|OpusEncoder|UdpSender|MicCaptureService" | tail -20 || echo "（无）"

echo
echo "--- 最近异常（10 条）---"
timeout 15 adb logcat -d 2>/dev/null | grep -iE "FATAL|AndroidRuntime|Force finishing" | tail -10 || echo "（无）"

echo
echo "--- /tmp/emulator.log 尾部（10 行）---"
tail -10 /tmp/emulator.log 2>/dev/null || echo "（无）"

echo
echo "--- 最近一次联调输出（若有）---"
[ -f /tmp/interop.out ] && tail -20 /tmp/interop.out || echo "（还没跑过 pc_client.py）"

echo
echo "==================== 常用命令 ===================="
cat <<'TIPS'
  重跑协议闭环：
    cd android && python3 verify/integration/pc_client.py --hold-seconds 12 2>&1 | tee /tmp/interop.out

  同步最新的脚本改动（不需要重新触发流水线）：
    git fetch origin && git checkout origin/dev/wifi-client -- .github/scripts

  实时看日志：
    tail -f /tmp/emulator.log
    adb logcat | grep -iE 'miclink|womic|UdpSender'

  手动启动服务（调试音模式）：
    adb shell am start-service -n com.miclink.open/.MicCaptureService \
      -a com.miclink.open.action.START \
      --ei extra.control_port 8125 --ei extra.volume 5 --ez extra.debug_tone true

  收集诊断到 /tmp/diag：
    cd android && bash ../.github/scripts/collect-diagnostics.sh
TIPS
