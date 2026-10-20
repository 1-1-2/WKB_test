#!/usr/bin/env bash
# 收集诊断信息到 $DIAG_DIR（默认 /tmp/diag），调用方用 if: always() 上传。
#
# 【重要】每条 adb 命令都套 timeout：上一轮事故（run 39453084847）里该脚本在
# "没有模拟器"的状态下把作业挂住 25 分钟，连 `gh run cancel` 都要等它退出——
# 诊断脚本绝不能成为新的挂起点。
#
# 可测试性：ADB_BIN / DIAG_DIR / ADB_TIMEOUT / KVM_PATH 均可覆盖，见 tests/run-tests.sh。

set -uo pipefail

DIAG_DIR="${DIAG_DIR:-/tmp/diag}"
ADB="${ADB_BIN:-adb}"
ADB_TIMEOUT="${ADB_TIMEOUT:-15}"

mkdir -p "$DIAG_DIR"

# 统一入口：超时/失败都写进对应文件，绝不阻塞
adbd() {
    local out="$1"; shift
    timeout "$ADB_TIMEOUT" "$ADB" "$@" > "$out" 2>&1
    local rc=$?
    if [ $rc -eq 124 ]; then
        echo "[诊断] adb $* 超时（>${ADB_TIMEOUT}s），已跳过" >> "$out"
    elif [ $rc -ne 0 ]; then
        echo "[诊断] adb $* 返回 $rc" >> "$out"
    fi
}

echo "=== 收集诊断到 $DIAG_DIR（adb=$ADB，单条上限 ${ADB_TIMEOUT}s）==="

{
    echo "=== 时间 ==="; date -u
    echo "=== 内核 ==="; uname -a
    echo "=== CPU ==="; nproc
    echo "=== 内存 ==="; free -h
    echo "=== 磁盘 ==="; df -h
    echo "=== KVM ==="; ls -la "${KVM_PATH:-/dev/kvm}" 2>&1
    echo "=== 环境 ==="; env | grep -iE "android|java_home|gradle" | sort
} > "$DIAG_DIR/env.txt" 2>&1

adbd "$DIAG_DIR/adb-devices.txt" devices -l

if [ -f /tmp/emulator.log ]; then
    cp /tmp/emulator.log "$DIAG_DIR/emulator.log"
    tail -200 /tmp/emulator.log > "$DIAG_DIR/emulator-tail.txt"
else
    echo "没有 /tmp/emulator.log（模拟器未启动到该步骤）" > "$DIAG_DIR/emulator.log"
fi

adbd "$DIAG_DIR/logcat.txt" logcat -d
adbd "$DIAG_DIR/logcat-crash.txt" logcat -d -b crash
adbd "$DIAG_DIR/services.txt" shell dumpsys activity services com.miclink.open
adbd "$DIAG_DIR/package.txt" shell dumpsys package com.miclink.open
adbd "$DIAG_DIR/props.txt" shell getprop
adbd "$DIAG_DIR/ps.txt" shell "ps -A"
adbd "$DIAG_DIR/net-tcp.txt" shell "cat /proc/net/tcp"

[ -f /tmp/interop.out ] && cp /tmp/interop.out "$DIAG_DIR/interop.out"

echo "===== $DIAG_DIR 内容 ====="
ls -la "$DIAG_DIR"

echo "===== logcat 中与本项目相关的行（最近 60 条）====="
grep -iE "miclink|womic|OpusEncoder|MicCaptureService|UdpSender" "$DIAG_DIR/logcat.txt" 2>/dev/null | tail -60 || echo "（无匹配）"

echo "===== logcat 中的异常（最近 40 条）====="
grep -iE "FATAL|AndroidRuntime|Exception|Force finishing" "$DIAG_DIR/logcat.txt" 2>/dev/null | tail -40 || echo "（无匹配）"

echo "===== 诊断收集完成 ====="
exit 0
