#!/usr/bin/env bash
# .github/scripts 下 CI 脚本的单元测试。
#
# 为什么需要：这些脚本的失败模式是"挂死"而不是"报错"——上一轮 CI 就因为
# 无超时的 `adb wait-for-device` 和没有 timeout 的诊断命令挂了 25+ 分钟。
# 挂死类缺陷无法靠看日志发现，只能用 stub 假命令在本地构造这些场景验证。
#
# 运行：bash .github/scripts/tests/run-tests.sh
# 依赖：bash、coreutils（timeout）、tar。不需要 adb/emulator/Java/Android SDK。

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$(cd "$HERE/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0; FAIL=0; FAILED_NAMES=()

ok()   { PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES+=("$1"); echo "  [FAIL] $1${2:+ -- $2}"; }

assert_eq()      { [ "$1" = "$2" ] && ok "$3" || bad "$3" "期望 [$2]，实得 [$1]"; }
assert_contains(){ case "$1" in *"$2"*) ok "$3";; *) bad "$3" "输出中未找到 [$2]";; esac; }
assert_not_contains(){ case "$1" in *"$2"*) bad "$3" "输出中不应出现 [$2]";; *) ok "$3";; esac; }
assert_lt()      { [ "$1" -lt "$2" ] && ok "$3" || bad "$3" "$1 不小于 $2"; }

# ---------------------------------------------------------------------------
# stubs：假 adb / emulator / avdmanager
# ---------------------------------------------------------------------------
STUB="$TMP/bin"
mkdir -p "$STUB"

cat > "$STUB/adb" <<'STUB_EOF'
#!/usr/bin/env bash
# FAKE_ADB_MODE: none | online | boot | hang
# hang 模式：除 devices/start-server 外所有子命令都挂住（模拟 adb server 卡死）
mode="${FAKE_ADB_MODE:-none}"
if [ "${FAKE_ADB_LOG:-}" ]; then echo "adb $*" >> "$FAKE_ADB_LOG"; fi
case "${1:-}" in
  start-server) exit 0 ;;
  devices)
    echo "List of devices attached"
    if [ "$mode" = "online" ] || [ "$mode" = "boot" ] || [ "$mode" = "hang" ]; then
      printf 'emulator-5554\tdevice\n'
    fi
    exit 0 ;;
  *)
    [ "$mode" = "hang" ] && exec sleep 600
    if [ "${1:-}" = "shell" ] && [ "${2:-}" = "getprop" ]; then
      [ "$mode" = "boot" ] && echo "1" || echo ""
    fi
    exit 0 ;;
esac
STUB_EOF

cat > "$STUB/emulator" <<'STUB_EOF'
#!/usr/bin/env bash
# FAKE_EMU_MODE: ok | crash | alive ; FAKE_EMU_ACCEL: ok | noaccel
mode="${FAKE_EMU_MODE:-ok}"
for a in "$@"; do
  case "$a" in
    -list-avds) echo "womic_interop"; exit 0 ;;
    -version)   echo "Android emulator version 36.0.0.0 (fake)"; exit 0 ;;
    -accel-check)
      if [ "${FAKE_EMU_ACCEL:-ok}" = "ok" ]; then
        echo "accel:"; echo "0"; echo "KVM (version 12) is installed and usable."
      else
        echo "accel:"; echo "1"; echo "KVM is not installed on this machine"
      fi
      exit 0 ;;
  esac
done
case "$mode" in
  crash) echo "fake emulator: ERROR: x86_64 emulation currently requires hardware acceleration" >&2
         exit 1 ;;
  alive) exec sleep 600 ;;
  *)     exec sleep 600 ;;
esac
STUB_EOF

cat > "$STUB/avdmanager" <<'STUB_EOF'
#!/usr/bin/env bash
# FAKE_AVD_EXISTS: 非空时在 list 输出中包含该 AVD 名（模拟"已存在"）
if [ "${1:-}" = "create" ]; then
  [ -n "${FAKE_AVD_LOG:-}" ] && echo "create $*" >> "$FAKE_AVD_LOG"
  exit 0
fi
echo "Available Android Virtual Devices:"
if [ -n "${FAKE_AVD_EXISTS:-}" ]; then
  echo "    Name: ${FAKE_AVD_EXISTS}"
  echo "  Device: pixel_6 (Google)"
  echo "    Path: /home/runner/.config/.android/avd/${FAKE_AVD_EXISTS}.avd"
fi
exit 0
STUB_EOF

cat > "$STUB/curl" <<'STUB_EOF'
#!/usr/bin/env bash
# 一律失败，用来验证"下载失败不会静默通过"
echo "fake curl: network disabled" >&2
exit 22
STUB_EOF

