/*
 * log.h -- 服务端日志：级别 + 时间戳 + 模块名，单次 write(2) 落盘。
 *
 * 实时性约束（重要）：
 *   - 不使用 iostream。iostream 的 locale/ facet 初始化与每次 << 的虚调用
 *     在音频线程上会带来不可预测的抖动；这里用 vsnprintf 格式化到栈上
 *     固定缓冲，再用一次 fwrite 写到 stderr，单次系统调用、无锁、无分配。
 *   - 日志点全部位于非热路径（状态打印、异常、状态切换）。电平日志
 *     默认关闭，开启后由 Stats 侧按 1s 节流，绝不会每帧调用。
 */
#ifndef WOMIC_SERVER_LOG_H
#define WOMIC_SERVER_LOG_H

#if defined(__GNUC__) || defined(__clang__)
#define WOMIC_LOG_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define WOMIC_LOG_FMT(a, b)
#endif

namespace womic {

/** 日志级别。数值越大越啰嗦；运行时按 log_level() 截断。 */
enum class LogLevel : int {
    kError = 0,   /* 致命 / 无法继续 */
    kWarn = 1,    /* 可恢复异常（丢包、缓冲满、格式切换） */
    kInfo = 2,    /* 正常状态（连接、启动、每秒统计） */
    kVerbose = 3, /* --verbose：每帧级/每包级细节 */
};

/** 初始化日志（进程启动时调用一次）。 */
void log_init(LogLevel level, bool with_timestamp);

/** 运行时调整级别（--verbose / --quiet）。 */
void log_set_level(LogLevel level);

/** 当前级别。 */
LogLevel log_level();

/** 该级别当前是否会输出。 */
bool log_enabled(LogLevel level);

/** 级别名（error/warn/info/verbose）。 */
const char *log_level_name(LogLevel level);

/** 由字符串解析级别，成功返回 true。 */
bool log_level_parse(const char *text, LogLevel *out);

/**
 * @brief 写一条日志。外部请优先使用 WOMIC_LOG* 宏。
 * @param level  级别
 * @param module 模块名（各 .cpp 内定义 kWomicLogModule）
 * @param fmt    printf 风格格式串
 */
void log_write(LogLevel level, const char *module, const char *fmt, ...) WOMIC_LOG_FMT(3, 4);

}  // namespace womic

/* --- 便捷宏：每个 .cpp 需先定义 static constexpr char kWomicLogModule[] --- */
#define WOMIC_LOGE(fmt, ...) \
    ::womic::log_write(::womic::LogLevel::kError, kWomicLogModule, fmt, ##__VA_ARGS__)
#define WOMIC_LOGW(fmt, ...) \
    ::womic::log_write(::womic::LogLevel::kWarn, kWomicLogModule, fmt, ##__VA_ARGS__)
#define WOMIC_LOGI(fmt, ...) \
    ::womic::log_write(::womic::LogLevel::kInfo, kWomicLogModule, fmt, ##__VA_ARGS__)
#define WOMIC_LOGV(fmt, ...) \
    ::womic::log_write(::womic::LogLevel::kVerbose, kWomicLogModule, fmt, ##__VA_ARGS__)

#endif  /* WOMIC_SERVER_LOG_H */
