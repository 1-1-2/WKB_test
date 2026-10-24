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
# create 时按真实行为在 $ANDROID_AVD_HOME 下写 <name>.ini（emulator 就是靠它找 AVD）
if [ "${1:-}" = "create" ]; then
  name=""
  while [ $# -gt 0 ]; do
    case "$1" in -n) name="$2"; shift 2;; *) shift;; esac
  done
  [ -n "${FAKE_AVD_LOG:-}" ] && echo "create name=$name" >> "$FAKE_AVD_LOG"
  if [ -n "${ANDROID_AVD_HOME:-}" ] && [ -n "$name" ]; then
    mkdir -p "$ANDROID_AVD_HOME"; : > "$ANDROID_AVD_HOME/$name.ini"
  fi
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

# "AVD 已存在"与"AVD 不存在"两个根目录（start-emulator.sh 用 <root>/<name>.ini 判断）
AVDH="$TMP/avdh"; mkdir -p "$AVDH"; : > "$AVDH/womic_interop.ini"
AVDH_EMPTY="$TMP/avdh_empty"; mkdir -p "$AVDH_EMPTY"

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
      EMULATOR_LOG="$TMP/emu6.log" FAKE_EMU_MODE=crash FAKE_ADB_MODE=none ANDROID_AVD_HOME="$AVDH" BOOT_TIMEOUT=60 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1); rc=$?
assert_eq "$rc" "1" "T06a 退出码 1"
assert_contains "$out" "模拟器进程已退出" "T06b 捕获进程退出"
assert_contains "$out" "requires hardware acceleration" "T06c 打印 emulator 自身报错"
assert_lt $((SECONDS-start)) 20 "T06d 20s 内结束（不会挂住）"

echo "== T07 start-emulator: 进程活着但永不 boot → 到点退出 =="
start=$SECONDS
out=$(EMU_BIN="$STUB/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu7.log" FAKE_EMU_MODE=alive FAKE_ADB_MODE=online ANDROID_AVD_HOME="$AVDH" BOOT_TIMEOUT=6 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1); rc=$?
elapsed=$((SECONDS-start))
assert_eq "$rc" "1" "T07a 退出码 1"
assert_contains "$out" "未完成启动" "T07b 明确报告超时"
assert_contains "$out" "boot_completed" "T07c 过程中有进度日志"
assert_lt "$elapsed" 25 "T07d 在预算内退出（实测 ${elapsed}s）"

echo "== T08 start-emulator: 正常 boot → 通过 =="
start=$SECONDS
out=$(EMU_BIN="$STUB/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu8.log" FAKE_EMU_MODE=alive FAKE_ADB_MODE=boot ANDROID_AVD_HOME="$AVDH" BOOT_TIMEOUT=15 POLL_INTERVAL=1 \
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
      EMULATOR_LOG="$TMP/emu9.log" FAKE_AVD_LOG="$AVD_LOG" FAKE_ADB_MODE=none \
      ANDROID_AVD_HOME="$AVDH_EMPTY" BOOT_TIMEOUT=3 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1)
assert_contains "$(cat "$AVD_LOG")" "create name=womic_interop" "T09a 调用了 avdmanager create avd"
[ -f "$AVDH_EMPTY/womic_interop.ini" ] && ok "T09b create 后 ini 就位（emulator 能找到）" \
    || bad "T09b create 后 ini 就位" "缺少 $AVDH_EMPTY/womic_interop.ini"

echo "== T09c start-emulator: AVD 已存在时不得重复创建 =="
AVD_LOG2="$TMP/avd2.log"; : > "$AVD_LOG2"
out=$(EMU_BIN="$TMP/bin2/emulator" ADB_BIN="$STUB/adb" AVDMANAGER_BIN="$STUB/avdmanager" \
      EMULATOR_LOG="$TMP/emu9b.log" FAKE_AVD_LOG="$AVD_LOG2" ANDROID_AVD_HOME="$AVDH" \
      FAKE_ADB_MODE=boot BOOT_TIMEOUT=5 POLL_INTERVAL=1 \
      bash "$SCRIPTS/start-emulator.sh" womic_interop android-34 2>&1)
