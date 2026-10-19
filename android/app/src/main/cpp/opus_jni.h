/*
 * opus_jni.h -- Android 端 libopus 的 JNI 封装（内部头文件）。
 *
 * 对上（Kotlin）只暴露 com.womic.open.OpusEncoder 一个类的 native 方法：
 *   nativeCreate / nativeDestroy / nativeEncode / nativePlc
 *   nativeSetBitrate / nativeSetComplexity / nativeSetInbandFec / nativeVersion
 *
 * 设计约束：
 *   1) 不在 native 层持有任何 Java 对象引用（不做缓存，避免 GC 与线程问题）；
 *   2) 所有 Java 数组访问都走 Get/Set<Type>ArrayRegion，且先校验长度再拷贝；
 *   3) 任何非法参数都返回负值错误码，绝不抛异常穿越 JNI 边界、绝不崩溃。
 */
#ifndef WOMIC_OPUS_JNI_H
#define WOMIC_OPUS_JNI_H

#include <jni.h>

/* 单包 payload 上限，与 shared/core/include/womic/womic_protocol.h 的
 * WOMIC_PROTO_MAX_PAYLOAD（4096）保持一致。 */
#define WOMIC_OPUS_MAX_PAYLOAD 4096

/* Opus 单帧最大样本数（120ms @ 48kHz） */
#define WOMIC_OPUS_MAX_FRAME 5760

/* 返回给 Java 的错误码（负值） */
#define WOMIC_OPUS_ERR_BAD_HANDLE   (-1)   /* handle 为 0 或非法的上下文 */
#define WOMIC_OPUS_ERR_BAD_ARG      (-2)   /* 参数非法（采样率/声道/帧长） */
#define WOMIC_OPUS_ERR_BUF_SMALL    (-3)   /* Java 侧输出数组太小 */
#define WOMIC_OPUS_ERR_JNI          (-4)   /* JNI 数组访问失败（已抛异常） */
#define WOMIC_OPUS_ERR_NO_REDUNDANT (-5)   /* plc 请求时还没有可重发的上一帧 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* WOMIC_OPUS_JNI_H */
