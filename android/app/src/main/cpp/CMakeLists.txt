# ---------------------------------------------------------------------------
# womic-open / Android 采集端 native 层
#
# 目标：把 app/src/main/cpp/opus_jni.cpp 编译成 libwomic_opus.so，并链接 libopus。
#
# 【重要】本工程**不内置** Opus 源码（体积大且许可证需单独保留）。
# 请先按 android/README.md 把 xiph/opus 的 release 源码放到：
#
#       app/src/main/cpp/opus/
#
# 期望目录结构（opus-1.4 及以上 release 原始布局）：
#       opus/CMakeLists.txt        （优先使用 opus 自带的 CMake 构建）
#       opus/include/opus.h
#       opus/src/*.c
#       opus/celt/*.c
#       opus/silk/*.c  opus/silk/float/*.c  opus/silk/fixed/*.c
#
# 若目录不存在，下面会给出明确的 FATAL_ERROR 而不是产生一个能编译但跑不起来的库。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22.1)
project(womic_opus CXX C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(WOMIC_OPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/opus")

if(NOT EXISTS "${WOMIC_OPUS_DIR}")
    message(FATAL_ERROR
"\n"
"=========================================================================\n"
" 未找到 Opus 源码：${WOMIC_OPUS_DIR}\n"
"-------------------------------------------------------------------------\n"
" 请先下载 Opus 官方 release 源码并解压到上述目录，例如：\n"
"\n"
"   curl -L -o opus-1.5.2.tar.gz \\\n"
"     https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz\n"
"   tar -xzf opus-1.5.2.tar.gz\n"
"   mkdir -p ${WOMIC_OPUS_DIR}\n"
"   cp -R opus-1.5.2/* ${WOMIC_OPUS_DIR}/\n"
"\n"
" 版本要求：1.4 及以上（自带 CMakeLists.txt 的版本可直接构建）。\n"
" 详细步骤见 android/README.md 的『准备 Opus 源码』一节。\n"
"=========================================================================\n")
endif()

if(NOT EXISTS "${WOMIC_OPUS_DIR}/include/opus.h")
    message(FATAL_ERROR
"\n"
" 目录 ${WOMIC_OPUS_DIR} 存在但缺少 include/opus.h。\n"
" 请确认解压时保留了 opus 的顶层目录结构（include/ src/ celt/ silk/）。\n")
endif()

set(WOMIC_OPUS_TARGET "")

# ---------------------------------------------------------------------------
# 路径 A（推荐）：opus 自带 CMakeLists.txt（1.4+ 的 release 均带）
# ---------------------------------------------------------------------------
if(EXISTS "${WOMIC_OPUS_DIR}/CMakeLists.txt")
    # 关掉一切不需要的东西：只要一个静态 libopus
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_OSCE_CROSS_CONFIG OFF CACHE BOOL "" FORCE)
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)   # 关闭 SIMD 内联汇编，跨 ABI 最稳
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_ENABLE_FLOAT_API ON CACHE BOOL "" FORCE)
    set(OPUS_VAR_ARRAYS OFF CACHE BOOL "" FORCE)

    add_subdirectory("${WOMIC_OPUS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/opus-build")

    if(TARGET opus)
        set(WOMIC_OPUS_TARGET opus)
    elseif(TARGET opus_static)
        set(WOMIC_OPUS_TARGET opus_static)
    elseif(TARGET libopus)
        set(WOMIC_OPUS_TARGET libopus)
    else()
        message(FATAL_ERROR
"\n"
" 已 add_subdirectory(${WOMIC_OPUS_DIR})，但没有找到 opus / opus_static / libopus\n"
" 目标。请检查该 opus 版本的 CMakeLists.txt，或改用『路径 B』：\n"
" 删除 ${WOMIC_OPUS_DIR}/CMakeLists.txt 后重新 configure，让本脚本用源码通配方式构建。\n")
    endif()
    message(STATUS "womic: 使用 opus 自带 CMake 构建，目标 = ${WOMIC_OPUS_TARGET}")