assert_eq "$(cat "$AVD_LOG2")" "" "T09d 未调用 create（复用已有 AVD）"
assert_contains "$out" "AVD 已存在" "T09e 日志说明复用"

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
# T14/T15：pc_client.py 的 AdbUdpTap.restart()
#
# 背景：toybox `nc -u -l` 收到首个数据报后 xconnect 到该对端
# （netcat.c:229），之后内核只交付该对端的包；app 每个会话新建
# DatagramSocket（新源端口）→ 重连会话必须换一个新监听，否则 C15
# udp=none（run 39473353065 实证）。这里用假 adb 验证 restart 的
# 行为契约：换新进程、旧进程退出、parser/queue 换新、起不来时重试。
# ---------------------------------------------------------------------------
REPO_ROOT="$(cd "$SCRIPTS/../.." && pwd)"
TAP_STUB="$TMP/bin/adb-tap"
cat > "$TAP_STUB" <<'STUB_EOF'
#!/usr/bin/env bash
# 假 adb：专供 T14~T21 验证 AdbUdpTap 的启动/restart 行为契约。
#
# 子命令分面：pc_client 的 UDP tap 走 `adb exec-out` 回收二进制 Opus 流
# （shell 会在设备端分配 PTY，ONLCR 把 0x0A 改写成 0D 0A 毁帧——C05 根因，
# 见 pc_client AdbUdpTap 文档字符串）；文本命令（cat/ps/pkill）仍走 shell。
# 本 stub 不模拟 PTY 时对两者行为一致（原样字节）；FAKE_ADB_FORCE_PTY=1
# 时强制按 PTY 通道处理（T21c 验证"通道若是 PTY 帧必被毁"）。
#
# FAKE_ADB_NC_FAILS_ON: 空格分隔的调用序号列表，命中序号的 nc 调用立即退出
#   （模拟"旧 nc 还没释放端口/新 nc 绑定失败"）；其余 nc 调用常驻（sleep）。
# FAKE_ADB_NC_COUNT: nc 调用计数文件路径（配合 FAILS_ON 使用）。
# FAKE_ADB_NC_STATE: "已 bind 端口"状态文件路径——常驻 nc 留下的痕迹：
#   nc 分支 exec sleep 前把端口写进去（FAILS_ON 命中而 exit 1 的分支不写）；
#   pkill 分支（进程被杀 = 端口释放）把对应端口移除；cat /proc/net/udp 分支
#   依据它决定是否输出媒体端口的监听行。以此保证"nc 进程活着"与"/proc/net/udp
#   有该端口行"两个事实始终一致（pc_client 的 bind 就绪探测正是同时看这两者）。
# FAKE_ADB_UDP_PROC_MODE: normal（默认，按状态文件输出 138C 行）|
#   unreadable（cat 报 Permission denied 并非零退出——模拟权限收紧的
#   ROM/SELinux，验证 pc_client 的降级兜底）| no-port（输出形态逼真但始终
#   没有媒体端口行——验证"读得到但没 bind 上"不得误降级）。
state_file="${FAKE_ADB_NC_STATE:-/tmp/fake_adb_tap_state}"

# 记录每次调用的子命令与参数（T20：tap 必须走 exec-out，nc 绝不走 shell）
if [ "${FAKE_ADB_ARGV_LOG:-}" ]; then
  printf '%s %s\n' "${1:-}" "${2:-}" >> "$FAKE_ADB_ARGV_LOG"
fi

# 端口释放（pkill 命中）：从状态文件移除该端口，与 /proc/net/udp 保持一致
port_release() {
  local port="$1" tmp="$state_file.tmp"
  [ -f "$state_file" ] || return 0
  grep -v "^${port}$" "$state_file" > "$tmp" 2>/dev/null || true
  mv "$tmp" "$state_file"
  [ -s "$state_file" ] || rm -f "$state_file"
}

