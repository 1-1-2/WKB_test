/*
 * stub_android_log.h -- 本机冒烟测试用的 <android/log.h> 替身。
 * 只提供 opus_jni.cpp 实际用到的 __android_log_print 与三个级别常量。
 */
#ifndef MICLINK_STUB_ANDROID_LOG_H
#define MICLINK_STUB_ANDROID_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6

void __android_log_print(int prio, const char *tag, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* MICLINK_STUB_ANDROID_LOG_H */
