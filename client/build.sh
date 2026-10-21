#!/usr/bin/env bash
# 交叉编译 WO Mic Link 客户端（Linux → Windows x64）
#
# 依赖：gcc-mingw-w64-x86-64（apt install gcc-mingw-w64-x86-64）
# 产物：build/womic.exe，约 82 KB，只依赖 Windows 自带 DLL。
set -euo pipefail

CC=${CC:-x86_64-w64-mingw32-gcc}
OUT=${1:-build/womic.exe}
mkdir -p "$(dirname "$OUT")"

# -Os      体积优先
# -s       去掉符号表
# -mwindows GUI 子系统（不弹控制台）
# --gc-sections 回收未被引用的段（省约 2 KB）
#
# ⚠ 不要加 -ffunction-sections / -fdata-sections：
#   实测加上后产物从 81 KB 涨到 113 KB（+36%）。原因是这两个选项把每个变量
#   拆成独立 section，触发 mingw 的对齐填充，导致 .data 段里 99.7% 是零字节
#   （29 KB 的 .data 仅 89 字节非零）。除非同时仔细调优 --gc-sections，
#   否则属于“只付代价、不收收益”。此结论基于实测对比五种参数组合。
#
# 不链接任何第三方库；只列系统导入库
"$CC" -Os -s -mwindows \
  -Wl,--gc-sections \
  -o "$OUT" \
  src/main.c src/engine.c \
  -lgdi32 -luser32 -lws2_32 -luxtheme -ldwmapi -lshell32 -lole32

SIZE=$(stat -c %s "$OUT" 2>/dev/null || stat -f %z "$OUT")
echo "built: $OUT  (${SIZE} bytes / $((SIZE/1024)) KiB)"
echo "imports: $(x86_64-w64-mingw32-objdump -p "$OUT" | grep 'DLL Name' | sed 's/.*DLL Name: //' | sort -u | tr '\n' ' ')"
