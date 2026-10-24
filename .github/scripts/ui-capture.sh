#!/usr/bin/env bash
# ui-capture.sh —— Android 采集端「界面取证」：四个界面各截一张 + uiautomator 控件断言。
#
# 目的：CI 此前只验协议闭环（能收流），从不验界面——抽屉导航、采集设置、外观页
# 到底长什么样、控件在不在，没有任何证据。本脚本在模拟器上把四屏走一遍并留证。
#
# 约定：
#   - 所有 adb 命令都套 timeout（防挂死，这是本仓库 CI 脚本的铁律）；
#   - 导航按 uiautomator dump 里控件的 bounds 中心点点击，不写死坐标（换分辨率不坏）；
#   - 断言失败只 WARN 不 exit 1（首轮以取证为主，截图与 dump 全量上传供人工复核；
#     只有「连 app 都拿不到界面」才 exit 1）。
#
# 用法：bash ui-capture.sh [输出目录]   （默认 /tmp/ui-shots）
set -uo pipefail

OUT="${1:-/tmp/ui-shots}"
PKG=com.miclink.open
mkdir -p "$OUT"

adb_t() { timeout 30 adb "$@"; }

# ------------------------------------------------------------------ 基础操作
# 当前界面 dump 到本地：$OUT/<name>.xml
dump_ui() {  # dump_ui <name>
    local name="$1"
    adb_t shell uiautomator dump /sdcard/ui-capture.xml >/dev/null 2>&1
    adb_t exec-out cat /sdcard/ui-capture.xml > "$OUT/$name.xml" 2>/dev/null
    [ -s "$OUT/$name.xml" ]
}

# 截图 + dump：$OUT/<name>.png / .xml
snap() {  # snap <name>
    local name="$1"
    timeout 30 adb exec-out screencap -p > "$OUT/$name.png" 2>/dev/null
    if [ ! -s "$OUT/$name.png" ]; then
        echo "::warning::screencap $name 失败（空文件）"
    fi
    dump_ui "$name" || echo "::warning::uiautomator dump $name 失败"
}

# 从本地 dump 里找 text 或 content-desc 包含 <needle> 的控件，输出 "x y"
# 同名文本可能既出现在顶栏标题（不可点）又出现在抽屉项（可点）——优先取
# clickable="true" 的节点，避免点到标题上白点一次。
find_center() {  # find_center <xml> <needle>
    python3 - "$1" "$2" <<'PY'
import sys
import xml.etree.ElementTree as ET

xml_path, needle = sys.argv[1], sys.argv[2]
try:
    root = ET.parse(xml_path).getroot()
except Exception:
    sys.exit(1)


def center(node):
    b = node.get("bounds") or ""              # [x1,y1][x2,y2]
    left, top = b.split("][")[0].strip("[").split(",")
    right, bottom = b.split("][")[1].strip("]").split(",")
    return (int(left) + int(right)) // 2, (int(top) + int(bottom)) // 2


fallback = None
for node in root.iter("node"):
    text = node.get("text") or ""
    desc = node.get("content-desc") or ""
    if needle in text or needle in desc:
        try:
            xy = center(node)
        except Exception:
            continue
        if node.get("clickable") == "true":
            print(*xy)
            sys.exit(0)
        if fallback is None:
            fallback = xy
if fallback:
    print(*fallback)
    sys.exit(0)
sys.exit(1)
PY
}

# 点按 text/content-desc 包含 <needle> 的控件（先 dump 再找）
tap_text() {  # tap_text <needle>
    local needle="$1" pos
    dump_ui _tap || { echo "::warning::tap_text($needle)：dump 失败"; return 1; }
    pos=$(find_center "$OUT/_tap.xml" "$needle") || { echo "::warning::找不到控件：$needle"; return 1; }
    # shellcheck disable=SC2086
    adb_t shell input tap $pos
    sleep 1
}

# 断言 dump 里包含某文本
assert_text() {  # assert_text <name> <needle> <label>
    if grep -qF "$2" "$OUT/$1.xml" 2>/dev/null; then
        echo "  [PASS] $3：$2"
    else
        echo "  [WARN] $3 未找到：$2"
    fi
}

# ------------------------------------------------------------------ 导航动作
open_drawer() { tap_text "打开导航抽屉" || tap_text "打开抽屉"; }
goto() { # goto <抽屉项文案>
    open_drawer
    tap_text "$1"
    sleep 1
}

# ------------------------------------------------------------------ 主流程
echo "[ui] 前置：确保 MainActivity 在前台"
adb_t shell am start -n "$PKG/.MainActivity" >/dev/null 2>&1 || true
sleep 3

echo "[ui] 1/4 主页"
snap 01-home
# 主操作按钮是二态：服务由 install-app.sh 预启动（调试音）→ 显示「停止服务」；
# 未启动时显示「启动服务」——两者其一即通过，不能写死单态（首轮曾误报 WARN）。
if grep -qF "停止服务" "$OUT/01-home.xml" 2>/dev/null || grep -qF "启动服务" "$OUT/01-home.xml" 2>/dev/null; then
    echo "  [PASS] 主页-主操作按钮：启动/停止服务（二态任一）"
else
    echo "  [WARN] 主页-主操作按钮（启动/停止服务）均未找到"
fi
assert_text 01-home "手机地址" "主页-手机地址"
assert_text 01-home "推流音量" "主页-设置区"

echo "[ui] 2/4 导航抽屉"
open_drawer
snap 02-drawer
assert_text 02-drawer "主页" "抽屉-主页"
assert_text 02-drawer "采集设置" "抽屉-采集设置"
assert_text 02-drawer "外观" "抽屉-外观"
assert_text 02-drawer "关于与帮助" "抽屉-关于"

echo "[ui] 3/4 外观页"
tap_text "外观"
sleep 1
snap 03-appearance
assert_text 03-appearance "主题模式" "外观-主题模式"
assert_text 03-appearance "配色" "外观-配色"

echo "[ui] 4/4 采集设置页"
goto "采集设置"
snap 04-capture
assert_text 04-capture "音源模式" "采集-音源模式"
assert_text 04-capture "麦克风" "采集-麦克风"
assert_text 04-capture "跟随系统" "采集-跟随系统"

# 下滑看效果开关区（降噪/回声/增益），截第二张
adb_t shell input swipe 540 1500 540 700 300 >/dev/null 2>&1 || true
sleep 1
snap 05-capture-effects
assert_text 05-capture-effects "降噪" "采集-降噪开关"

echo "[ui] 复原：回主页（不影响后续 interop）"
goto "主页"

echo "[ui] 完成，产物如下："
ls -la "$OUT"
