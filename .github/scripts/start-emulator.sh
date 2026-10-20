#!/usr/bin/env bash
# 启动 Android 模拟器并等到 boot_completed=1。
#
# 上一轮事故（run 39422464400）：用 `adb wait-for-device` 等设备，它没有任何
# 超时；模拟器其实根本没起来，于是作业挂了 25 分 38 秒直到被人工取消，且
# emulator.log 一行都没打出来，无法定位。
#
# 本脚本的三条硬约束：
#   1) 绝不做无超时的等待：总预算 BOOT_TIMEOUT（默认 480s）到点必退出；
#   2) 每 10s 打一行状态（进程存活 / adb 设备数 / boot_completed / 日志尾行），
#      运行过程中在 Actions 日志里就能看出卡在哪一环；
#   3) 任何失败路径都打印 emulator.log 尾部，直接给出模拟器自己的报错。
#
# 用法：bash start-emulator.sh [avd_name] [api_level] [boot_timeout_s]

set -uo pipefail

AVD_NAME="${1:-womic_interop}"
API_LEVEL="${2:-android-34}"
BOOT_TIMEOUT="${3:-480}"
LOG=/tmp/emulator.log

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
ADB="$(command -v adb || echo "$SDK/platform-tools/adb")"
EMU="$SDK/emulator/emulator"
[ -x "$EMU" ] || EMU="$(command -v emulator || echo "")"
if [ -z "$EMU" ] || [ ! -x "$EMU" ]; then
    echo "::error::找不到 emulator 可执行文件（SDK=$SDK）"
    exit 1
fi
echo "[emu] binary : $EMU"
echo "[emu] adb    : $ADB"
echo "[emu] avd    : $AVD_NAME (system-images;$API_LEVEL;default;x86_64)"
echo "[emu] 预算   : ${BOOT_TIMEOUT}s"

# --- 创建 AVD（幂等：已存在就跳过）---
if "$EMU" -list-avds 2>/dev/null | grep -qx "$AVD_NAME"; then
    echo "[emu] AVD 已存在，复用"
else
    echo "[emu] 创建 AVD"
    echo "no" | avdmanager create avd -n "$AVD_NAME" \
        -k "system-images;${API_LEVEL};default;x86_64" -d pixel_6 --force \
        || echo "no" | avdmanager create avd -n "$AVD_NAME" \
        -k "system-images;${API_LEVEL};default;x86_64" --force
    if ! "$EMU" -list-avds 2>/dev/null | grep -qx "$AVD_NAME"; then
        echo "::error::AVD 创建失败"
        avdmanager list avd 2>&1 | sed -n '1,30p'
        exit 1
    fi
fi
"$EMU" -list-avds 2>&1 | sed 's/^/[emu] avd: /'

# --- 启动 ---
: > "$LOG"
"$EMU" -avd "$AVD_NAME" \
    -no-window -no-audio -no-boot-anim -no-snapshot -no-metrics \
    -accel on -gpu swiftshader_indirect -memory 2048 \
    -camera-back none -camera-front none \
    -verbose \
    > "$LOG" 2>&1 &
EMU_PID=$!
echo "[emu] pid=$EMU_PID 日志=$LOG"

"$ADB" start-server > /dev/null 2>&1 || true

START=$SECONDS
BOOT=""
while [ $((SECONDS - START)) -lt "$BOOT_TIMEOUT" ]; do
    ELAPSED=$((SECONDS - START))

    # 1) 进程是否还活着（最常见：缺库 / accel 不可用直接退出）
    if ! kill -0 "$EMU_PID" 2>/dev/null; then
        wait "$EMU_PID" 2>/dev/null
        RC=$?
        echo "::error::模拟器进程已退出（pid=$EMU_PID，rc=$RC，运行 ${ELAPSED}s）"
        echo "===== emulator.log (tail 120) ====="
        tail -120 "$LOG"
        echo "===== adb devices ====="
        "$ADB" devices -l || true
        exit 1
    fi

    # 2) adb 是否看到设备 / 是否完成启动
    DEV_COUNT=$("$ADB" devices 2>/dev/null | sed -n '2,$p' | grep -cE '[[:space:]]device$' || true)
    BOOT_RAW=$(timeout 10 "$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)
    [ -n "$BOOT_RAW" ] && BOOT="$BOOT_RAW"
    TAIL=$(tail -1 "$LOG" 2>/dev/null | cut -c1-140)

    echo "[emu ${ELAPSED}s] alive=1 adb_devices=${DEV_COUNT:-0} boot_completed=${BOOT_RAW:-<none>} | $TAIL"

    if [ "$BOOT_RAW" = "1" ]; then
        BOOT="1"
        echo "[emu] boot_completed=1，用时 ${ELAPSED}s"
        break
    fi
    sleep 10
done

if [ "$BOOT" != "1" ]; then
    echo "::error::模拟器 ${BOOT_TIMEOUT}s 内未完成启动"
    echo "===== adb devices ====="
    "$ADB" devices -l || true
    echo "===== emulator.log (tail 150) ====="
    tail -150 "$LOG"
    exit 1
fi

echo "===== 模拟器就绪 ====="
"$ADB" devices -l
echo "[emu] SDK: $("$ADB" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')"
echo "[emu] ABI: $("$ADB" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
# 解锁屏幕，避免 Activity 停在 keyguard 后面
"$ADB" shell wm dismiss-keyguard > /dev/null 2>&1 || true
"$ADB" shell input keyevent 82 > /dev/null 2>&1 || true
