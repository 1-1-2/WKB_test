#!/usr/bin/env bash
# 安装 APK 并以"调试音模式"启动采集服务（每步都有超时与输出）。
#
# 说明：
#   - `adb install -g` 安装即授予 RECORD_AUDIO / POST_NOTIFICATIONS 等运行时权限；
#   - 模拟器镜像是 userdebug，`adb root` 后 `am start-service` 不受后台启动限制；
#   - 先拉 MainActivity（前台 + UI 冒烟），再启服务；
#   - 调试音模式不打开 AudioRecord，CI 无麦克风也能跑通整条媒体链路。
#
# 用法：bash install-app.sh <apk_path>

set -uo pipefail

APK="${1:-../apk/app-debug.apk}"
PKG=com.miclink.open

# 允许从任意目录调用：脚本在 <repo>/.github/scripts/ 下
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$APK" ] || APK="$REPO_ROOT/apk/app-debug.apk"

if [ ! -f "$APK" ]; then
    echo "::error::APK 不存在: $APK"
    echo "--- 当前目录 ---"; pwd; ls -la; echo "--- 上级 ---"; ls -la ..
    exit 1
fi
echo "[app] APK: $(ls -la "$APK")"

echo "[app] adb root"
timeout 60 adb root 2>&1 || echo "[app] adb root 失败（非 userdebug 镜像？继续）"
timeout 60 adb wait-for-device 2>&1 || echo "[app] wait-for-device 超时，继续尝试"

echo "[app] 安装"
if ! timeout 240 adb install -r -g "$APK" 2>&1; then
    echo "::error::APK 安装失败"
    adb shell pm list packages 2>/dev/null | grep -i miclink || true
    adb logcat -d -t 50 2>/dev/null | tail -20 || true
    exit 1
fi
adb shell pm list packages 2>/dev/null | grep -i "$PKG" || echo "[app] 警告：包列表里没找到 $PKG"

echo "[app] 前台 Activity"
timeout 90 adb shell am start -n "$PKG/.MainActivity" 2>&1 || echo "[app] MainActivity 启动返回非 0（继续）"
sleep 5
echo "[app] 进程 pid: $(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r' || echo '<无>')"

echo "[app] 启动采集服务（调试音模式）"
timeout 90 adb shell am start-service -n "$PKG/.MicCaptureService" \
    -a "$PKG.action.START" \
    --ei extra.control_port 8125 \
    --ei extra.volume 5 \
    --ez extra.debug_tone true 2>&1 || echo "[app] start-service 返回非 0（继续，下面看状态）"
sleep 3

echo "[app] 服务记录"
adb shell dumpsys activity services "$PKG" 2>/dev/null | grep -iE "ServiceRecord|isForeground|app=|createTime" | head -12 || true

echo "[app] 监听端口（/proc/net/tcp 中 8125 = 1FBD）"
adb shell "cat /proc/net/tcp 2>/dev/null | awk 'NR>1 {print \$2}' | grep -i ':1FBD' || echo '(未监听)'" || true

echo "[app] 最近 40 行 logcat"
adb logcat -d -t 40 2>/dev/null | tail -40 || true
