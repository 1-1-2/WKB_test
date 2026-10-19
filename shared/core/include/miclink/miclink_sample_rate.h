/*
 * miclink_sample_rate.h -- 三方（手机采集 / 服务端解码 / 驱动端点）采样率一致性校验。
 *
 * PRD 要求：采样率链路不一致时服务端必须拒绝启动；驱动侧绝不做重采样。
 *
 * 本项目约定（实现语义，见 miclink_check_sample_rate_chain 注释）：
 *   - endpoint_rate（驱动端点格式）必须严格等于 decode_rate（服务端解码采样率），
 *     否则驱动就要重采样，这是被禁止的，直接拒绝启动；
 *   - capture_rate（手机侧 Opus 采样率）允许与 decode_rate 不同：
 *     由服务端在解码后统一重采样到 decode_rate，再原样下发给驱动，
 *     即“uniformity”——最终到达驱动的始终是单一采样率。
 *     因此 capture_rate != decode_rate 不是致命错误，只在 out_message 中给出提示，
 *     供服务端日志打印（说明本次会话启用了服务端重采样）。
 */
#ifndef MICLINK_SAMPLE_RATE_H
#define MICLINK_SAMPLE_RATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MICLINK_SR_OK = 0,                    /* 链路一致，可以启动 */
    MICLINK_SR_CAPTURE_MISMATCH = 1,      /* 采集率 != 解码率：非致命，服务端重采样消化 */
    MICLINK_SR_ENDPOINT_MISMATCH = 2,     /* 解码率 != 端点率：致命，拒绝启动 */
    MICLINK_SR_UNSUPPORTED_RATE = 3,      /* 采样率不在支持列表内：致命 */
    MICLINK_SR_UNSUPPORTED_CHANNELS = 4   /* 声道数不支持：致命 */
} miclink_sr_result;

/**
 * @brief 校验采样率链路。
 *
 * 判定顺序：
 *   1) capture_rate / decode_rate / endpoint_rate 任一不在 {16000,44100,48000}
 *      -> MICLINK_SR_UNSUPPORTED_RATE
 *   2) channels 不在 {1,2} -> MICLINK_SR_UNSUPPORTED_CHANNELS
 *   3) decode_rate != endpoint_rate -> MICLINK_SR_ENDPOINT_MISMATCH（致命，拒绝启动）
 *   4) capture_rate != decode_rate -> MICLINK_SR_OK，并在消息中说明服务端会重采样
 *      （保留码 MICLINK_SR_CAPTURE_MISMATCH 供未来“严格模式”使用）
 *
 * @param out_message 输出中文可读原因串（静态字符串，无需释放）；可为 NULL。
 * @return miclink_sr_result
 */
miclink_sr_result miclink_check_sample_rate_chain(uint32_t capture_rate, uint32_t decode_rate,
                                              uint32_t endpoint_rate, uint16_t channels,
                                              const char **out_message);

/**
 * @brief 该结果是否致命（会阻止服务端启动 / 驱动接受格式）。
 * @return 1 = 致命，必须拒绝；0 = 可继续（仅提示）
 */
int miclink_sr_is_fatal(miclink_sr_result r);

/**
 * @brief 采样率是否被支持（16000 / 44100 / 48000）。
 * @return 1 支持，0 不支持
 */
int miclink_is_supported_rate(uint32_t rate);

/** @brief 声道数是否被支持（1 / 2）。 */
int miclink_is_supported_channels(uint16_t channels);

#ifdef __cplusplus
}
#endif

#endif /* MICLINK_SAMPLE_RATE_H */