# exec-out 与 shell 等价放行（tap 走 exec-out，文本命令走 shell；PTY 差异
# 由 FAKE_ADB_FORCE_PTY 在 nc 分支按通道模拟，见下）
subcmd="${1:-}"
case "$subcmd" in
  shell|exec-out) shift ;;
  *) exit 0 ;;
esac
  # pkill 的模式串里也含 "nc -u -l" 字样，必须先于 nc 匹配退出，否则挂起
  case "$*" in
    pkill*)
      port=""
      for a in $*; do port="$a"; done
      port="${port//[!0-9]/}"   # 剥掉模式串里的引号等非数字字符
      [ -n "$port" ] && port_release "$port"
      exit 0 ;;
  esac
  case "$*" in
    *"nc -u -l"*|*"nc -ulp"*)
      if [ "${FAKE_ADB_NC_FAILS_ON:-}" ]; then
        cnt_file="${FAKE_ADB_NC_COUNT:-/tmp/fake_adb_tap_count}"
        n=$(( $(cat "$cnt_file" 2>/dev/null || echo 0) + 1 ))
        echo "$n" > "$cnt_file"
        for bad in $FAKE_ADB_NC_FAILS_ON; do
          [ "$n" -eq "$bad" ] && exit 1
        done
      fi
      # 常驻 nc = bind 成功：先留痕迹（端口进状态文件）再 exec sleep
      port=""
      for a in $*; do port="$a"; done
      port="${port//[!0-9]/}"
      [ -n "$port" ] && echo "$port" >> "$state_file"
      # 模拟 app 发来一个二进制格式 C 帧（FAKE_ADB_NC_EMIT_FILE 指定时）：
      # shell 通道 = 设备侧 PTY 开 ONLCR，每个 0x0A 被改写成 0D 0A（毁帧）；
      # exec-out 无 PTY，字节原样。FAKE_ADB_FORCE_PTY=1 可强制走 ONLCR
      # （T21c：证明"通道若是 PTY，帧必被毁"——旧实现红的证据）。
      if [ -n "${FAKE_ADB_NC_EMIT_FILE:-}" ] && [ -f "$FAKE_ADB_NC_EMIT_FILE" ]; then
        if [ "$subcmd" = "shell" ] || [ -n "${FAKE_ADB_FORCE_PTY:-}" ]; then
          python3 -c 'import sys; sys.stdout.buffer.write(sys.stdin.buffer.read().replace(b"\x0a", b"\x0d\x0a"))' < "$FAKE_ADB_NC_EMIT_FILE"
        else
          cat "$FAKE_ADB_NC_EMIT_FILE"
        fi
      fi
      exec sleep 600 ;;
    "cat /proc/net/udp"|"cat /proc/net/udp6")
      case "${FAKE_ADB_UDP_PROC_MODE:-normal}" in
        unreadable)
          # 权限收紧/SELinux：cat 失败并非零退出（stderr 供 pc_client 定位原因）
          echo "cat: ${*#cat }: Permission denied" >&2
          exit 1 ;;
      esac
      # 形态逼真的假 /proc/net/udp：表头 + 无关端口噪声（含一条 st=01 的
      # ESTABLISHED 行，模拟 toybox nc 收到首个包后的 xconnect 状态）
      printf '%s\n' \
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode" \
        "   0: 0100007F:0035 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 21234 1 0000000000000000 100 0" \
        "   1: 00000000:01BB 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 23456 1 0000000000000000 100 0" \
        "   2: 0A00000A:EAF9 0A00000A:0035 01 00000000:00000000 00:00000000 00000000  1000        0 24567 1 0000000000000000 100 0"
      # 当且仅当状态文件里有端口（即有常驻 nc）时，输出媒体端口的 LISTEN 行
      if [ "${FAKE_ADB_UDP_PROC_MODE:-normal}" != "no-port" ] && [ -f "$state_file" ]; then
        idx=3
        while read -r p; do
          [ -n "$p" ] || continue
          printf '   %d: 00000000:%04X 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 34567 1 0000000000000000 100 0\n' "$idx" "$p"
          idx=$((idx+1))
        done < "$state_file"
      fi
      exit 0 ;;
    *) exit 0 ;;
  esac
