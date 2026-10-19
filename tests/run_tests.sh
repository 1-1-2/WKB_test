#!/usr/bin/env bash
#
# run_tests.sh -- 一条命令完成 clean build + ctest。
#
#   cd /workspace/miclink-open/tests && ./run_tests.sh
#
# 可选参数：
#   --jobs N      并行编译线程数（默认 nproc）
#   --no-clean    跳过 clean，增量构建（调试时更快）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

JOBS="$(nproc 2>/dev/null || echo 8)"
DO_CLEAN=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)      JOBS="$2"; shift 2 ;;
        --no-clean)  DO_CLEAN=0; shift ;;
        -h|--help)   echo "用法: $0 [--jobs N] [--no-clean]"; exit 0 ;;
        *)           echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if [[ "${DO_CLEAN}" -eq 1 && -d "${BUILD_DIR}" ]]; then
    echo "==> 清理 ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

echo "==> 配置 (Ninja, RelWithDebInfo)"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo

echo "==> 编译 (-j${JOBS})"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

echo "==> 运行 ctest"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "==> 全部通过"
