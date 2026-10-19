/*
 * opus_jni.cpp -- Android 端 libopus 的 JNI 封装实现。
 *
 * 对应 Kotlin 类：com.womic.open.OpusEncoder（包名/类名变更时必须同步改函数名，
 * 否则 System.loadLibrary 之后会出现 UnsatisfiedLinkError）。
 *
 * 音频链路中的职责：
 *   AudioRecord(PCM16) -> nativeEncode -> Opus payload -> Packetizer(32B 头) -> UDP
 *
 * 关于 plc（丢包隐藏）：
 *   - **解码侧的 PLC 由 Windows 服务端完成**（server/src/opus_decoder.cpp 使用
 *     opus_decode(..., data=NULL ...)，本端不参与）；
 *   - 本文件提供的 nativePlc 是**发送侧的前向冗余**：把上一帧的 Opus payload
 *     原样重发一次，由 Packetizer 打上 WOMIC_FLAG_FEC 后发出。服务端若发现
 *     上一包丢失，可用该冗余包补上一帧，属于 Opus 推荐的重传/带内 FEC 用法。
 *     它不能替代服务端 PLC，只是给服务端多一份补救材料。
 */
#include "opus_jni.h"

#include <android/log.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#define WOMIC_LOG_TAG "womic-opus"
#define WOMIC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, WOMIC_LOG_TAG, __VA_ARGS__)
#define WOMIC_LOGW(...) __android_log_print(ANDROID_LOG_WARN, WOMIC_LOG_TAG, __VA_ARGS__)
#define WOMIC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, WOMIC_LOG_TAG, __VA_ARGS__)

namespace {

/** 每个编码器实例的状态。由 nativeCreate 分配，nativeDestroy 释放。 */
struct WomicOpusContext {
    OpusEncoder *encoder;
    int sample_rate;
    int channels;
    int frame_size;                             /* 每帧每声道样本数 */
    unsigned char last_payload[WOMIC_OPUS_MAX_PAYLOAD];
    int last_payload_bytes;                     /* 0 表示还没有可重发的帧 */
    int last_frame_size;
    short pcm_scratch[WOMIC_OPUS_MAX_FRAME];    /* short[] -> native 的落地点 */

    WomicOpusContext()
        : encoder(NULL), sample_rate(0), channels(0), frame_size(0),
          last_payload_bytes(0), last_frame_size(0) {
        memset(last_payload, 0, sizeof(last_payload));
        memset(pcm_scratch, 0, sizeof(pcm_scratch));
    }
};

/**
 * @brief 校验 Opus 帧长的合法性。
 *
 * Opus 只接受 2.5ms 的整数倍：frame_size * 400 % sample_rate == 0。
 * 例：48kHz -> 120/240/480/960/1920/2880；16kHz -> 40/80/160/320/640。
 */
bool womic_opus_valid_frame(int sample_rate, int frame_size) {
    if ((sample_rate != 8000) && (sample_rate != 12000) && (sample_rate != 16000) &&
        (sample_rate != 24000) && (sample_rate != 48000)) {
        return false;
    }
    if ((frame_size <= 0) || (frame_size > WOMIC_OPUS_MAX_FRAME)) {
        return false;
    }
    /* 2.5ms 的整数倍 <=> frame_size * 1000 / sample_rate 是 2.5 的倍数
       <=> frame_size * 400 能被 sample_rate 整除 */
    if (((frame_size * 400) % sample_rate) != 0) {
        return false;
    }
    return true;
}

WomicOpusContext *womic_opus_from_handle(jlong handle) {
    if (handle == 0) {
        return NULL;
    }
    return reinterpret_cast<WomicOpusContext *>(static_cast<intptr_t>(handle));
}

}  // namespace