exit 0
STUB_EOF
chmod +x "$TAP_STUB"

cat > "$TMP/restart_test.py" <<'PY'
import os, sys
sys.path.insert(0, sys.argv[1])
import pc_client

adb = pc_client.Adb(sys.argv[2])
tap = pc_client.AdbUdpTap(adb, 5004)
old_pid = tap.proc.pid
old_parser = tap.parser
try:
    tap.restart()
    assert tap.proc is not None and tap.proc.poll() is None, "restart 后监听应存活"
    new_pid = tap.proc.pid
    assert new_pid != old_pid, "restart 后应换新进程（旧 %d 新 %d）" % (old_pid, new_pid)
    try:
        os.kill(old_pid, 0); alive = True
    except OSError:
        alive = False
    assert not alive, "旧 nc 宿主进程应已退出"
    assert tap.parser is not old_parser, "parser 应换新（杜绝残字节喂新会话）"
    assert tap.queue.empty(), "queue 应为空"
    assert tap.received == 0, "received 应清零"
    print("RESTART-OK %d -> %d" % (old_pid, new_pid))
finally:
    tap.close()
PY

echo "== T14 AdbUdpTap.restart: 换新监听、旧进程退出、状态清零 =="
rm -f "$TMP/nc_state14"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state14" \
      python3 "$TMP/restart_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
assert_eq "$rc" "0" "T14a restart 基本路径成功"
assert_contains "$out" "RESTART-OK" "T14b 报告新旧进程 pid"

echo "== T15 AdbUdpTap.restart: 前两次监听起不来时自动重试 =="
rm -f "$TMP/nc_count15" "$TMP/nc_state15"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state15" FAKE_ADB_NC_FAILS_ON="2 3" FAKE_ADB_NC_COUNT="$TMP/nc_count15" \
      python3 "$TMP/restart_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
assert_eq "$rc" "0" "T15a 重试后成功"
assert_contains "$out" "RESTART-OK" "T15b 报告新旧进程 pid"

echo "== T16 AdbUdpTap.restart: 始终起不来时必须报错而不是假装成功 =="
rm -f "$TMP/nc_count16" "$TMP/nc_state16"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state16" FAKE_ADB_NC_FAILS_ON="2 3 4 5 6 7 8 9" FAKE_ADB_NC_COUNT="$TMP/nc_count16" \
      python3 "$TMP/restart_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T16a 退出码非 0" || bad "T16a 退出码非 0" "实得 $rc"
assert_contains "$out" "设备上 UDP 监听未就绪" "T16b 报错信息可定位"
assert_contains "$out" "hex 138C" "T16c 报错含端口/hex 定位信息"

# ---------------------------------------------------------------------------
# T17/T18/T19：bind 就绪探测读不到 /proc/net/udp 时的降级兜底
#
# 背景（run 40061050114 回归修复）：上一个修复给 _start_process 加了
# /proc/net/udp 轮询探测，但权限收紧的 ROM / SELinux 策略 / 定制镜像可能
# 根本不允许 adb shell 读它。若把"读不到内容"当成"没 bind 上"，tap 会完全
# 起不来，比修复前更糟。pc_client 现在区分两种"无监听行"：
#   (a) 读到了内容但没有该端口的行 → 继续等到超时失败（真的没 bind 上）；
#   (b) 两个 proc 文件都读不到内容（blind）→ nc 连续存活超过 2s 确认窗后，
#       按旧判据（进程活着）降级放行，并打印明确的"降级"日志（不静默）。
# 这里用假 adb 的三种 /proc/net/udp 形态验证这条分界。
# ---------------------------------------------------------------------------
cat > "$TMP/tap_start_test.py" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import pc_client

