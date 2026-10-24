/*
 * opus_decoder.h -- libopus 解码器封装（含丢包隐藏 PLC 与增益控制）。
 *
 * 注意：OpusDecoder 实例**不是**线程安全的。本项目里它只被 UDP 接收线程
 * （同时承载播放时钟）访问，解码与 PLC 都在那一个线程内串行发生，
 * 因此不需要加锁 —— 这是刻意的线程模型设计，不是遗漏。
 */
#ifndef MICLINK_SERVER_OPUS_DECODER_H
#define MICLINK_SERVER_OPUS_DECODER_H

#include <cstdint>
#include <string>

#include <opus.h>

namespace miclink {

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper() = default;
    ~OpusDecoderWrapper();

    OpusDecoderWrapper(const OpusDecoderWrapper &) = delete;
    OpusDecoderWrapper &operator=(const OpusDecoderWrapper &) = delete;

    /**
     * @brief 创建解码器。
     * @param sample_rate 解码输出采样率（16000 / 24000 / 48000；Opus 内部总是 48k）
     * @param channels    声道数
     * @param err         失败原因
     */
    bool init(uint32_t sample_rate, uint32_t channels, std::string *err);

    /** 已初始化？ */
    bool ready() const { return dec_ != nullptr; }

    /** 释放解码器。 */
    void destroy();

    /** 当前解码采样率。 */
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }

    /**
     * @brief 解码一帧。
     * @param payload       Opus 负载（可为 NULL，此时等价于 decode_plc）
     * @param payload_bytes 负载长度；0 表示丢包
     * @param pcm           输出，容量至少 frame_size * channels 个样本
     * @param frame_size    每声道样本数（必须是合法 Opus 帧长）
     * @return 实际输出的每声道样本数；负值表示错误（Opus 错误码）
     */
    int decode(const uint8_t *payload, int payload_bytes, int16_t *pcm, int frame_size);

    /** 丢包隐藏：opus_decode(dec, NULL, 0, ...)。 */
    int decode_plc(int16_t *pcm, int frame_size);

    /** 播放前的静音预热（避免首帧从随机电平跳变）。 */
    int decode_silence(int16_t *pcm, int frame_size);

    /** 设置解码增益（Q8 dB，如 6.0dB -> 48）。 */
    bool set_gain_q8(int gain_q8, std::string *err);

    /** 设置增益（dB，内部转 Q8）。 */
    bool set_gain_db(float db, std::string *err);

    /** 复位解码器状态（换客户端 / 采样率切换后调用）。 */
    bool reset(std::string *err);

    /** Opus 错误码 -> 文本。 */
    static std::string error_string(int code);

    /**
     * @brief 校验 frame_size 是否是该采样率下的合法 Opus 帧长。
     * 合法帧长：2.5 / 5 / 10 / 20 / 40 / 60 ms。
     */
    static bool is_valid_frame_size(uint32_t sample_rate, uint32_t frame_samples);

private:
    ::OpusDecoder *dec_ = nullptr;
    uint32_t sample_rate_ = 0u;
    uint32_t channels_ = 0u;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_OPUS_DECODER_H */
