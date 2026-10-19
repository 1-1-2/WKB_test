/*
 * opus_stub_impl.cpp -- **假的 libopus 实现**，只服务于本机 JNI 冒烟测试。
 *
 * 行为（固定、可预测，便于断言）：
 *   - opus_encoder_create：申请一个空结构，*error = 0，返回非 NULL；
 *   - opus_encode：校验 st / data / max_data_bytes / frame_size，写 10 字节 0xAB，返回 10；
 *   - opus_encoder_ctl：一律返回 OPUS_OK（0）；
 *   - opus_get_version_string：返回 "stub-opus（非真实 Opus）"。
 *
 * 绝不能被 Android 构建使用。
 */
#include "stub_opus.h"
#include "stub_android_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OpusEncoder {
    int fs;
    int channels;
    int bitrate;
};

extern "C" {

OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error) {
    OpusEncoder *st = static_cast<OpusEncoder *>(calloc(1, sizeof(OpusEncoder)));
    (void)application;
    if (st == NULL) {
        if (error != NULL) {
            *error = -1;
        }
        return NULL;
    }
    st->fs = static_cast<int>(Fs);
    st->channels = channels;
    st->bitrate = 64000;
    if (error != NULL) {
        *error = 0;
    }
    return st;
}

void opus_encoder_destroy(OpusEncoder *st) {
    free(st);
}

opus_int32 opus_encode(OpusEncoder *st, const short *pcm, int frame_size,
                       unsigned char *data, opus_int32 max_data_bytes) {
    (void)pcm;
    if ((st == NULL) || (data == NULL) || (max_data_bytes < 10) || (frame_size <= 0)) {
        return -1;
    }
    memset(data, 0xAB, 10);
    return 10;
}

int opus_encoder_ctl(OpusEncoder *st, int request, ...) {
    (void)st;
    (void)request;
    return OPUS_OK;
}

const char *opus_get_version_string(void) {
    return "stub-opus（非真实 Opus）";
}

void __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[stub-log/%d/%s] ", prio, tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

}  // extern "C"
