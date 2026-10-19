/*
 * resampler.h -- **服务端侧**重采样。
 *
 * 【红线说明（务必读完再改）】
 * 本项目严禁内核驱动做任何重采样：驱动端点一旦 SET_FORMAT 成功，之后所有
 * WRITE_PCM 必须是该采样率/声道/位深的原始 PCM，驱动绝不转换（见
 * shared/core/include/miclink/miclink_ioctl.h 中 MICLINK_ERR_FORMAT_MISMATCH 的语义）。
 *
 * 因此当"手机侧 Opus 采集率" != "驱动端点采样率"时（典型：手机 16k，端点 48k），
 * 转换责任 100% 落在用户态服务端，且只在本文件里发生：
 *
 *     手机 Opus(16k) --解码--> 16k PCM --[Resampler]--> 48k PCM --WRITE_PCM--> 驱动
 *
 * 后端二选一（编译期可都编进来，运行时 --resampler 切换）：
 *   - libsoxr   ：默认。多相滤波 + 可选抖动，阻带衰减与通带平坦度都优于 speex，
 *                 16k->48k 这类整数倍上采样的镜像抑制明显更好；代价是依赖体积大
 *                 （~200KB）且首次调用会分配滤波器。
 *   - libspeexdsp：依赖极小（本来就在很多系统里），API 简单、逐帧行为可预测
 *                 （整数比下输入输出样本数严格成比例），适合资源受限或需要
 *                 严格可预测缓冲行为的场景。音质略逊于 soxr。
 *
 * 采样率相同时直接走 memcpy 旁路，零开销、零音质损失。
 */
#ifndef MICLINK_SERVER_RESAMPLER_H
#define MICLINK_SERVER_RESAMPLER_H

#include <cstdint>
#include <string>
#include <vector>

namespace miclink {

class Resampler {
public:
    /** 可用后端。 */
    enum class Backend : int {
        kNone = 0,  /* 旁路：要求输入率 == 输出率 */
        kSoxr = 1,
        kSpeex = 2,
        kAuto = 3,  /* 有 soxr 用 soxr，否则 speex（调用方传入即可） */
    };

    Resampler() = default;
    ~Resampler();

    Resampler(const Resampler &) = delete;
    Resampler &operator=(const Resampler &) = delete;

    /**
     * @brief 初始化。in_rate == out_rate 时自动旁路（backend 记录为 kNone）。
     * @param preferred 期望后端；不可用时按 soxr -> speex -> none 降级，
     *                  并把实际后端写入 out_actual（可为 NULL）。
     */
    bool init(Backend preferred, uint32_t in_rate, uint32_t out_rate, uint32_t channels,
              uint32_t quality, Backend *out_actual, std::string *err);

    /** 释放资源。 */
    void destroy();

    /** 是否真的需要重采样（输入率 != 输出率）。 */
    bool needed() const { return needed_; }

    /** 实际使用的后端名（日志/状态显示）。 */
    const char *backend_name() const;

    /**
     * @brief 处理一帧。
     * @param in          输入样本（交错，样本数 = in_frames * channels）
     * @param in_frames   输入帧数（每声道）
     * @param out         输出缓冲
     * @param out_capacity 输出容量（每声道帧数）
     * @return 输出的每声道帧数；负值表示错误
     */
    int process(const int16_t *in, uint32_t in_frames, int16_t *out, uint32_t out_capacity);

    /**
     * @brief 流末尾把重采样器内部残留样本挤出（仅重采样路径有意义）。
     * 调用后本对象不可再 process()（soxr 语义下 EOF 不可逆），需要重新 init()。
     */
    int flush(int16_t *out, uint32_t out_capacity);

    /** 理论输出帧数（用于容量估算；重采样器实际输出可能少 1 帧，需 flush 补齐）。 */
    uint32_t expected_out_frames(uint32_t in_frames) const;

    /** 当前编译期是否编入了 soxr / speex。 */
    static bool soxr_available();
    static bool speex_available();

private:
    bool init_soxr(uint32_t quality, std::string *err);
    bool init_speex(uint32_t quality, std::string *err);

    Backend backend_ = Backend::kNone;
    bool needed_ = false;
    uint32_t in_rate_ = 0u;
    uint32_t out_rate_ = 0u;
    uint32_t channels_ = 0u;

    /* 具体后端句柄：用 void* 避免把第三方头文件拖进本头文件。
       生命周期由本类的 init/destroy 管理。 */
    void *handle_ = nullptr;
    /* speex 逐声道处理需要一块 int16 暂存区（样本数 <= 输入帧数 * channels）。 */
    std::vector<int16_t> scratch_;
    bool flushed_ = false;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_RESAMPLER_H */