chmod +x "$STUB"/*

# ---------------------------------------------------------------------------
echo "== T01 setup-android-sdk: 定位已有 cmdline-tools =="
FAKE_SDK="$TMP/sdk1"
mkdir -p "$FAKE_SDK/cmdline-tools/latest/bin" "$FAKE_SDK/platform-tools"
printf '#!/bin/sh\necho "19.0"\n' > "$FAKE_SDK/cmdline-tools/latest/bin/sdkmanager"
chmod +x "$FAKE_SDK/cmdline-tools/latest/bin/sdkmanager"
out=$(SDK_ROOT="$FAKE_SDK" GITHUB_ENV= GITHUB_PATH= bash "$SCRIPTS/setup-android-sdk.sh" 2>&1); rc=$?
assert_eq "$rc" "0" "T01a 退出码 0"
assert_contains "$out" "使用 SDK: $FAKE_SDK" "T01b 打印选中的 SDK"
assert_contains "$out" "sdkmanager --version: 19.0" "T01c 能执行 sdkmanager"

echo "== T02 setup-android-sdk: 缺 cmdline-tools 且下载失败时必须报错 =="
FAKE_SDK2="$TMP/sdk2"; mkdir -p "$FAKE_SDK2"
out=$(PATH="$STUB:$PATH" SDK_ROOT="$FAKE_SDK2" GITHUB_ENV= GITHUB_PATH= bash "$SCRIPTS/setup-android-sdk.sh" 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T02a 退出码非 0（不静默通过）" || bad "T02a 退出码非 0" "实得 $rc"

echo "== T03 verify-kvm: /dev/kvm 不存在 → 快速失败 =="
start=$SECONDS
out=$(KVM_PATH="$TMP/no-such-kvm" EMU_BIN="$STUB/emulator" bash "$SCRIPTS/verify-kvm.sh" 2>&1); rc=$?
assert_eq "$rc" "1" "T03a 退出码 1"
assert_contains "$out" "::error::" "T03b 输出 ::error::"
assert_lt $((SECONDS-start)) 10 "T03c 在 10s 内结束"

echo "== T04 verify-kvm: accel 可用 → 通过 =="
touch "$TMP/kvm-ok"
out=$(KVM_PATH="$TMP/kvm-ok" EMU_BIN="$STUB/emulator" AVDMANAGER_BIN="$STUB/avdmanager" FAKE_EMU_ACCEL=ok bash "$SCRIPTS/verify-kvm.sh" 2>&1); rc=$?
assert_eq "$rc" "0" "T04a 退出码 0"
assert_contains "$out" "加速可用" "T04b 报告加速可用"

echo "== T05 verify-kvm: accel-check 明确不可用 → 立刻失败（不等 8 分钟） =="
out=$(KVM_PATH="$TMP/kvm-ok" EMU_BIN="$STUB/emulator" AVDMANAGER_BIN="$STUB/avdmanager" FAKE_EMU_ACCEL=noaccel bash "$SCRIPTS/verify-kvm.sh" 2>&1); rc=$?
assert_eq "$rc" "1" "T05a 退出码 1"
assert_contains "$out" "加速不可用" "T05b 给出明确原因"

echo "== T06 start-emulator: 模拟器进程立刻崩溃 → 快速失败并打印自身日志 =="
start=$SECONDS
out=$(EMU_BIN="$STUB/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu6.log" FAKE_EMU_MODE=crash FAKE_ADB_MODE=none FAKE_AVD_EXISTS=womic_interop BOOT_TIMEOUT=60 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1); rc=$?
assert_eq "$rc" "1" "T06a 退出码 1"
assert_contains "$out" "模拟器进程已退出" "T06b 捕获进程退出"
assert_contains "$out" "requires hardware acceleration" "T06c 打印 emulator 自身报错"
assert_lt $((SECONDS-start)) 20 "T06d 20s 内结束（不会挂住）"

echo "== T07 start-emulator: 进程活着但永不 boot → 到点退出 =="
start=$SECONDS
out=$(EMU_BIN="$STUB/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu7.log" FAKE_EMU_MODE=alive FAKE_ADB_MODE=online FAKE_AVD_EXISTS=womic_interop BOOT_TIMEOUT=6 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1); rc=$?
elapsed=$((SECONDS-start))
assert_eq "$rc" "1" "T07a 退出码 1"
assert_contains "$out" "未完成启动" "T07b 明确报告超时"
assert_contains "$out" "boot_completed" "T07c 过程中有进度日志"
assert_lt "$elapsed" 25 "T07d 在预算内退出（实测 ${elapsed}s）"

echo "== T08 start-emulator: 正常 boot → 通过 =="
start=$SECONDS
out=$(EMU_BIN="$STUB/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu8.log" FAKE_EMU_MODE=alive FAKE_ADB_MODE=boot FAKE_AVD_EXISTS=womic_interop BOOT_TIMEOUT=15 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1); rc=$?
assert_eq "$rc" "0" "T08a 退出码 0"
assert_contains "$out" "boot_completed=1" "T08b 检测到启动完成"
assert_lt $((SECONDS-start)) 12 "T08c 快速完成"

echo "== T09 start-emulator: AVD 不存在时要调用 avdmanager 创建 =="
mkdir -p "$TMP/bin2"
cat > "$TMP/bin2/emulator" <<'EOF'
#!/usr/bin/env bash
for a in "$@"; do [ "$a" = "-list-avds" ] && exit 0; done
exec sleep 600
EOF
chmod +x "$TMP/bin2/emulator"
AVD_LOG="$TMP/avd.log"; : > "$AVD_LOG"
out=$(EMU_BIN="$TMP/bin2/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu9.log" FAKE_AVD_LOG="$AVD_LOG" FAKE_ADB_MODE=none BOOT_TIMEOUT=3 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1)
assert_contains "$(cat "$AVD_LOG")" "create" "T09a 调用了 avdmanager create avd"

echo "== T09b start-emulator: AVD 已存在时不得重复创建 =="
AVD_LOG2="$TMP/avd2.log"; : > "$AVD_LOG2"
out=$(EMU_BIN="$TMP/bin2/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu9b.log" FAKE_AVD_LOG="$AVD_LOG2" FAKE_AVD_EXISTS=womic_interop \
      FAKE_ADB_MODE=boot BOOT_TIMEOUT=5 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1)
assert_eq "$(cat "$AVD_LOG2")" "" "T09b 未调用 create（复用已有 AVD）"
assert_contains "$out" "AVD 已存在" "T09c 日志说明复用"

echo "== T10 collect-diagnostics: adb 挂死时仍在上限内结束 =="
start=$SECONDS
out=$(ADB_BIN="$STUB/adb" DIAG_DIR="$TMP/diag" ADB_TIMEOUT=2 FAKE_ADB_MODE=hang \
      bash "$SCRIPTS/collect-diagnostics.sh" 2>&1); rc=$?
elapsed=$((SECONDS-start))
assert_eq "$rc" "0" "T10a 退出码 0（诊断失败不影响作业结论）"
assert_contains "$out" "诊断收集完成" "T10b 跑完全部条目"
assert_lt "$elapsed" 60 "T10c 60s 内结束（实测 ${elapsed}s）"
assert_contains "$(cat "$TMP/diag/logcat.txt" 2>/dev/null)" "超时" "T10d 记录到超时而不是静默"

echo "== T11 fetch-opus: 已有完整源码时跳过下载 =="
FAKE_ANDROID="$TMP/android11"
mkdir -p "$FAKE_ANDROID/app/src/main/cpp/opus/include" "$FAKE_ANDROID/app/src/main/cpp/opus/src"
touch "$FAKE_ANDROID/app/src/main/cpp/opus/CMakeLists.txt" "$FAKE_ANDROID/app/src/main/cpp/opus/include/opus.h"
out=$(PATH="$STUB:$PATH" ANDROID_DIR="$FAKE_ANDROID" bash "$SCRIPTS/fetch-opus.sh" 2>&1); rc=$?
assert_eq "$rc" "0" "T11a 退出码 0"
assert_contains "$out" "已存在完整源码" "T11b 走了跳过分支"

echo "== T12 fetch-opus: OPUS_FORCE=1 时从 OPUS_URL 重新解压 =="
FAKE_SRC="$TMP/src12"; mkdir -p "$FAKE_SRC/opus-1.5.2/include" "$FAKE_SRC/opus-1.5.2/src"
touch "$FAKE_SRC/opus-1.5.2/CMakeLists.txt" "$FAKE_SRC/opus-1.5.2/include/opus.h"
printf 'AC_INIT([opus],[1.5.2])\n' > "$FAKE_SRC/opus-1.5.2/configure.ac"
( cd "$FAKE_SRC" && tar -czf "$TMP/opus12.tar.gz" opus-1.5.2 )
FAKE_ANDROID12="$TMP/android12"; mkdir -p "$FAKE_ANDROID12"
# 注意：这里必须用真实 curl（$STUB 里的 curl 是"永远失败"的假货，专给 T13 用）
out=$(ANDROID_DIR="$FAKE_ANDROID12" OPUS_FORCE=1 OPUS_URL="file://$TMP/opus12.tar.gz" \
      bash "$SCRIPTS/fetch-opus.sh" 2>&1); rc=$?
assert_eq "$rc" "0" "T12a 退出码 0"
assert_contains "$out" "源码版本: opus-1.5.2" "T12b 报告版本"
[ -f "$FAKE_ANDROID12/app/src/main/cpp/opus/CMakeLists.txt" ] && ok "T12c 文件已就位" || bad "T12c 文件已就位"

echo "== T13 fetch-opus: 下载失败时必须非 0 =="
FAKE_ANDROID13="$TMP/android13"; mkdir -p "$FAKE_ANDROID13"
out=$(PATH="$STUB:$PATH" ANDROID_DIR="$FAKE_ANDROID13" bash "$SCRIPTS/fetch-opus.sh" 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T13a 退出码非 0" || bad "T13a 退出码非 0" "实得 $rc"
assert_contains "$out" "::error::" "T13b 输出 ::error::"

# ---------------------------------------------------------------------------
echo
echo "================ 单元测试结果 ================"
echo "  通过 $PASS，失败 $FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '  失败用例: %s\n' "${FAILED_NAMES[*]}"
    exit 1
fi
echo "=============================================="
exit 0