# argv[3] 可选：bind_timeout（T18 用它把超时压到 3s，避免干等满 10s；
# 判据逻辑与默认 10s 完全一致，只是预算更短）
bind_timeout = float(sys.argv[3]) if len(sys.argv) > 3 else pc_client.BIND_READY_TIMEOUT
adb = pc_client.Adb(sys.argv[2])
tap = pc_client.AdbUdpTap(adb, 5004, bind_timeout=bind_timeout)
try:
    assert tap.proc is not None and tap.proc.poll() is None, "tap 启动后应存活"
    print("TAP-OK %d" % tap.proc.pid)
finally:
    tap.close()
PY

echo "== T17 /proc/net/udp 读不到（Permission denied）→ 降级成功且不静默 =="
rm -f "$TMP/nc_state17"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state17" FAKE_ADB_UDP_PROC_MODE=unreadable \
      python3 "$TMP/tap_start_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
assert_eq "$rc" "0" "T17a 读不到 /proc/net/udp 时 tap 仍能启动（降级）"
assert_contains "$out" "降级" "T17b 日志明确标注降级（不静默）"
assert_contains "$out" "Permission denied" "T17c 降级原因可定位（含 adb stderr）"
assert_contains "$out" "TAP-OK" "T17d 启动后 nc 进程存活"

echo "== T18 /proc/net/udp 正常但始终无 138C 行 → 超时失败，不得误降级 =="
rm -f "$TMP/nc_state18"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state18" FAKE_ADB_UDP_PROC_MODE=no-port \
      python3 "$TMP/tap_start_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 3.0 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T18a 退出码非 0" || bad "T18a 退出码非 0" "实得 $rc"
assert_contains "$out" "设备上 UDP 监听未就绪" "T18b 明确报告未就绪"
assert_not_contains "$out" "降级" "T18c 读得到内容时不得降级（等到超时）"

echo "== T19 /proc/net/udp 读不到但 nc 立即退出 → 不得降级放行死进程 =="
rm -f "$TMP/nc_count19" "$TMP/nc_state19"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state19" FAKE_ADB_UDP_PROC_MODE=unreadable \
      FAKE_ADB_NC_FAILS_ON="1 2 3 4 5 6 7 8 9" FAKE_ADB_NC_COUNT="$TMP/nc_count19" \
      python3 "$TMP/restart_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T19a 退出码非 0" || bad "T19a 退出码非 0" "实得 $rc"
assert_contains "$out" "设备上 UDP 监听未就绪" "T19b 明确报告未就绪"
assert_not_contains "$out" "降级" "T19c 进程已退出时不得降级（优先级高于 blind）"

# ---------------------------------------------------------------------------
# T20/T21：exec-out 通道回归（C05 真正根因：adb shell 的 PTY/ONLCR 毁二进制帧）
#
# 背景（2026-10-24 沙箱实验钉死）：`adb shell` 在设备端分配 PTY，ONLCR 把
# 输出中每个 0x0A 改写成 0x0D 0x0A——Opus 二进制帧被毁，160 字节帧不含
# 0x0A 的概率仅 (255/256)^160 ≈ 53%，C05 因此约一半概率红（历史上所有
# "C05 抖动"的根因，与 app 代码改动无关）。修复：tap 改走 `adb exec-out`
# （官方二进制通道，无 PTY，字节原样）。T20 断言子命令是 exec-out（stub
# 记录 argv）；T21 端到端验证字节级回传，并证明"通道若是 PTY 帧必被毁"
# （旧实现红的证据）。三层判据与 test_pc_client.py 的 ExecOutChannelTest
# 对应：实现一旦回退到 shell，这些用例转红。
# ---------------------------------------------------------------------------
echo "== T20 tap 必须走 exec-out 而非 shell（C05 PTY/ONLCR 根因回归）=="
rm -f "$TMP/nc_state20" "$TMP/argv20.log"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state20" FAKE_ADB_ARGV_LOG="$TMP/argv20.log" \
      python3 "$TMP/tap_start_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" 2>&1); rc=$?
