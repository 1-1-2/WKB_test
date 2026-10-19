/*
 * sample_rate.c -- 三方采样率一致性校验。
 *
 * 语义（见 miclink_sample_rate.h 顶部说明）：
 *   - endpoint_rate 必须 == decode_rate，否则驱动需要重采样，禁止，拒绝启动；
 *   - capture_rate != decode_rate 允许：由服务端在解码后统一重采样到 decode_rate，
 *     再下发给驱动，保证驱动侧永远是单一采样率（uniformity）。
 *     该情况返回 MICLINK_SR_OK，并通过 out_message 给出日志提示。
 *     （保留码 MICLINK_SR_CAPTURE_MISMATCH 供未来切到“严格模式”时使用。）
 *
 * out_message 返回的是静态字符串常量，调用方无需释放，生命周期无限。
 */
#include <stddef.h>

#include "miclink/miclink_sample_rate.h"

int miclink_is_supported_rate(uint32_t rate)
{
    return ((rate == 16000u) || (rate == 44100u) || (rate == 48000u)) ? 1 : 0;
}

int miclink_is_supported_channels(uint16_t channels)
{
    return ((channels == 1u) || (channels == 2u)) ? 1 : 0;
}

int miclink_sr_is_fatal(miclink_sr_result r)
{
    switch (r) {
    case MICLINK_SR_OK:
        return 0; /* 采集率不一致由服务端重采样消化，不阻断启动 */
    case MICLINK_SR_CAPTURE_MISMATCH:
        return 0; /* 保留：严格模式下才阻断 */
    case MICLINK_SR_ENDPOINT_MISMATCH:
    case MICLINK_SR_UNSUPPORTED_RATE:
    case MICLINK_SR_UNSUPPORTED_CHANNELS:
        return 1;
    default:
        return 1; /* 未知结果一律按致命处理，fail-safe */
    }
}

miclink_sr_result miclink_check_sample_rate_chain(uint32_t capture_rate, uint32_t decode_rate,
                                              uint32_t endpoint_rate, uint16_t channels,
                                              const char **out_message)
{
    const char *msg = "采样率链路一致（采集/解码/端点），可以启动。";
    miclink_sr_result result = MICLINK_SR_OK;

    /* 1) 采样率支持性 */
    if (!miclink_is_supported_rate(capture_rate)) {
        msg = "采集采样率不在支持列表（16000/44100/48000）内，拒绝启动。";
        result = MICLINK_SR_UNSUPPORTED_RATE;
    } else if (!miclink_is_supported_rate(decode_rate)) {
        msg = "解码采样率不在支持列表（16000/44100/48000）内，拒绝启动。";
        result = MICLINK_SR_UNSUPPORTED_RATE;
    } else if (!miclink_is_supported_rate(endpoint_rate)) {
        msg = "驱动端点采样率不在支持列表（16000/44100/48000）内，拒绝启动。";
        result = MICLINK_SR_UNSUPPORTED_RATE;
    } else if (!miclink_is_supported_channels(channels)) {
        /* 2) 声道数 */
        msg = "声道数不支持（仅支持 1 或 2 声道），拒绝启动。";
        result = MICLINK_SR_UNSUPPORTED_CHANNELS;
    } else if (decode_rate != endpoint_rate) {
        /* 3) 解码率必须等于端点率：驱动绝不做重采样 */
        msg = "解码采样率与驱动端点采样率不一致：驱动侧禁止重采样，拒绝启动；"
              "请先把服务端解码采样率改为与端点一致。";
        result = MICLINK_SR_ENDPOINT_MISMATCH;
    } else if (capture_rate != decode_rate) {
        /* 4) 采集率不一致：服务端重采样统一，允许启动（非致命） */
        msg = "采集采样率与解码采样率不一致：将由服务端在解码后重采样统一，允许启动。";
        result = MICLINK_SR_OK;
    } else {
        msg = "采样率链路一致（采集/解码/端点），可以启动。";
        result = MICLINK_SR_OK;
    }

    if (out_message != NULL) {
        *out_message = msg;
    }
    return result;
}
