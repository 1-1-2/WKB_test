/*
 * test_sample_rate.cpp -- 采样率链路校验测试。
 *
 * 约定（见 womic_sample_rate.h）：
 *   decode_rate == endpoint_rate 是硬性要求（驱动不做重采样）；
 *   capture_rate != decode_rate 由服务端重采样消化，允许启动（非致命）。
 */
#include <womic/womic_sample_rate.h>

#include <cstdint>

#include "test_util.h"

namespace {

void test_supported_rates()
{
    WOMIC_CHECK_EQ(womic_is_supported_rate(16000u), 1);
    WOMIC_CHECK_EQ(womic_is_supported_rate(44100u), 1);
    WOMIC_CHECK_EQ(womic_is_supported_rate(48000u), 1);
    WOMIC_CHECK_EQ(womic_is_supported_rate(8000u), 0);
    WOMIC_CHECK_EQ(womic_is_supported_rate(22050u), 0);
    WOMIC_CHECK_EQ(womic_is_supported_rate(96000u), 0);
    WOMIC_CHECK_EQ(womic_is_supported_rate(0u), 0);

    WOMIC_CHECK_EQ(womic_is_supported_channels(1u), 1);
    WOMIC_CHECK_EQ(womic_is_supported_channels(2u), 1);
    WOMIC_CHECK_EQ(womic_is_supported_channels(0u), 0);
    WOMIC_CHECK_EQ(womic_is_supported_channels(6u), 0);
}

void test_pass_cases()
{
    const char *msg = nullptr;

    /* 全链路 48k */
    womic_sr_result r = womic_check_sample_rate_chain(48000u, 48000u, 48000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_OK);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 0);
    WOMIC_CHECK(msg != nullptr);

    /* 全链路 16k，双声道 */
    r = womic_check_sample_rate_chain(16000u, 16000u, 16000u, 2u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_OK);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 0);

    /* 44100 全链路 */
    r = womic_check_sample_rate_chain(44100u, 44100u, 44100u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_OK);

    /* 采集 16k / 解码与端点 48k：服务端重采样后统一，允许启动 */
    r = womic_check_sample_rate_chain(16000u, 48000u, 48000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_OK);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 0); /* 非致命 */
    WOMIC_CHECK(msg != nullptr);

    /* out_message 可为 NULL */
    r = womic_check_sample_rate_chain(48000u, 48000u, 48000u, 1u, nullptr);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_OK);
}

void test_reject_cases()
{
    const char *msg = nullptr;

    /* 解码 48k 但端点 16k -> 端点不一致（致命，拒绝启动） */
    womic_sr_result r = womic_check_sample_rate_chain(48000u, 48000u, 16000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_ENDPOINT_MISMATCH);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 解码 16k 但端点 48k -> 同上 */
    r = womic_check_sample_rate_chain(16000u, 16000u, 48000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_ENDPOINT_MISMATCH);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 采集率不支持：即使解码/端点一致也要拒绝 */
    r = womic_check_sample_rate_chain(8000u, 48000u, 48000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_RATE);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 解码率不支持 */
    r = womic_check_sample_rate_chain(48000u, 96000u, 96000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_RATE);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 端点率不支持 */
    r = womic_check_sample_rate_chain(48000u, 48000u, 8000u, 1u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_RATE);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 声道数不支持：优先于端点一致性检查？不，支持性检查在前，这里 endpoint 一致 */
    r = womic_check_sample_rate_chain(48000u, 48000u, 48000u, 6u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_CHANNELS);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    r = womic_check_sample_rate_chain(48000u, 48000u, 48000u, 0u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_CHANNELS);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);

    /* 全 0 输入 -> 采样率不支持 */
    r = womic_check_sample_rate_chain(0u, 0u, 0u, 0u, &msg);
    WOMIC_CHECK_EQ((int)r, (int)WOMIC_SR_UNSUPPORTED_RATE);
    WOMIC_CHECK_EQ(womic_sr_is_fatal(r), 1);
}

void test_messages_are_chinese_and_stable()
{
    const char *m1 = nullptr;
    const char *m2 = nullptr;
    (void)womic_check_sample_rate_chain(48000u, 48000u, 16000u, 1u, &m1);
    (void)womic_check_sample_rate_chain(48000u, 48000u, 16000u, 1u, &m2);
    WOMIC_CHECK(m1 != nullptr);
    WOMIC_CHECK(m1 == m2); /* 静态串，重复调用返回同一地址，无需释放 */

    /* 中文原因串：包含至少一个 GBK/UTF-8 中文字符（UTF-8 首字节 >= 0xE4） */
    bool has_cjk = false;
    for (const char *p = m1; *p != '\0'; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xE0u) == 0xE0u) {
            has_cjk = true;
            break;
        }
    }
    WOMIC_CHECK(has_cjk);
}

}  // namespace

WOMIC_TEST_MAIN(test_sample_rate, ({
    test_supported_rates();
    test_pass_cases();
    test_reject_cases();
    test_messages_are_chinese_and_stable();
}))
