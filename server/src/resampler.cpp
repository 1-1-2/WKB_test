/*
 * resampler.cpp -- 服务端侧重采样实现（soxr / speex 二选一，编译期可选）。
 *
 * 再次强调：驱动侧绝不做重采样，重采样只发生在这里。
 */
#include "resampler.h"

#include <cstring>

#include "log.h"

#if defined(WOMIC_HAVE_SOXR)
#include <soxr.h>
#endif
#if defined(WOMIC_HAVE_SPEEXDSP)
#include <speex/speex_resampler.h>
#endif

namespace womic {

static constexpr char kWomicLogModule[] = "resamp";

Resampler::~Resampler()
{
    destroy();
}

bool Resampler::soxr_available()
{
#if defined(WOMIC_HAVE_SOXR)
    return true;
#else
    return false;
#endif
}

bool Resampler::speex_available()
{
#if defined(WOMIC_HAVE_SPEEXDSP)
    return true;
#else
    return false;
#endif
}

const char *Resampler::backend_name() const
{
    if (!needed_) {
        return "bypass";
    }
    switch (backend_) {
        case Backend::kSoxr:
            return "soxr";
        case Backend::kSpeex:
            return "speex";
        case Backend::kAuto:
        case Backend::kNone:
        default:
            return "none";
    }
}

void Resampler::destroy()
{
#if defined(WOMIC_HAVE_SOXR)
    if (handle_ != nullptr) {
        if (backend_ == Backend::kSoxr) {
            soxr_delete(static_cast<soxr_t>(handle_));
        }
    }
#endif
#if defined(WOMIC_HAVE_SPEEXDSP)
    if ((handle_ != nullptr) && (backend_ == Backend::kSpeex)) {
        speex_resampler_destroy(static_cast<SpeexResamplerState *>(handle_));
    }
#endif
    handle_ = nullptr;
    backend_ = Backend::kNone;
    needed_ = false;
    flushed_ = false;
    scratch_.clear();
}

bool Resampler::init(Backend preferred, uint32_t in_rate, uint32_t out_rate, uint32_t channels,
                     uint32_t quality, Backend *out_actual, std::string *err)
{
    destroy();

    in_rate_ = in_rate;
    out_rate_ = out_rate;
    channels_ = (channels == 0u) ? 1u : channels;

    if ((in_rate == 0u) || (out_rate == 0u)) {
        if (err != nullptr) {
            *err = "重采样：采样率为 0";
        }
        return false;
    }

    if (in_rate == out_rate) {
        /* 旁路：最常见的生产配置（手机 48k -> 端点 48k），零开销。 */
        needed_ = false;
        backend_ = Backend::kNone;
        if (out_actual != nullptr) {
            *out_actual = Backend::kNone;
        }
        return true;
    }

    needed_ = true;

    if (preferred == Backend::kNone) {
        if (err != nullptr) {
            *err = "采集率与端点率不同，但重采样被显式禁用（resampler=none）";
        }
        return false;
    }

    bool try_soxr = (preferred == Backend::kSoxr) || (preferred == Backend::kAuto);
    bool try_speex = (preferred == Backend::kSpeex) || (preferred == Backend::kAuto);

    std::string soxr_err;
    std::string speex_err;

    if (try_soxr && soxr_available()) {
        if (init_soxr(quality, &soxr_err)) {
            backend_ = Backend::kSoxr;
            if (out_actual != nullptr) {
                *out_actual = Backend::kSoxr;
            }
            return true;
        }
        WOMIC_LOGW("soxr 初始化失败，降级到 speex：%s", soxr_err.c_str());
    }
    if (try_speex && speex_available()) {
        if (init_speex(quality, &speex_err)) {
            backend_ = Backend::kSpeex;
            if (out_actual != nullptr) {
                *out_actual = Backend::kSpeex;
            }
            return true;
        }
        WOMIC_LOGW("speex 初始化失败：%s", speex_err.c_str());
    }

    if (err != nullptr) {
        *err = "没有可用的重采样后端（需要 libsoxr 或 libspeexdsp）。soxr: " +
               (soxr_err.empty() ? std::string("未尝试") : soxr_err) +
               "；speex: " + (speex_err.empty() ? std::string("未尝试") : speex_err);
    }
    return false;
}

#if defined(WOMIC_HAVE_SOXR)
bool Resampler::init_soxr(uint32_t quality, std::string *err)
{
    /*
     * 档位映射：0..6 -> SOXR_QQ..SOXR_VHQ（soxr 的 quality_spec 采用
     * "recipe + flags" 形式；这里用 recipe 档位直接对应，够用且稳定）。
     * 输入/输出都声明为 SOXR_INT16_I（交错 int16），避免额外的 float 往返。
     */
    unsigned long recipe = SOXR_HQ;
    switch (quality) {
        case 0u:
            recipe = SOXR_QQ;
            break;
        case 1u:
            recipe = SOXR_LQ;
            break;
        case 2u:
            recipe = SOXR_MQ;
            break;
        case 3u:
            recipe = SOXR_HQ;
            break;
        case 4u:
            recipe = SOXR_VHQ;
            break;
        default:
            recipe = SOXR_VHQ;
            break;
    }
    soxr_quality_spec_t qspec = soxr_quality_spec(recipe, 0);
    soxr_io_spec_t iospec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);

