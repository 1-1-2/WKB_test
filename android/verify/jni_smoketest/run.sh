#!/usr/bin/env bash
#
# run.sh -- opus_jni.cpp 的本机 JNI 冒烟测试。
#
#   cd android/verify/jni_smoketest && ./run.sh
#
# 做什么：把 opus_jni.cpp 与一个**假的 libopus**实现编成 libwomic_opus.so，
# 再用与 OpusEncoder.kt 同名同签名的 Java 类去加载、调用它，验证：
#   * JNI 符号名（Java_com_womic_open_OpusEncoder_*）与 Kotlin 声明一致；
#   * 参数签名（long 句柄 / short[] / byte[]）一致；
#   * 错误码（-1/-2/-3/-5）按契约返回；
#   * 非法帧长的 create 被拒绝。
#
# 前置：g++、JDK（javac/java，需要 jni.h）。
#
# 诚实声明：**链接的是假 Opus**，所以本测试只证明"JNI 接线正确"，
# 不证明 Opus 编码质量，也不代表 Android 真机可用。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

JNI_CPP="${REPO_ROOT}/android/app/src/main/cpp/opus_jni.cpp"

echo "==> 仓库根：${REPO_ROOT}"
mkdir -p "${BUILD_DIR}/classes"

# ---- 定位 JDK 的 jni.h ----
if command -v javac >/dev/null 2>&1; then
    JAVAC_BIN="$(command -v javac)"
else
    echo "FAIL: 找不到 javac（需要 JDK）" >&2
    exit 1
fi
JAVA_HOME_GUESS="$(dirname "$(dirname "$(readlink -f "${JAVAC_BIN}")")")"
JNI_INC="${JAVA_HOME_GUESS}/include"
echo "==> JAVA_HOME=${JAVA_HOME_GUESS}"
if [[ ! -f "${JNI_INC}/jni.h" ]]; then
    echo "FAIL: 找不到 ${JNI_INC}/jni.h" >&2
    exit 1
fi

# 平台相关的 jni_md.h 目录
JNI_MD_INC=""
for cand in "${JNI_INC}/linux" "${JNI_INC}/darwin" "${JNI_INC}/win32" "${JNI_INC}"; do
    if [[ -f "${cand}/jni_md.h" ]]; then
        JNI_MD_INC="${cand}"
        break
    fi
done

echo "==> 1/3 编译 libwomic_opus.so（链接 stub opus）"
# stubs/opus.h 与 stubs/android/log.h 是假头文件（内容转发到 stub_*.h），
# 通过 -I 优先命中，从而让 opus_jni.cpp 里的 <opus.h> / <android/log.h> 使用替身。
g++ -std=c++17 -fPIC -shared -Wall -Wextra \
    -I "${SCRIPT_DIR}" \
    -I "${SCRIPT_DIR}/stubs" \
    -I "${JNI_INC}" ${JNI_MD_INC:+-I "${JNI_MD_INC}"} \
    "${JNI_CPP}" "${SCRIPT_DIR}/opus_stub_impl.cpp" \
    -o "${BUILD_DIR}/libwomic_opus.so"

if [[ ! -f "${BUILD_DIR}/libwomic_opus.so" ]]; then
    echo "FAIL: 未生成 libwomic_opus.so" >&2
    exit 1
fi
echo "  [ok] 生成 ${BUILD_DIR}/libwomic_opus.so"
nm -D "${BUILD_DIR}/libwomic_opus.so" | grep 'Java_com_womic_open_OpusEncoder' | sed 's/^/       /'

echo "==> 2/3 编译 Java 镜像类"
javac -d "${BUILD_DIR}/classes" "${SCRIPT_DIR}/OpusEncoder.java"

echo "==> 3/3 运行冒烟测试"
java -cp "${BUILD_DIR}/classes" -Djava.library.path="${BUILD_DIR}" com.womic.open.OpusEncoder
