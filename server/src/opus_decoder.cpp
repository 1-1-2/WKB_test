/*
 * opus_decoder.cpp -- libopus 解码封装实现。
 */
#include "opus_decoder.h"

#include <cmath>
#include <cstdio>

#include "log.h"

namespace womic {

static constexpr char kWomicLogModule[] = "opus";

OpusDecoderWrapper::~OpusDecoderWrapper()
{
    destroy();
}

void OpusDecoderWrapper::destroy()
{
    if (dec_ != nullptr) {
        opus_decoder_destroy(dec_);
        dec_ = nullptr;
    }
    sample_rate_ = 0u;
    channels_ = 0u;
}

bool OpusDecoderWrapper::init(uint32_t sample_rate, uint32_t channels, std::string *err)
{
    destroy();

    int error = OPUS_OK;
    ::OpusDecoder *dec = opus_decoder_create(static_cast<opus_int32>(sample_rate),
                                             static_cast<int>(channels), &error);
    if ((dec == nullptr) || (error != OPUS_OK)) {
        if (err != nullptr) {
            *err = std::string("opus_decoder_create 失败: ") + error_string(error);
        }
        return false;
    }
    dec_ = dec;
    sample_rate_ = sample_rate;
    channels_ = channels;
    return true;
}

bool OpusDecoderWrapper::is_valid_frame_size(uint32_t sample_rate, uint32_t frame_samples)
{
    if ((sample_rate == 0u) || (frame_samples == 0u)) {
        return false;
    }
    /* 用 ms*2 做整数比较，避免 2.5ms 的浮点判断。 */
    const uint32_t ms2 = static_cast<uint32_t>(static_cast<uint64_t>(frame_samples) * 2000u /
                                               static_cast<uint64_t>(sample_rate));
    switch (ms2) {
        case 5u:   /* 2.5 ms */
        case 10u:  /* 5 ms */
        case 20u:  /* 10 ms */
        case 40u:  /* 20 ms */
        case 80u:  /* 40 ms */
        case 120u: /* 60 ms */
            return true;
        default:
            return false;
    }
}

int OpusDecoderWrapper::decode(const uint8_t *payload, int payload_bytes, int16_t *pcm,
                               int frame_size)
{
    if (dec_ == nullptr) {
        return OPUS_INVALID_STATE;
    }
    if ((pcm == nullptr) || (frame_size <= 0)) {
        return OPUS_BAD_ARG;
    }
    if ((payload == nullptr) || (payload_bytes <= 0)) {
        return decode_plc(pcm, frame_size);
    }

    int n = opus_decode(dec_, payload, payload_bytes, pcm, frame_size, 0);
    if (n < 0) {
        WOMIC_LOGW("opus_decode 失败(%d): %s，改用 PLC 补齐", n, error_string(n).c_str());
        return decode_plc(pcm, frame_size);
    }
    return n;
}

int OpusDecoderWrapper::decode_plc(int16_t *pcm, int frame_size)
{
    if (dec_ == nullptr) {
        return OPUS_INVALID_STATE;
    }
    if ((pcm == nullptr) || (frame_size <= 0)) {
        return OPUS_BAD_ARG;
    }
    /*
     * PLC（Packet Loss Concealment）：payload 传 NULL、长度 0。
     * Opus 会基于上一帧的激励与 LPC 状态外推出一帧，听感上远好于填静音。
     * 注意 PLC 会改变解码器内部状态，因此必须按顺序调用，不能并发。
     */
    int n = opus_decode(dec_, nullptr, 0, pcm, frame_size, 0);
    if (n < 0) {
        WOMIC_LOGW("opus_decode(PLC) 失败(%d): %s，退化为静音", n, error_string(n).c_str());
        for (int i = 0; i < frame_size * static_cast<int>(channels_); ++i) {
            pcm[i] = 0;
        }
        return frame_size;
    }
    return n;
}

int OpusDecoderWrapper::decode_silence(int16_t *pcm, int frame_size)
{
    if ((pcm == nullptr) || (frame_size <= 0)) {
        return OPUS_BAD_ARG;
    }
    const int total = frame_size * ((channels_ > 0u) ? static_cast<int>(channels_) : 1);
    for (int i = 0; i < total; ++i) {
        pcm[i] = 0;
    }
    return frame_size;
}

bool OpusDecoderWrapper::set_gain_q8(int gain_q8, std::string *err)
{
    if (dec_ == nullptr) {
        if (err != nullptr) {
            *err = "解码器未初始化";
        }
        return false;
    }
    int rc = opus_decoder_ctl(dec_, OPUS_SET_GAIN(gain_q8));
    if (rc != OPUS_OK) {
        if (err != nullptr) {
            *err = std::string("OPUS_SET_GAIN 失败: ") + error_string(rc);
        }
        return false;
    }
    return true;
}

bool OpusDecoderWrapper::set_gain_db(float db, std::string *err)
{
    /*
     * OPUS_SET_GAIN 的单位是 Q8 dB（1/256 dB）。这里直接对浮点 dB 做
     * round(db * 256) 量化，避免链式误差；超出 [-32768, 32767] 时截断。
     */
    double q8 = std::round(static_cast<double>(db) * 256.0);
    if (q8 > 32767.0) {
        q8 = 32767.0;
    }
    if (q8 < -32768.0) {
        q8 = -32768.0;
    }
    return set_gain_q8(static_cast<int>(q8), err);
}

bool OpusDecoderWrapper::reset(std::string *err)
{
    if (dec_ == nullptr) {
        if (err != nullptr) {
            *err = "解码器未初始化";
        }
        return false;
    }
    int rc = opus_decoder_ctl(dec_, OPUS_RESET_STATE);
    if (rc != OPUS_OK) {
        if (err != nullptr) {
            *err = std::string("OPUS_RESET_STATE 失败: ") + error_string(rc);
        }
        return false;
    }
    return true;
}

std::string OpusDecoderWrapper::error_string(int code)
{
    const char *text = opus_strerror(code);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d (%s)", code, (text != nullptr) ? text : "unknown");
    return std::string(buf);
}

}  // namespace womic
