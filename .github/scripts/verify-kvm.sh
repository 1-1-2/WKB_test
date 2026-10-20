#!/usr/bin/env bash
# 模拟器前置检查：KVM 可用性 + emulator 二进制 + AVD 环境。
#
# 设计原则（吸取 run 39453084847 的教训）：
#   * 只有"确定跑不了"才 fail fast，其余给 ::warning:: 继续——误杀比漏报代价更高；
#   * 每条外部命令都套 timeout，本脚本自身必须在数秒内结束；
#   * 所有判断都打印依据（为什么是这个结论），便于事后核对。
#
# 可测试性：KVM_PATH / EMU_BIN / ADB_BIN / AVDMANAGER_BIN 均可覆盖。

set -uo pipefail

KVM_PATH="${KVM_PATH:-/dev/kvm}"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
EMU="${EMU_BIN:-$SDK/emulator/emulator}"
[ -x "$EMU" ] || EMU="$(command -v emulator || echo "")"
AVDMANAGER="${AVDMANAGER_BIN:-avdmanager}"

echo "=== 硬件虚拟化 ==="
if [ ! -e "$KVM_PATH" ]; then
    echo "::error::$KVM_PATH 不存在：runner 未提供嵌套虚拟化，模拟器无法硬件加速"
    uname -a
    ls /dev | grep -i kvm || echo "（/dev 下无 kvm 节点）"
    exit 1
fi
ls -la "$KVM_PATH"
if [ ! -r "$KVM_PATH" ] || [ ! -w "$KVM_PATH" ]; then
    # 权限不足不立刻判死：部分镜像靠 kvm 组授权，emulator 自己会给出更准确的报错
    echo "::warning::$KVM_PATH 当前用户（$(id -un)）不可读写，继续尝试（若模拟器报 accel 错误请看下方 accel-check）"
else
    echo "[kvm] 可读写"
fi
echo "[kvm] CPU 虚拟化标志: $(grep -cE 'vmx|svm' /proc/cpuinfo 2>/dev/null || echo 0) 行"

echo "=== emulator 二进制 ==="
if [ -z "$EMU" ] || [ ! -x "$EMU" ]; then
    echo "::error::找不到 emulator 可执行文件（SDK=$SDK）"
    ls "$SDK" 2>&1 || true
    exit 1
fi
echo "[emu] $EMU"
timeout 30 "$EMU" -version 2>&1 | head -5 || true

echo "=== 系统镜像 ==="
ls -d "$SDK"/system-images/*/*/* 2>/dev/null || echo "（无，需 sdkmanager 安装）"

echo "=== 已有 AVD ==="
timeout 30 "$AVDMANAGER" list avd 2>&1 | sed -n '1,25p' || true

echo "=== accel-check（决定要不要继续）==="
ACCEL_OUT="$(timeout 30 "$EMU" -accel-check 2>&1 || true)"
echo "$ACCEL_OUT" | head -10

if echo "$ACCEL_OUT" | grep -qiE "is installed and usable"; then
    echo "[kvm] 加速可用"
elif echo "$ACCEL_OUT" | grep -qiE "not installed|not supported|permission denied|cannot open"; then
    # 明确不可用：与其等 8 分钟超时，不如立刻失败并留下原因
    echo "::error::emulator -accel-check 明确报告加速不可用（见上），停止以免空耗 8 分钟"
    exit 1
else
    echo "::warning::accel-check 输出无法判定（格式未知），继续尝试启动"
fi

echo "=== 前置检查通过 ==="
