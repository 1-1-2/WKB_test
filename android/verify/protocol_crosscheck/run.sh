#!/usr/bin/env bash
#
# run.sh -- Android 端封包字节布局 与 shared/core 权威实现的**逐字节交叉校验**。
#
#   cd android/verify/protocol_crosscheck && ./run.sh
#
# 做什么：
#   1) 用真实的 shared/core/src/protocol.c 编出参考包（gcc）；
#   2) 用 PacketizerProbe.java（Packetizer.kt 的逻辑镜像）编出同样的包（javac + java）；
#   3) 逐字节 diff，任何一个字节不同即失败（退出码 1）。
#
# 前置：gcc、javac/java（JDK 8+）。不需要 Android SDK / NDK / libopus。
#
# 诚实声明：本脚本校验的是**逻辑一致性**（常量 / 偏移 / 字节序），
# 不是 Kotlin 源码本身（沙箱里没有 Kotlin 编译器）。
# 改 Packetizer.kt 时必须同步改 PacketizerProbe.java，否则本脚本给出的绿灯会失真。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# protocol_crosscheck -> verify -> android -> womic-open（仓库根）
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "==> 仓库根：${REPO_ROOT}"
mkdir -p "${BUILD_DIR}"

echo "==> 1/3 用 shared/core/src/protocol.c 编出参考包（C 权威实现）"
gcc -std=c99 -Wall -Wextra \
    -I "${REPO_ROOT}/shared/core/include" \
    "${SCRIPT_DIR}/pktc.c" \
    "${REPO_ROOT}/shared/core/src/protocol.c" \
    -o "${BUILD_DIR}/pktc"
"${BUILD_DIR}/pktc" > "${BUILD_DIR}/c_out.txt"
cat "${BUILD_DIR}/c_out.txt"

echo "==> 2/3 用 PacketizerProbe.java（Packetizer.kt 镜像）编出同样的包"
javac -d "${BUILD_DIR}/classes" "${SCRIPT_DIR}/PacketizerProbe.java"
java -cp "${BUILD_DIR}/classes" PacketizerProbe > "${BUILD_DIR}/java_out.txt"
cat "${BUILD_DIR}/java_out.txt"

echo "==> 3/3 逐字节比对（第 2 行是十六进制报文字节串）"
C_HEX="$(sed -n 2p "${BUILD_DIR}/c_out.txt")"
J_HEX="$(sed -n 2p "${BUILD_DIR}/java_out.txt")"
echo "  C    : ${C_HEX}"
echo "  Java : ${J_HEX}"

if [[ "${C_HEX}" != "${J_HEX}" ]]; then
    echo ""
    echo "FAIL：字节不一致 —— Android 端封包与服务端解码器契约不匹配！"
    exit 1
fi

if ! grep -q "SELFFIELDS-OK" "${BUILD_DIR}/java_out.txt"; then
    echo ""
    echo "FAIL：字段回读不一致"
    exit 1
fi

if ! grep -q "decode=37" "${BUILD_DIR}/c_out.txt"; then
    echo ""
    echo "FAIL：服务端解码器未接受该包（decode 返回值异常）"
    exit 1
fi

echo ""
echo "PASS：Android 端封包与 shared/core 权威实现逐字节一致，"
echo "      且能被 womic_packet_decode() 正常解回（len=37 = 32 头 + 5 payload）。"