    soxr_error_t error = nullptr;
    soxr_t resampler = soxr_create(static_cast<double>(in_rate_), static_cast<double>(out_rate_),
                                   static_cast<unsigned>(channels_), &error, &iospec, &qspec,
                                   nullptr);
    if (error != nullptr) {
        if (err != nullptr) {
            *err = std::string("soxr_create: ") + soxr_strerror(error);
        }
        if (resampler != nullptr) {
            soxr_delete(resampler);
        }
        return false;
    }
    if (resampler == nullptr) {
        if (err != nullptr) {
            *err = "soxr_create 返回空句柄";
        }
        return false;
    }
    handle_ = static_cast<void *>(resampler);
    return true;
}
#else
bool Resampler::init_soxr(uint32_t quality, std::string *err)
{
    (void)quality;
    if (err != nullptr) {
        *err = "编译时未启用 libsoxr";
    }
    return false;
}
#endif

#if defined(WOMIC_HAVE_SPEEXDSP)
bool Resampler::init_speex(uint32_t quality, std::string *err)
{
    int err_code = 0;
    /* speex 的 quality 是 0..10，配置里的 0..6 直接映射过去即可。 */
    int q = static_cast<int>(quality);
    if (q < 0) {
        q = 0;
    }
    if (q > 10) {
        q = 10;
    }
    SpeexResamplerState *st = speex_resampler_init(static_cast<spx_uint32_t>(channels_),
                                                   static_cast<spx_uint32_t>(in_rate_),
                                                   static_cast<spx_uint32_t>(out_rate_), q,
                                                   &err_code);
    if ((st == nullptr) || (err_code != RESAMPLER_ERR_SUCCESS)) {
        if (err != nullptr) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "speex_resampler_init: %s (%d)",
                          speex_resampler_strerror(err_code), err_code);
            *err = buf;
        }
        if (st != nullptr) {
            speex_resampler_destroy(st);
        }
        return false;
    }
    handle_ = static_cast<void *>(st);
    return true;
}
#else
bool Resampler::init_speex(uint32_t quality, std::string *err)
{
    (void)quality;
    if (err != nullptr) {
        *err = "编译时未启用 libspeexdsp";
    }
    return false;
}
#endif

uint32_t Resampler::expected_out_frames(uint32_t in_frames) const
{
    if (!needed_ || (in_rate_ == 0u)) {
        return in_frames;
    }
    uint64_t out = (static_cast<uint64_t>(in_frames) * static_cast<uint64_t>(out_rate_) +
                    static_cast<uint64_t>(in_rate_) - 1u) /
                   static_cast<uint64_t>(in_rate_);
    return static_cast<uint32_t>(out);
}

