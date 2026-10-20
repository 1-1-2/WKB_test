#!/usr/bin/env bash
# 模拟器前置检查：KVM 权限 + emulator 动态库 + 系统镜像 + accel 能力。
#
# 设计原则（吸取 run 39453084847 / 39459335348 的教训）：
#   * 只有"确定跑不了"才 fail fast，其余给 ::warning:: 继续——误杀比漏报代价更高；
#   * 每条外部命令都套 timeout，本脚本自身必须在数秒内结束；
#   * 所有判断都打印依据（为什么是这个结论），便于事后核对。
#
# GitHub ubuntu-24.04 镜像的两个坑（实测）：
#   1) /dev/kvm 是 crw-rw---- root:kvm，runner 用户不在 kvm 组 →
#      emulator 报 "This user doesn't have permissions to use KVM"。runner 有免密
#      sudo，chmod 666 是简单可靠的解法（仅限一次性 CI runner）。
#   2) 镜像没装 libpulse0，而 emulator 的 qemu 二进制链接了它（即便 -no-audio）→
#      "error while loading shared libraries: libpulse.so.0"。库由 workflow 的
#      "Install emulator system dependencies" 步骤安装，这里只做残留校验。
#
# 可测试性：KVM_PATH / EMU_BIN / ADB_BIN / AVDMANAGER_BIN / SUDO_BIN 均可覆盖。

set -uo pipefail

KVM_PATH="${KVM_PATH:-/dev/kvm}"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
EMU="${EMU_BIN:-$SDK/emulator/emulator}"
[ -x "$EMU" ] || EMU="$(command -v emulator || echo "")"
AVDMANAGER="${AVDMANAGER_BIN:-avdmanager}"
SUDO="${SUDO_BIN:-sudo}"

echo "=== 硬件虚拟化 ==="
if [ ! -e "$KVM_PATH" ]; then
    echo "::error::$KVM_PATH 不存在：runner 未提供嵌套虚拟化，模拟器无法硬件加速"
    uname -a
    ls /dev | grep -i kvm || echo "（/dev 下无 kvm 节点）"
    exit 1
fi
ls -la "$KVM_PATH"

if [ ! -r "$KVM_PATH" ] || [ ! -w "$KVM_PATH" ]; then
    echo "[kvm] 当前用户（$(id -un)）不可读写，尝试 sudo chmod 666（CI runner 一次性环境）"
    if command -v "$SUDO" > /dev/null 2>&1; then
        "$SUDO" chmod 666 "$KVM_PATH" 2>&1 || echo "::warning::chmod 失败"
    else
        echo "::warning::没有 sudo，无法修正 $KVM_PATH 权限"
    fi
    ls -la "$KVM_PATH"
fi
if [ -r "$KVM_PATH" ] && [ -w "$KVM_PATH" ]; then
    echo "[kvm] 可读写 ok"
else
    echo "::error::$KVM_PATH 仍不可读写，模拟器无法使用硬件加速"
    id; grep -E '^kvm:' /etc/group || true
    exit 1
fi
echo "[kvm] CPU 虚拟化标志: $(grep -cE 'vmx|svm' /proc/cpuinfo 2>/dev/null || echo 0) 行"

echo "=== emulator 二进制 ==="
if [ -z "$EMU" ] || [ ! -x "$EMU" ]; then
    echo "::error::找不到 emulator 可执行文件（SDK=$SDK）"
    ls "$SDK" 2>&1 || true
    exit 1
fi
echo "[emu] $EMU"

# 动态库校验：直接 ldd qemu 二进制，缺什么列什么（比启动后猜错误更快）
QEMU="$SDK/emulator/qemu/linux-x86_64/qemu-system-x86_64"
if [ -x "$QEMU" ]; then
    MISSING=$(timeout 30 ldd "$QEMU" 2>/dev/null | grep "not found" | awk '{print $1}' | sort -u || true)
    if [ -n "$MISSING" ]; then
        echo "::error::qemu 缺少动态库：$(echo "$MISSING" | tr '\n' ' ')"
        echo "  修复：sudo apt-get install -y libpulse0 libnss3 libxdamage1 libxcomposite1 libxcursor1 libxi6 libxtst6 libglu1-mesa"
        exit 1
    fi
    echo "[emu] qemu 动态库完整"
else
    echo "::warning::未找到 $QEMU，跳过动态库校验"
fi
timeout 30 "$EMU" -version 2>&1 | head -5 || true

echo "=== 系统镜像 ==="
ls -d "$SDK"/system-images/*/*/* 2>/dev/null || echo "（无，需 sdkmanager 安装）"

echo "=== 已有 AVD（用 avdmanager，不用 emulator -list-avds）==="
echo "  注：emulator -list-avds 在缺动态库时会静默失败，不能用来判断 AVD 是否存在。"
timeout 30 "$AVDMANAGER" list avd 2>&1 | sed -n '1,25p' || true

echo "=== accel-check ==="
ACCEL_OUT="$(timeout 30 "$EMU" -accel-check 2>&1 || true)"
echo "$ACCEL_OUT" | head -12

if echo "$ACCEL_OUT" | grep -qiE "is installed and usable"; then
    echo "[kvm] 加速可用"
elif echo "$ACCEL_OUT" | grep -qiE "permission"; then
    echo "::error::accel-check 仍报权限问题（上面的 chmod 没生效？）"
    exit 1
elif echo "$ACCEL_OUT" | grep -qiE "not installed|not supported|cannot open"; then
    echo "::error::emulator -accel-check 明确报告加速不可用（见上），停止以免空耗 8 分钟"
    exit 1
else
    echo "::warning::accel-check 输出无法判定（格式未知），继续尝试启动"
fi

echo "=== 前置检查通过 ==="
