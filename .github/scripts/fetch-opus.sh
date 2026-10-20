#!/usr/bin/env bash
# 下载 libopus 源码到 android/app/src/main/cpp/opus/。
#
# 源码不入库（体积 + 许可证单独保留），CI 现场拉取；缺失时 CMakeLists.txt 会
# FATAL_ERROR（这是刻意的：给明确的错误而不是编出个跑不起来的空库）。
#
# 依次尝试：xiph 官方下载站 → GitHub codeload（tag tarball）。
# 两者都不支持断点续传，因此用整包下载 + 校验结构。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT/android"

DEST="app/src/main/cpp/opus"
ARCHIVE=/tmp/opus-src.tar.gz

# 幂等：本地已有完整源码就不重下（CI 的 checkout 里没有，因为 .gitignore 排除了）
if [ -f "$DEST/CMakeLists.txt" ] && [ -f "$DEST/include/opus.h" ] && [ "${OPUS_FORCE:-0}" != "1" ]; then
    echo "[opus] 已存在完整源码：$DEST（OPUS_FORCE=1 可强制重下）"
    exit 0
fi

mkdir -p app/src/main/cpp
rm -rf "$DEST" "$ARCHIVE"

URLS=(
    "https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz"
    "https://codeload.github.com/xiph/opus/tar.gz/refs/tags/v1.5.2"
)
# 可用 OPUS_URL 覆盖（内网镜像 / 本地 file:// 复跑）
[ -n "${OPUS_URL:-}" ] && URLS=("$OPUS_URL" "${URLS[@]}")

ok=0
for url in "${URLS[@]}"; do
    echo "[opus] 尝试: $url"
    if curl -fsSL --retry 2 --connect-timeout 20 -o "$ARCHIVE" "$url"; then
        if tar -tzf "$ARCHIVE" > /dev/null 2>&1; then
            echo "[opus] 下载成功: $(stat -c%s "$ARCHIVE" 2>/dev/null || echo '?') 字节"
            ok=1
            break
        fi
        echo "[opus] 包损坏，换下一个源"
    fi
done
if [ "$ok" != "1" ]; then
    echo "::error::opus 源码下载失败（xiph 与 GitHub 均不可用）"
    exit 1
fi

rm -rf /tmp/opus-src && mkdir -p /tmp/opus-src
tar -xzf "$ARCHIVE" -C /tmp/opus-src
SRC="$(ls -d /tmp/opus-src/opus-*/ | head -1)"
SRC_NAME="$(basename "$SRC")"
mv "$SRC" "$DEST"

# 结构校验：路径 A（opus 自带 CMakeLists，1.4+）与路径 B（源码通配）都需要这些
test -f "$DEST/CMakeLists.txt"   || { echo "::error::缺少 $DEST/CMakeLists.txt"; exit 1; }
test -f "$DEST/include/opus.h"   || { echo "::error::缺少 $DEST/include/opus.h"; exit 1; }
test -d "$DEST/src"              || { echo "::error::缺少 $DEST/src"; exit 1; }

# 注意：GitHub tag tarball 里 configure.ac 的版本是占位符 CURRENT_VERSION，
# 真实版本看解压出来的目录名（release 包才会替换）。
echo "[opus] 源码版本: ${SRC_NAME}（目录名）"
grep -m1 '^OPUS_VERSION\|^AC_INIT' "$DEST/configure.ac" 2>/dev/null | sed 's/^/[opus] configure.ac: /' || true
echo "[opus] 安装到: $DEST（$(du -sh "$DEST" | cut -f1)），顶层文件:"
ls "$DEST" | head -12
