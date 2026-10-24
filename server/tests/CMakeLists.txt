# ---------------------------------------------------------------------------
# 服务端测试：真实 UDP loopback + 真实 libopus + Fake 设备后端
#
#   test_pipeline  端到端流水线（4 个场景：48k 直通 / 5% 丢包 / soxr 上采样 /
#                  speex 上采样），断言样本数、序列号连续性、电平与无溢出
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)
find_package(Threads REQUIRED)

set(MICLINK_TEST_WARNING_FLAGS -Wall -Wextra -Werror)

add_executable(test_pipeline test_pipeline.cpp)
target_include_directories(test_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${OPUS_INCLUDE_DIRS})
target_compile_options(test_pipeline PRIVATE ${MICLINK_TEST_WARNING_FLAGS})
target_link_libraries(test_pipeline PRIVATE miclink_server_core ${OPUS_LIBRARIES} Threads::Threads)
target_link_directories(test_pipeline PRIVATE ${OPUS_LIBRARY_DIRS})

add_test(NAME test_pipeline COMMAND test_pipeline)