int Resampler::process(const int16_t *in, uint32_t in_frames, int16_t *out, uint32_t out_capacity)
{
    if ((in == nullptr) || (out == nullptr)) {
        return -1;
    }
    if (in_frames == 0u) {
        return 0;
    }

    if (!needed_) {
        /* 旁路：逐样本拷贝，帧数不变。 */
        uint32_t n = (in_frames < out_capacity) ? in_frames : out_capacity;
        std::memcpy(out, in, static_cast<size_t>(n) * static_cast<size_t>(channels_) *
                                 sizeof(int16_t));
        return static_cast<int>(n);
    }

    if (flushed_) {
        /* soxr 在收到 EOF 后不可再喂数据。 */
        return -2;
    }

#if defined(WOMIC_HAVE_SOXR)
    if (backend_ == Backend::kSoxr) {
        size_t idone = 0u;
        size_t odone = 0u;
        soxr_error_t error =
            soxr_process(static_cast<soxr_t>(handle_), static_cast<const void *>(in),
                         static_cast<size_t>(in_frames), &idone, static_cast<void *>(out),
                         static_cast<size_t>(out_capacity), &odone);
        if (error != nullptr) {
            WOMIC_LOGW("soxr_process 失败: %s", soxr_strerror(error));
            return -3;
        }
        if (idone < static_cast<size_t>(in_frames)) {
            WOMIC_LOGV("soxr 只消费了 %u/%u 输入帧（输出容量 %u）",
                       static_cast<unsigned>(idone), static_cast<unsigned>(in_frames),
                       static_cast<unsigned>(out_capacity));
        }
        return static_cast<int>(odone);
    }
#endif

#if defined(WOMIC_HAVE_SPEEXDSP)
    if (backend_ == Backend::kSpeex) {
        SpeexResamplerState *st = static_cast<SpeexResamplerState *>(handle_);
        const uint32_t ch = (channels_ == 0u) ? 1u : channels_;
        const size_t need_in = static_cast<size_t>(in_frames);
        const size_t need_out = static_cast<size_t>(out_capacity);
        if (scratch_.size() < need_in + need_out) {
            scratch_.resize(need_in + need_out);
        }
        int16_t *deinterleaved = scratch_.data();
        int16_t *resampled = scratch_.data() + need_in;
        uint32_t produced = 0u;

        /* 解交错 -> 逐声道处理 -> 再交错。speex 的 process_int 是单声道 API，
           输入与输出必须使用**不同**缓冲（speex 内部会在 float 域做流水线，
           原地处理会读到已被覆盖的样本）。 */
        for (uint32_t c = 0u; c < ch; ++c) {
            for (uint32_t i = 0u; i < in_frames; ++i) {
                deinterleaved[i] = in[i * ch + c];
            }
            spx_uint32_t in_len = static_cast<spx_uint32_t>(in_frames);
            spx_uint32_t out_len = static_cast<spx_uint32_t>(out_capacity);
            int rc = speex_resampler_process_int(st, static_cast<spx_uint32_t>(c), deinterleaved,
                                                 &in_len, resampled, &out_len);
            if (rc != RESAMPLER_ERR_SUCCESS) {
                WOMIC_LOGW("speex_resampler_process_int 失败(%d)", rc);
                return -3;
            }
            for (uint32_t i = 0u; i < out_len; ++i) {
                out[i * ch + c] = resampled[i];
            }
            if (c == 0u) {
                produced = static_cast<uint32_t>(out_len);
            }
        }
        return static_cast<int>(produced);
    }
#endif

    return -4; /* 需要重采样但没有可用后端 */
}

int Resampler::flush(int16_t *out, uint32_t out_capacity)
{
    if (!needed_) {
        return 0;
    }
    if (out == nullptr) {
        return -1;
    }

#if defined(WOMIC_HAVE_SOXR)
    if (backend_ == Backend::kSoxr) {
        size_t idone = 0u;
        size_t odone = 0u;
        /* in = NULL 表示 EOF：把内部滤波器残留的样本挤出来。 */
        soxr_error_t error = soxr_process(static_cast<soxr_t>(handle_), nullptr, 0u, &idone,
                                          static_cast<void *>(out),
                                          static_cast<size_t>(out_capacity), &odone);
        flushed_ = true;
        if (error != nullptr) {
            WOMIC_LOGW("soxr flush 失败: %s", soxr_strerror(error));
            return -3;
        }
        return static_cast<int>(odone);
    }
#endif

#if defined(WOMIC_HAVE_SPEEXDSP)
    if (backend_ == Backend::kSpeex) {
        /* speex 的输出延迟是有界的（ratio * 滤波阶数），且在整数比下
           输入/输出严格成比例，无需显式 flush；这里返回 0 表示无残留。 */
        (void)out;
        (void)out_capacity;
        flushed_ = true;
        return 0;
    }
#endif

    return 0;
}

}  // namespace womic
