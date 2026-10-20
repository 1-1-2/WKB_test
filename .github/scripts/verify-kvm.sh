#!/usr/bin/env bash
# 模拟器前置检查：KVM 可用性 + emulator 二进制 + AVD 环境。
#
# 上一轮失败教训：/dev/kvm 存在不代表可用（权限/嵌套虚拟化关闭都会让
# emulator 起不来），所以这里除了存在性还检查可读写，并打印足够的环境信息
# 供事后定位。

set -uo pipefail

echo "=== 硬件虚拟化前置检查 ==="
if [ ! -e /dev/kvm ]; then
    echo "::error::/dev/kvm 不存在：runner 未提供嵌套虚拟化"
    echo "--- 环境 ---"
    uname -a; nproc; free -h | head -2; df -h / | tail -1
    echo "--- /dev 下是否有 kvm 相关节点 ---"
    ls /dev | grep -i kvm || echo "（无）"
    exit 1
fi
ls -la /dev/kvm
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo "::error::/dev/kvm 权限不足（当前用户 $(id -un) 无读写）"
    id; exit 1
fi
echo "[kvm] 可读写 ok"
grep -cE 'vmx|svm' /proc/cpuinfo 2>/dev/null | sed 's/^/[kvm] CPU 虚拟化标志行数: /' || true

echo "=== emulator 二进制 ==="
EMU="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}/emulator/emulator"
[ -x "$EMU" ] || EMU="$(command -v emulator || true)"
if [ -z "$EMU" ] || [ ! -x "$EMU" ]; then
    echo "::error::找不到 emulator 可执行文件（ANDROID_HOME=$ANDROID_HOME）"
    ls "$ANDROID_HOME" || true
    exit 1
fi
echo "[emu] $EMU"
"$EMU" -version 2>&1 | head -5 || true

echo "=== 系统镜像 ==="
ls -d "$ANDROID_HOME"/system-images/*/*/* 2>/dev/null || echo "（无）"

echo "=== 已有 AVD ==="
avdmanager list avd 2>&1 | sed -n '1,25p' || true

echo "=== 加速能力自检（emulator -accel-check）==="
"$EMU" -accel-check 2>&1 | head -10 || true