# ---------------------------------------------------------------------------
# 路径 B（兜底）：没有 CMakeLists.txt，直接通配源码（浮点构建）
# ---------------------------------------------------------------------------
else()
    if(NOT EXISTS "${WOMIC_OPUS_DIR}/src")
        message(FATAL_ERROR
"\n"
" ${WOMIC_OPUS_DIR} 既没有 CMakeLists.txt 也没有 src/ 目录，无法构建 libopus。\n"
" 请重新下载完整的 opus release 源码。\n")
    endif()

    file(GLOB WOMIC_OPUS_CELT_SRCS "${WOMIC_OPUS_DIR}/celt/*.c")
    file(GLOB WOMIC_OPUS_SILK_SRCS "${WOMIC_OPUS_DIR}/silk/*.c")
    file(GLOB WOMIC_OPUS_SILK_FLOAT_SRCS "${WOMIC_OPUS_DIR}/silk/float/*.c")
    file(GLOB WOMIC_OPUS_SRC_SRCS "${WOMIC_OPUS_DIR}/src/*.c")

    set(WOMIC_OPUS_ALL_SRCS
        ${WOMIC_OPUS_CELT_SRCS}
        ${WOMIC_OPUS_SILK_SRCS}
        ${WOMIC_OPUS_SILK_FLOAT_SRCS}
        ${WOMIC_OPUS_SRC_SRCS})

    if(WOMIC_OPUS_ALL_SRCS STREQUAL "")
        message(FATAL_ERROR "\n 在 ${WOMIC_OPUS_DIR} 下没有找到任何 .c 源文件，无法构建 libopus。\n")
    endif()

    # config.h：opus 源码需要一份配置头（浮点 / 定点、平台特性）。
    # 优先使用源码树里自带的；都没有才生成一份最小可用版本。
    set(WOMIC_OPUS_CONFIG_INCLUDE "")
    foreach(candidate
            "${WOMIC_OPUS_DIR}/opus_config.h"
            "${WOMIC_OPUS_DIR}/config.h"
            "${WOMIC_OPUS_DIR}/include/opus_config.h"
            "${WOMIC_OPUS_DIR}/win32/config.h")
        if(EXISTS "${candidate}")
            get_filename_component(WOMIC_OPUS_CONFIG_INCLUDE "${candidate}" DIRECTORY)
            message(STATUS "womic: 使用 opus 自带配置头 ${candidate}")
            break()
        endif()
    endforeach()

    if(WOMIC_OPUS_CONFIG_INCLUDE STREQUAL "")
        set(WOMIC_OPUS_CONFIG_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/opus-generated-config")
        file(MAKE_DIRECTORY "${WOMIC_OPUS_CONFIG_INCLUDE}")
        # 浮点构建、禁用内联汇编：不定义 FIXED_POINT 即为浮点，OPUS_ARM_ASM / OPUS_X86_*
        # 一律不定义，走可移植 C 实现（与 OPUS_DISABLE_INTRINSICS 等价）。
        file(WRITE "${WOMIC_OPUS_CONFIG_INCLUDE}/config.h"
"/* 由 womic-open 的 CMakeLists.txt 生成的最小 opus 配置头（兜底路径）。
 *
 * 说明：这是『能编译』的保守配置——浮点构建、关闭所有内联汇编优化。
 * 推荐使用 opus 自带 CMakeLists.txt（路径 A），那份配置会做运行时 CPU 特性检测，
 * 性能更好。若你看到本文件生效，说明你的 opus 源码包缺少 CMakeLists.txt。
 */
#ifndef CONFIG_H
#define CONFIG_H

#define OPUS_VERSION \"unknown\"
#define PACKAGE_VERSION \"unknown\"
#define PACKAGE_NAME \"opus\"
#define PACKAGE_TARNAME \"opus\"
#define PACKAGE_STRING \"opus unknown\"

/* 浮点构建：不要定义 FIXED_POINT */
/* #undef FIXED_POINT */
/* #undef DISABLE_FLOAT_API */
/* #undef OPUS_ARM_ASM */
/* #undef OPUS_ARM_MAY_HAVE_NEON */
/* #undef OPUS_ARM_MAY_HAVE_NEON_INTR */
/* #undef OPUS_ARM_PRESUME_NEON */
/* #undef OPUS_ARM_PRESUME_NEON_INTR */
/* #undef OPUS_X86_MAY_HAVE_SSE */
/* #undef OPUS_X86_MAY_HAVE_SSE2 */
/* #undef OPUS_X86_MAY_HAVE_SSE4_1 */
/* #undef OPUS_X86_MAY_HAVE_AVX */
/* #undef OPUS_X86_PRESUME_SSE */
/* #undef OPUS_X86_PRESUME_SSE2 */
/* #undef OPUS_X86_PRESUME_SSE4_1 */
/* #undef OPUS_X86_PRESUME_AVX */
/* #undef OPUS_HAVE_RTCD */
/* #undef OPUS_BUILD */
/* #undef ENABLE_HARDENING */
/* #undef OPUS_FORTIFY_SOURCE */
/* #undef OPUS_CHECK_ASM */
/* #undef OPUS_VAR_ARRAYS */
/* #undef ENABLE_ASSERTIONS */
/* #undef OPUS_TEST_RANGE_CODER_STATE */

#endif /* CONFIG_H */
")
        message(WARNING
"womic: 未找到 opus 自带的 config.h，已生成最小配置头 "
"${WOMIC_OPUS_CONFIG_INCLUDE}/config.h（浮点 + 纯 C，无 SIMD）。"
"建议改用带 CMakeLists.txt 的 opus 1.4+ 源码以获得最佳性能。")
    endif()

    add_library(opus STATIC ${WOMIC_OPUS_ALL_SRCS})
    target_include_directories(opus PUBLIC
        "${WOMIC_OPUS_DIR}/include"
        "${WOMIC_OPUS_DIR}/src"
        "${WOMIC_OPUS_DIR}/celt"
        "${WOMIC_OPUS_DIR}/silk"
        "${WOMIC_OPUS_DIR}/silk/float"
        "${WOMIC_OPUS_CONFIG_INCLUDE}")
    target_compile_definitions(opus PRIVATE HAVE_CONFIG_H=1 OPUS_BUILD=1)
    # opus 部分代码使用 __attribute__((unused)) 与浮点比较，关闭噪音告警
    target_compile_options(opus PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-sign-compare)

    set(WOMIC_OPUS_TARGET opus)
endif()

# ---------------------------------------------------------------------------
# JNI 封装库
# ---------------------------------------------------------------------------
add_library(womic_opus SHARED
        opus_jni.cpp
        opus_jni.h)

target_include_directories(womic_opus PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${WOMIC_OPUS_DIR}/include")

# 兜底路径下 opus 的 INTERFACE 里已带 include 目录；路径 A 下显式再补一次，
# 保证 #include <opus.h> 一定能找到（不同 opus 版本的 target 导出策略不同）。
target_link_libraries(womic_opus PRIVATE ${WOMIC_OPUS_TARGET} log)

target_compile_options(womic_opus PRIVATE -Wall -Wextra -Wno-unused-parameter)
