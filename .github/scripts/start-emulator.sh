#!/usr/bin/env bash
# 启动 Android 模拟器并等到 boot_completed=1。
#
# 上一轮事故（run 39422464400）：用无超时的 `adb wait-for-device` 等设备；模拟器
# 其实没起来，于是作业挂了 25 分 38 秒直到被人工取消，emulator.log 一行都没打出来。
#
# 三条硬约束：
#   1) 任何等待都有上限：总预算 BOOT_TIMEOUT（默认 480s），每条外部命令也各自 timeout；
#   2) 每轮打印一行状态（进程存活 / adb 设备数 / boot_completed / emulator.log 尾行），
#      运行中就能看出卡在哪一环；
#   3) 所有失败路径都打印 emulator.log 尾部——模拟器自己的报错才是根因。
#
# 用法：BOOT_TIMEOUT=480 bash start-emulator.sh [avd_name] [api_level]
# 可测试性：EMU_BIN / ADB_BIN / AVDMANAGER_BIN / POLL_INTERVAL / EMULATOR_LOG 可覆盖。

set -uo pipefail

AVD_NAME="${1:-womic_interop}"
API_LEVEL="${2:-android-34}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-480}"
POLL_INTERVAL="${POLL_INTERVAL:-10}"
LOG="${EMULATOR_LOG:-/tmp/emulator.log}"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
ADB="${ADB_BIN:-$(command -v adb || echo "$SDK/platform-tools/adb")}"
EMU="${EMU_BIN:-$SDK/emulator/emulator}"
[ -x "$EMU" ] || EMU="$(command -v emulator || echo "")"
AVDMANAGER="${AVDMANAGER_BIN:-avdmanager}"

# ---- AVD 目录统一（run 39462796356 实证的坑）----
# ubuntu-24.04 上 avdmanager 会把 AVD 建到 $XDG_CONFIG_HOME/.android/avd
# （实测 /home/runner/.config/.android/avd），而 emulator 只查
# $ANDROID_AVD_HOME → $ANDROID_SDK_HOME/avd → $HOME/.android/avd，
# 于是报 "Unknown AVD name / HOME is defined but there is no file <avd>.ini"。
# 显式把两个工具的 AVD 根目录钉死到同一处。
export ANDROID_USER_HOME="${ANDROID_USER_HOME:-$HOME/.android}"
export ANDROID_AVD_HOME="${ANDROID_AVD_HOME:-$ANDROID_USER_HOME/avd}"
mkdir -p "$ANDROID_AVD_HOME"
echo "[emu] ANDROID_AVD_HOME=$ANDROID_AVD_HOME"

if [ -z "$EMU" ] || [ ! -x "$EMU" ]; then
    echo "::error::找不到 emulator 可执行文件（SDK=$SDK）"
    exit 1
fi
echo "[emu] binary : $EMU"
echo "[emu] adb    : $ADB"
echo "[emu] avd    : $AVD_NAME (system-images;$API_LEVEL;default;x86_64)"
echo "[emu] 预算   : ${BOOT_TIMEOUT}s，轮询间隔 ${POLL_INTERVAL}s"

avd_exists() {
    # 与 emulator 的查找逻辑对齐：它认的就是 <ANDROID_AVD_HOME>/<name>.ini。
    # 不用 `emulator -list-avds`（缺动态库时会静默返回空列表，见 run 39459335348），
    # 也不用 `avdmanager list avd`（它可能看的是另一个 AVD 根目录，见 run 39462796356）。
    [ -f "$ANDROID_AVD_HOME/$AVD_NAME.ini" ]
}

if avd_exists; then
    echo "[emu] AVD 已存在（$ANDROID_AVD_HOME/$AVD_NAME.ini），复用"
