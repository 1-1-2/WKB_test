/*
 * stub_opus.h -- **仅供本机冒烟测试的假 libopus**（不是真正的 Opus！）
 *
 * 目的：在没有 Android NDK、也不想下载几 MB Opus 源码的情况下，
 * 仍然可以验证 opus_jni.cpp 的 **JNI 符号名、参数签名、错误码返回**
 * 是否与 Kotlin 侧 OpusEncoder.kt 声明的一致。
 *
 * 真正的 Opus 由使用者按 android/README.md 下载到 app/src/main/cpp/opus/。
 * 本文件绝不参与 Android 构建（它不在 app/src/main/cpp 下）。
 */
#ifndef WOMIC_STUB_OPUS_H
#define WOMIC_STUB_OPUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t opus_int32;
typedef struct OpusEncoder OpusEncoder;

#define OPUS_OK 0
#define OPUS_APPLICATION_AUDIO 2049
#define OPUS_SIGNAL_VOICE 3001

/* 真 opus.h 里这些是 "请求码, 值" 的二元组宏；这里为可编译而简化为同样的形状 */
#define OPUS_SET_BITRATE(x) 4002, (x)
#define OPUS_SET_COMPLEXITY(x) 4010, (x)
#define OPUS_SET_VBR(x) 4006, (x)
#define OPUS_SET_VBR_CONSTRAINT(x) 4020, (x)
#define OPUS_SET_SIGNAL(x) 4024, (x)
#define OPUS_SET_DTX(x) 4016, (x)
#define OPUS_SET_INBAND_FEC(x) 4012, (x)
#define OPUS_SET_PACKET_LOSS_PERC(x) 4014, (x)
#define OPUS_SET_LSB_DEPTH(x) 4032, (x)
#define OPUS_SET_FORCE_CHANNELS(x) 4022, (x)

OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
opus_int32 opus_encode(OpusEncoder *st, const short *pcm, int frame_size,
                       unsigned char *data, opus_int32 max_data_bytes);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
const char *opus_get_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* WOMIC_STUB_OPUS_H */
