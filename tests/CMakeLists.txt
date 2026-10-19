# ---------------------------------------------------------------------------
# miclink-open -- shared/core 的 Linux 单元测试（独立构建，一条命令即可）
#
#   cd tests && ./run_tests.sh
#
# 覆盖：
#   test_ring_buffer     SPSC 环形缓冲 + 1000 万帧并发压力
#   test_jitter_buffer   自适应抖动缓冲 32 组网络场景
#   test_protocol        UDP 包协议编解码与错误码
#   test_opus_roundtrip  真实 libopus 16 组参数 SNR 验证
#   test_dsp             增益 / 电平 / 去咔哒 / 静音填充
#   test_sample_rate     采样率链路校验
#   fuzz_protocol        15 万轮畸形包 fuzz（ASan + UBSan）
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(miclink_open_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)
find_package(Threads REQUIRED)

set(MICLINK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared/core")

set(MICLINK_CORE_SOURCES
    ${MICLINK_CORE_DIR}/src/ring_buffer.c
    ${MICLINK_CORE_DIR}/src/protocol.c
    ${MICLINK_CORE_DIR}/src/sample_rate.c
    ${MICLINK_CORE_DIR}/src/dsp.c
    ${MICLINK_CORE_DIR}/src/jitter_buffer.c
)

# shared/core 是三方共用代码，按最严格标准编译：C11 + 零警告
add_library(miclink_core STATIC ${MICLINK_CORE_SOURCES})
target_include_directories(miclink_core PUBLIC ${MICLINK_CORE_DIR}/include)
target_compile_options(miclink_core PRIVATE -std=c11 -Wall -Wextra -Werror)
set_target_properties(miclink_core PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

# fuzz 目标单独使用带 sanitizer 插桩的 core，
# 这样 core 内部的越界读也能被 ASan 捕获（未插桩的静态库不会被检测）
add_library(miclink_core_sanitized STATIC ${MICLINK_CORE_SOURCES})
target_include_directories(miclink_core_sanitized PUBLIC ${MICLINK_CORE_DIR}/include)
target_compile_options(miclink_core_sanitized PRIVATE
    -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g)
set_target_properties(miclink_core_sanitized PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

set(MICLINK_TEST_WARNING_FLAGS -Wall -Wextra -Werror)

function(miclink_add_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OPUS_INCLUDE_DIRS})
    target_compile_options(${name} PRIVATE ${MICLINK_TEST_WARNING_FLAGS})
    target_link_libraries(${name} PRIVATE miclink_core ${OPUS_LIBRARIES} Threads::Threads)
    target_link_directories(${name} PRIVATE ${OPUS_LIBRARY_DIRS})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()

miclink_add_test(test_ring_buffer)
miclink_add_test(test_jitter_buffer)
miclink_add_test(test_protocol)
miclink_add_test(test_opus_roundtrip)
miclink_add_test(test_dsp)
miclink_add_test(test_sample_rate)

# ---- fuzz 目标：单独开 ASan + UBSan ----
add_executable(fuzz_protocol fuzz_protocol.cpp)
target_include_directories(fuzz_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OPUS_INCLUDE_DIRS})
target_compile_options(fuzz_protocol PRIVATE
    ${MICLINK_TEST_WARNING_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_libraries(fuzz_protocol PRIVATE miclink_core_sanitized ${OPUS_LIBRARIES} Threads::Threads)
target_link_options(fuzz_protocol PRIVATE -fsanitize=address,undefined)
target_link_directories(fuzz_protocol PRIVATE ${OPUS_LIBRARY_DIRS})
set_target_properties(fuzz_protocol PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1")
add_test(NAME fuzz_protocol COMMAND fuzz_protocol)