extern "C" {

/**
 * @brief 创建编码器。
 * @return 成功返回非 0 句柄；失败返回 0。
 */
JNIEXPORT jlong JNICALL
Java_com_womic_open_OpusEncoder_nativeCreate(JNIEnv *env, jobject /*thiz*/, jint sampleRate,
                                             jint channels, jint bitrate, jint frameSize,
                                             jint complexity, jint enableFec) {
    (void)env;
    if ((channels != 1) && (channels != 2)) {
        WOMIC_LOGE("nativeCreate: 非法声道数 %d", static_cast<int>(channels));
        return 0;
    }
    if (!womic_opus_valid_frame(static_cast<int>(sampleRate), static_cast<int>(frameSize))) {
        WOMIC_LOGE("nativeCreate: 非法帧长 sampleRate=%d frameSize=%d",
                   static_cast<int>(sampleRate), static_cast<int>(frameSize));
        return 0;
    }

    WomicOpusContext *ctx = new (std::nothrow) WomicOpusContext();
    if (ctx == NULL) {
        WOMIC_LOGE("nativeCreate: 内存分配失败");
        return 0;
    }

    int error = OPUS_OK;
    ctx->encoder = opus_encoder_create(static_cast<opus_int32>(sampleRate),
                                       static_cast<int>(channels), OPUS_APPLICATION_AUDIO,
                                       &error);
    if ((ctx->encoder == NULL) || (error != OPUS_OK)) {
        WOMIC_LOGE("nativeCreate: opus_encoder_create 失败 error=%d", error);
        delete ctx;
        return 0;
    }

    ctx->sample_rate = static_cast<int>(sampleRate);
    ctx->channels = static_cast<int>(channels);
    ctx->frame_size = static_cast<int>(frameSize);

    /* 语音场景：OPUS_APPLICATION_AUDIO（默认）已兼顾语音与音质，这里不再切换。 */
    opus_encoder_ctl(ctx->encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_COMPLEXITY(static_cast<opus_int32>(complexity)));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_VBR(1));                 /* 语音用受约束 VBR */
    opus_encoder_ctl(ctx->encoder, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_DTX(0));                 /* DTX 交给服务端判断 */
    opus_encoder_ctl(ctx->encoder, OPUS_SET_INBAND_FEC(enableFec != 0 ? 1 : 0));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_PACKET_LOSS_PERC(enableFec != 0 ? 20 : 0));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(ctx->encoder, OPUS_SET_FORCE_CHANNELS(static_cast<opus_int32>(channels)));

    WOMIC_LOGI("nativeCreate: rate=%d ch=%d bitrate=%d frame=%d ok", ctx->sample_rate,
               ctx->channels, static_cast<int>(bitrate), ctx->frame_size);
    return static_cast<jlong>(reinterpret_cast<intptr_t>(ctx));
}

/** 销毁编码器并释放上下文。 */
JNIEXPORT void JNICALL
Java_com_womic_open_OpusEncoder_nativeDestroy(JNIEnv *env, jobject /*thiz*/, jlong handle) {
    (void)env;
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if (ctx == NULL) {
        return;
    }
    if (ctx->encoder != NULL) {
        opus_encoder_destroy(ctx->encoder);
        ctx->encoder = NULL;
    }
    delete ctx;
}

/**
 * @brief 编码一帧 PCM。
 *
 * @param pcm       Java short[]，长度必须 >= frameSize（单声道）或 frameSize*channels
 * @param frameSize 每声道样本数，必须等于 create 时的 frameSize
 * @param out       Java byte[]，输出 payload
 * @return >=0 为 payload 字节数；<0 见 opus_jni.h 的错误码（或 Opus 自身错误码）
 */
JNIEXPORT jint JNICALL
Java_com_womic_open_OpusEncoder_nativeEncode(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                             jshortArray pcm, jint frameSize, jbyteArray out) {
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if ((ctx == NULL) || (ctx->encoder == NULL)) {
        return WOMIC_OPUS_ERR_BAD_HANDLE;
    }
    if ((pcm == NULL) || (out == NULL)) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }
    if ((frameSize <= 0) || (frameSize > WOMIC_OPUS_MAX_FRAME)) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }

    const int samples_to_read = frameSize * ctx->channels;
    const jsize pcm_len = env->GetArrayLength(pcm);
    if (pcm_len < samples_to_read) {
        WOMIC_LOGE("nativeEncode: PCM 数组过短 %d < %d", static_cast<int>(pcm_len),
                   samples_to_read);
        return WOMIC_OPUS_ERR_BAD_ARG;
    }

    env->GetShortArrayRegion(pcm, 0, static_cast<jsize>(samples_to_read), ctx->pcm_scratch);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return WOMIC_OPUS_ERR_JNI;
    }

    unsigned char payload[WOMIC_OPUS_MAX_PAYLOAD];
    const opus_int32 n = opus_encode(ctx->encoder, ctx->pcm_scratch, static_cast<int>(frameSize),
                                     payload, static_cast<opus_int32>(sizeof(payload)));
    if (n < 0) {
        WOMIC_LOGE("nativeEncode: opus_encode 失败 %d", static_cast<int>(n));
        return static_cast<jint>(n);
    }
    if (n > static_cast<opus_int32>(WOMIC_OPUS_MAX_PAYLOAD)) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }

    const jsize out_cap = env->GetArrayLength(out);
    if (out_cap < static_cast<jsize>(n)) {
        return WOMIC_OPUS_ERR_BUF_SMALL;
    }
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                            reinterpret_cast<const jbyte *>(payload));
    if (env->ExceptionCheck() == JNI_TRUE) {
        return WOMIC_OPUS_ERR_JNI;
    }

    /* 记下本帧 payload，供 nativePlc 作为前向冗余重发 */
    memcpy(ctx->last_payload, payload, static_cast<size_t>(n));
    ctx->last_payload_bytes = static_cast<int>(n);
    ctx->last_frame_size = static_cast<int>(frameSize);

    return static_cast<jint>(n);
}

