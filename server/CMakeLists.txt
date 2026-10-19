# ---------------------------------------------------------------------------
# womic-open / server -- Windows 虚拟麦克风用户态服务端 (womicserver)
#
# 设计要点：
#   * 只有 3 处薄平台适配层使用 #ifdef _WIN32：
#       src/net_socket.cpp    winsock2   vs  Berkeley sockets
#       src/threads_sync.cpp  Win32 Thread/CRITICAL_SECTION vs pthread/std::thread
#       src/injector.cpp      CreateFileW/DeviceIoControl vs Fake 设备后端
#     其余全部为平台中立代码，因此 Linux 上可以真实编译 + 真实运行，
#     除 Win32 薄层外的所有逻辑都被真实执行过。
#
# Linux 冒烟构建（本仓库 CI 使用）：
#   cmake -B build -DWOMIC_WITH_FAKE_DEVICE=ON && cmake --build build
#   ./build/test_pipeline
#
# Windows 构建见 build_windows.md（vcpkg: opus / soxr / speexdsp）。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(womicserver LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

option(WOMIC_BUILD_TESTS "构建服务端测试" ON)
# Linux 上默认打开：让 "auto" 后端解析为 Fake，从而可以真跑端到端流水线。
if(WIN32)
    option(WOMIC_WITH_FAKE_DEVICE "内置 Fake 设备后端并使 auto 解析为它" OFF)
else()
    option(WOMIC_WITH_FAKE_DEVICE "内置 Fake 设备后端并使 auto 解析为它" ON)
endif()
option(WOMIC_WITH_SOXR "启用 libsoxr 重采样后端" ON)
option(WOMIC_WITH_SPEEXDSP "启用 libspeexdsp 重采样后端" ON)

set(WOMIC_WARNING_FLAGS -Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)
find_package(Threads REQUIRED)

set(WOMIC_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared/core")

# ---------------------------------------------------------------------------
# shared/core：三方共用代码（内核驱动 / 服务端 / Linux 单测），只 include 不复制。
# 按最严格标准编译，与 tests/CMakeLists.txt 保持一致。
# ---------------------------------------------------------------------------
add_library(womic_core STATIC
    ${WOMIC_CORE_DIR}/src/ring_buffer.c
    ${WOMIC_CORE_DIR}/src/protocol.c
    ${WOMIC_CORE_DIR}/src/sample_rate.c
    ${WOMIC_CORE_DIR}/src/dsp.c
    ${WOMIC_CORE_DIR}/src/jitter_buffer.c
)
target_include_directories(womic_core PUBLIC ${WOMIC_CORE_DIR}/include)
target_compile_options(womic_core PRIVATE -std=c11 ${WOMIC_WARNING_FLAGS})
set_target_properties(womic_core PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# 服务端核心库（不含 main.cpp，便于测试链接同一份代码）
# ---------------------------------------------------------------------------
set(WOMIC_SERVER_SOURCES
    src/log.cpp
    src/threads_sync.cpp
    src/threads.cpp
    src/net_socket.cpp
    src/config.cpp
    src/stats.cpp
    src/opus_decoder.cpp
    src/resampler.cpp
    src/ring_adapter.cpp
    src/jitter_pipeline.cpp
    src/injector.cpp
    src/udp_receiver.cpp
    src/pipeline.cpp
    src/discovery.cpp
    src/session.cpp
)

add_library(womic_server_core STATIC ${WOMIC_SERVER_SOURCES})
target_include_directories(womic_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${WOMIC_CORE_DIR}/include
    ${OPUS_INCLUDE_DIRS})
target_compile_options(womic_server_core PRIVATE ${WOMIC_WARNING_FLAGS})
target_link_libraries(womic_server_core PUBLIC womic_core ${OPUS_LIBRARIES} Threads::Threads)
target_link_directories(womic_server_core PUBLIC ${OPUS_LIBRARY_DIRS})

# --- 重采样后端（二选一或两者都开，运行时用 --resampler 切换） ---
if(WOMIC_WITH_SOXR)
    pkg_check_modules(SOXR QUIET soxr)
    if(SOXR_FOUND)
        target_include_directories(womic_server_core PRIVATE ${SOXR_INCLUDE_DIRS})
        target_link_libraries(womic_server_core PUBLIC ${SOXR_LIBRARIES})
        target_link_directories(womic_server_core PUBLIC ${SOXR_LIBRARY_DIRS})
        target_compile_definitions(womic_server_core PUBLIC WOMIC_HAVE_SOXR=1)
    else()
        message(STATUS "libsoxr 未找到：禁用 soxr 重采样后端")
    endif()
endif()

if(WOMIC_WITH_SPEEXDSP)
    pkg_check_modules(SPEEXDSP QUIET speexdsp)
    if(SPEEXDSP_FOUND)
        target_include_directories(womic_server_core PRIVATE ${SPEEXDSP_INCLUDE_DIRS})
        target_link_libraries(womic_server_core PUBLIC ${SPEEXDSP_LIBRARIES})
        target_link_directories(womic_server_core PUBLIC ${SPEEXDSP_LIBRARY_DIRS})
        target_compile_definitions(womic_server_core PUBLIC WOMIC_HAVE_SPEEXDSP=1)
    else()
        message(STATUS "libspeexdsp 未找到：禁用 speex 重采样后端")
    endif()
endif()

if(WOMIC_WITH_FAKE_DEVICE)
    target_compile_definitions(womic_server_core PUBLIC WOMIC_WITH_FAKE_DEVICE=1)
endif()

if(WIN32)
    target_compile_definitions(womic_server_core PUBLIC WOMIC_PLATFORM_WINDOWS=1)
    target_link_libraries(womic_server_core PUBLIC ws2_32)
else()
    target_compile_definitions(womic_server_core PUBLIC WOMIC_PLATFORM_POSIX=1)
    # Linux 上需要 librt / libm（clock_gettime 在 glibc 2.17+ 已进入 libc，保留兜底）
    find_library(WOMIC_RT_LIBRARY rt)
    if(WOMIC_RT_LIBRARY)
        target_link_libraries(womic_server_core PUBLIC ${WOMIC_RT_LIBRARY})
    endif()
    find_library(WOMIC_M_LIBRARY m)
    if(WOMIC_M_LIBRARY)
        target_link_libraries(womic_server_core PUBLIC ${WOMIC_M_LIBRARY})
    endif()
endif()

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(womicserver src/main.cpp)
target_include_directories(womicserver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${WOMIC_CORE_DIR}/include
    ${OPUS_INCLUDE_DIRS})
target_compile_options(womicserver PRIVATE ${WOMIC_WARNING_FLAGS})
target_link_libraries(womicserver PRIVATE womic_server_core ${OPUS_LIBRARIES} Threads::Threads)
target_link_directories(womicserver PRIVATE ${OPUS_LIBRARY_DIRS})

# ---------------------------------------------------------------------------
# 测试：真实 UDP loopback + 真实 libopus + Fake 设备后端
# ---------------------------------------------------------------------------
if(WOMIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 安装（Windows 侧主要用 build_windows.md 里的手工部署，这里提供基本规则）
# ---------------------------------------------------------------------------
install(TARGETS womicserver RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/womicserver.ini.example"
        DESTINATION etc OPTIONAL)