assert_eq "$rc" "0" "T20a tap 启动成功（exec-out 通道）"
assert_contains "$out" "TAP-OK" "T20b nc 进程存活"
argv_log="$(cat "$TMP/argv20.log" 2>/dev/null)"
assert_contains "$argv_log" "exec-out toybox nc" "T20c tap 的 nc 调用走 exec-out（无 PTY）"
assert_not_contains "$argv_log" "shell toybox nc" "T20d nc 绝不走 shell（PTY/ONLCR 毁帧）"
assert_contains "$argv_log" "shell cat /proc/net/udp" "T20e 文本探测仍走 shell（未被误改）"

cat > "$TMP/tap_bytes_test.py" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import pc_client

# argv[2]=stub adb 路径；argv[3]=期望字节级一致的格式 C 帧文件。
# 断言 tap 回收到的首包与文件内容字节级一致——通道若是 PTY（ONLCR 把
# 0x0A 改写成 0D 0A）则必然不符（C05 根因的端到端判据）。
adb = pc_client.Adb(sys.argv[2])
tap = pc_client.AdbUdpTap(adb, 5004)
try:
    pkt = tap.read_packet(timeout=3.0)
    expected = open(sys.argv[3], "rb").read()
    assert pkt is not None, "未收到任何 UDP 包"
    assert pkt == expected, (
        "字节流被通道破坏（PTY/ONLCR 毁帧）：期望 %s，实得 %s"
        % (expected.hex(), pkt.hex()))
    print("TAP-BYTES-OK %d" % len(pkt))
finally:
    tap.close()
PY

echo "== T21 tap 字节级回传：exec-out 原样还原；PTY 通道必毁帧 =="
FRAME_FILE="$TMP/opus_frame.bin"
EMIT_FILE="$TMP/opus_emit.bin"
PYTHONPATH="$REPO_ROOT/android/verify/integration" python3 - "$FRAME_FILE" "$EMIT_FILE" <<'PY'
import sys
import pc_client
# 一个含 0x0A 的合法格式 C 帧（len=14，payload = 10 0A 20 0D 30 0A 40）
frame = pc_client.build_format_c(0x002A, 0x01020304, b"\x10\x0a\x20\x0d\x30\x0a\x40", volume=5)
open(sys.argv[1], "wb").write(frame)
# emit 文件 = 帧重复 5000 份（90KB > 64KB）：tap 的 pump 用
# read(65536)（BufferedReader 语义：凑满 65536 或 EOF 才返回），只发一帧
# 会让 read 永久阻塞。断言只看 stream 的**首个包**（见 tap_bytes_test.py）。
open(sys.argv[2], "wb").write(frame * 5000)
PY
rm -f "$TMP/nc_state21a"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state21a" FAKE_ADB_NC_EMIT_FILE="$EMIT_FILE" \
      python3 "$TMP/tap_bytes_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" "$FRAME_FILE" 2>&1); rc=$?
assert_eq "$rc" "0" "T21a exec-out 通道字节级还原首包（新实现绿）"
assert_contains "$out" "TAP-BYTES-OK" "T21b 报告还原字节数"

rm -f "$TMP/nc_state21c"
out=$(FAKE_ADB_NC_STATE="$TMP/nc_state21c" FAKE_ADB_NC_EMIT_FILE="$EMIT_FILE" FAKE_ADB_FORCE_PTY=1 \
      python3 "$TMP/tap_bytes_test.py" "$REPO_ROOT/android/verify/integration" "$TAP_STUB" "$FRAME_FILE" 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "T21c 通道若是 PTY 帧必被毁（旧实现红的证据：字节级还原失败）" \
    || bad "T21c 通道若是 PTY 帧必被毁" "PTY 通道下竟还原出了原帧？"
assert_contains "$out" "字节流被通道破坏" "T21d 毁帧原因可定位（PTY/ONLCR）"

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