/**
 * @brief 取上一帧 payload 作为前向冗余（FEC 重发）包。
 *
 * @return >0 为重发 payload 的字节数；0 表示尚无历史帧；<0 为错误码。
 */
JNIEXPORT jint JNICALL
Java_com_womic_open_OpusEncoder_nativePlc(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                          jbyteArray out) {
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if ((ctx == NULL) || (ctx->encoder == NULL)) {
        return WOMIC_OPUS_ERR_BAD_HANDLE;
    }
    if (out == NULL) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }
    if (ctx->last_payload_bytes <= 0) {
        return WOMIC_OPUS_ERR_NO_REDUNDANT;
    }

    const jsize out_cap = env->GetArrayLength(out);
    if (out_cap < static_cast<jsize>(ctx->last_payload_bytes)) {
        return WOMIC_OPUS_ERR_BUF_SMALL;
    }
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(ctx->last_payload_bytes),
                            reinterpret_cast<const jbyte *>(ctx->last_payload));
    if (env->ExceptionCheck() == JNI_TRUE) {
        return WOMIC_OPUS_ERR_JNI;
    }
    return static_cast<jint>(ctx->last_payload_bytes);
}

/** 运行时改码率（bps）。 */
JNIEXPORT jint JNICALL
Java_com_womic_open_OpusEncoder_nativeSetBitrate(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                 jint bitrateBps) {
    (void)env;
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if ((ctx == NULL) || (ctx->encoder == NULL)) {
        return WOMIC_OPUS_ERR_BAD_HANDLE;
    }
    /* Opus 合法区间：500 ~ 300000 bps，另有 OPUS_AUTO / OPUS_BITRATE_MAX */
    if ((bitrateBps < 500) || (bitrateBps > 300000)) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }
    return static_cast<jint>(opus_encoder_ctl(ctx->encoder, OPUS_SET_BITRATE(
                                                  static_cast<opus_int32>(bitrateBps))));
}

/** 运行时改复杂度（0..10）。 */
JNIEXPORT jint JNICALL
Java_com_womic_open_OpusEncoder_nativeSetComplexity(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                    jint complexity) {
    (void)env;
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if ((ctx == NULL) || (ctx->encoder == NULL)) {
        return WOMIC_OPUS_ERR_BAD_HANDLE;
    }
    if ((complexity < 0) || (complexity > 10)) {
        return WOMIC_OPUS_ERR_BAD_ARG;
    }
    return static_cast<jint>(opus_encoder_ctl(ctx->encoder, OPUS_SET_COMPLEXITY(
                                                  static_cast<opus_int32>(complexity))));
}

/** 运行时开关带内 FEC。 */
JNIEXPORT jint JNICALL
Java_com_womic_open_OpusEncoder_nativeSetInbandFec(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                   jint enabled) {
    (void)env;
    WomicOpusContext *ctx = womic_opus_from_handle(handle);
    if ((ctx == NULL) || (ctx->encoder == NULL)) {
        return WOMIC_OPUS_ERR_BAD_HANDLE;
    }
    const int rc = opus_encoder_ctl(ctx->encoder, OPUS_SET_INBAND_FEC(enabled != 0 ? 1 : 0));
    if (rc == OPUS_OK) {
        opus_encoder_ctl(ctx->encoder, OPUS_SET_PACKET_LOSS_PERC(enabled != 0 ? 20 : 0));
    }
    return static_cast<jint>(rc);
}

/** 返回底层 libopus 版本字符串（用于 UI 显示与问题定位）。 */
JNIEXPORT jstring JNICALL
Java_com_womic_open_OpusEncoder_nativeVersion(JNIEnv *env, jobject /*thiz*/) {
    const char *v = opus_get_version_string();
    if (v == NULL) {
        v = "unknown";
    }
    return env->NewStringUTF(v);
}

}  // extern "C"
