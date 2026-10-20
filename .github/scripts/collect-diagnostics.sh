#!/usr/bin/env bash
# 收集诊断信息到 /tmp/diag（调用方用 if: always() 上传，取消/失败都能留证）。
#
# 上一轮被取消时作业什么都没留下，只能靠"卡在 Start emulator"这一条猜测原因。

set -uo pipefail
mkdir -p /tmp/diag

{
    echo "=== 时间 ==="; date -u
    echo "=== 内核 ==="; uname -a
    echo "=== CPU ==="; nproc
    echo "=== 内存 ==="; free -h
    echo "=== 磁盘 ==="; df -h
    echo "=== KVM ==="; ls -la /dev/kvm 2>&1
    echo "=== 环境 ==="; env | grep -iE "android|java_home|gradle" | sort
} > /tmp/diag/env.txt 2>&1

adb devices -l > /tmp/diag/adb-devices.txt 2>&1 || true

if [ -f /tmp/emulator.log ]; then
    cp /tmp/emulator.log /tmp/diag/emulator.log
else
    echo "没有 /tmp/emulator.log（模拟器未启动到该步骤）" > /tmp/diag/emulator.log
fi

adb logcat -d > /tmp/diag/logcat.txt 2>&1 || true
adb logcat -d -b crash > /tmp/diag/logcat-crash.txt 2>&1 || true
adb shell dumpsys activity services com.miclink.open > /tmp/diag/services.txt 2>&1 || true
adb shell dumpsys package com.miclink.open > /tmp/diag/package.txt 2>&1 || true
adb shell dumpsys media.audio_flinger > /tmp/diag/audio_flinger.txt 2>&1 || true
adb shell getprop > /tmp/diag/props.txt 2>&1 || true
adb shell "ps -A" > /tmp/diag/ps.txt 2>&1 || true
adb shell "cat /proc/net/tcp" > /tmp/diag/net-tcp.txt 2>&1 || true

[ -f /tmp/interop.out ] && cp /tmp/interop.out /tmp/diag/interop.out
[ -f /tmp/emulator.log ] && tail -200 /tmp/emulator.log > /tmp/diag/emulator-tail.txt

echo "===== /tmp/diag 内容 ====="
ls -la /tmp/diag

echo "===== logcat 中与本项目相关的行（最近 60 条）====="
grep -iE "miclink|womic|OpusEncoder|MicCaptureService|UdpSender" /tmp/diag/logcat.txt 2>/dev/null | tail -60 || echo "（无匹配）"

echo "===== logcat 中的异常（最近 40 条）====="
grep -iE "FATAL|AndroidRuntime|Exception|Force finishing" /tmp/diag/logcat.txt 2>/dev/null | tail -40 || echo "（无匹配）"