else
    echo "[emu] 创建 AVD"
    echo "no" | timeout 120 "$AVDMANAGER" create avd -n "$AVD_NAME" \
        -k "system-images;${API_LEVEL};default;x86_64" -d pixel_6 --force \
        || echo "no" | timeout 120 "$AVDMANAGER" create avd -n "$AVD_NAME" \
        -k "system-images;${API_LEVEL};default;x86_64" --force
    if ! avd_exists; then
        echo "::error::AVD 创建失败（$ANDROID_AVD_HOME/$AVD_NAME.ini 不存在）"
        echo "--- avdmanager list avd ---"
        timeout 30 "$AVDMANAGER" list avd 2>&1 | sed -n '1,30p' || true
        echo "--- AVD 根目录内容 ---"
        ls -la "$ANDROID_AVD_HOME" 2>&1 || true
        exit 1
    fi
fi
echo "--- AVD 根目录 ---"
ls -la "$ANDROID_AVD_HOME" 2>&1 | head -10 || true

: > "$LOG"
"$EMU" -avd "$AVD_NAME" \
    -no-window -no-audio -no-boot-anim -no-snapshot -no-metrics \
    -accel on -gpu swiftshader_indirect -memory 2048 \
    -camera-back none -camera-front none \
    -verbose \
    > "$LOG" 2>&1 &
EMU_PID=$!
echo "[emu] pid=$EMU_PID 日志=$LOG"

timeout 60 "$ADB" start-server > /dev/null 2>&1 || true

START=$SECONDS
BOOT=""
while [ $((SECONDS - START)) -lt "$BOOT_TIMEOUT" ]; do
    ELAPSED=$((SECONDS - START))

    if ! kill -0 "$EMU_PID" 2>/dev/null; then
        wait "$EMU_PID" 2>/dev/null
        RC=$?
        echo "::error::模拟器进程已退出（pid=$EMU_PID，rc=$RC，运行 ${ELAPSED}s）"
        echo "===== emulator.log (tail 120) ====="
        tail -120 "$LOG" 2>/dev/null || echo "(无日志)"
        echo "===== adb devices ====="
        timeout 20 "$ADB" devices -l || true
        exit 1
    fi

    DEV_COUNT=$(timeout 20 "$ADB" devices 2>/dev/null | sed -n '2,$p' | grep -cE '[[:space:]]device$' || true)
    BOOT_RAW=$(timeout 15 "$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)
    [ -n "$BOOT_RAW" ] && BOOT="$BOOT_RAW"
    TAIL=$(tail -1 "$LOG" 2>/dev/null | cut -c1-140)

    echo "[emu ${ELAPSED}s] alive=1 adb_devices=${DEV_COUNT:-0} boot_completed=${BOOT_RAW:-<none>} | $TAIL"

    if [ "$BOOT_RAW" = "1" ]; then
        BOOT="1"
        echo "[emu] boot_completed=1，用时 ${ELAPSED}s"
        break
    fi
    sleep "$POLL_INTERVAL"
done

if [ "$BOOT" != "1" ]; then
    echo "::error::模拟器 ${BOOT_TIMEOUT}s 内未完成启动"
    echo "===== adb devices ====="
    timeout 20 "$ADB" devices -l || true
    echo "===== emulator.log (tail 150) ====="
    tail -150 "$LOG" 2>/dev/null || echo "(无日志)"
    echo "===== 进程是否仍存活 ====="
    if kill -0 "$EMU_PID" 2>/dev/null; then echo "emulator 仍在运行（pid=$EMU_PID）"; else echo "emulator 已退出"; fi
    exit 1
fi

echo "===== 模拟器就绪 ====="
timeout 20 "$ADB" devices -l || true
echo "[emu] SDK: $(timeout 15 "$ADB" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')"
echo "[emu] ABI: $(timeout 15 "$ADB" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
timeout 20 "$ADB" shell wm dismiss-keyguard > /dev/null 2>&1 || true
timeout 20 "$ADB" shell input keyevent 82 > /dev/null 2>&1 || true
