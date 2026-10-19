/*
 * log.cpp -- 日志实现。
 *
 * 输出到 stderr（重定向到文件/控制台都方便，且不占用 stdout 便于脚本解析）。
 * 每条日志 = 一次 vsnprintf + 一次 fwrite，无锁：即便多线程同时写，
 * 单条长度 < PIPE_BUF(4096) 的写操作在 POSIX 上是原子的，不会出现半行交错。
 * Windows 下 stderr 无此保证，但日志频率极低且仅用于诊断，可接受。
 */
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace womic {
namespace {

constexpr int kLogBufferSize = 1024;

LogLevel g_level = LogLevel::kInfo;
bool g_with_timestamp = true;

/* 线程安全地取当前本地时间的 HH:MM:SS.mmm。 */
void format_timestamp(char *dst, size_t dst_size)
{
    if (dst_size == 0u) {
        return;
    }
    dst[0] = '\0';

    std::time_t now = std::time(nullptr);
    std::tm tm_buf {};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    /* 毫秒部分：steady_clock 不能给墙钟毫秒，这里取 time(NULL) 之外的
       CLOCK_REALTIME（Windows 用系统时间），仅用于显示，失败不影响日志。 */
    int millis = 0;
#if defined(_WIN32)
    SYSTEMTIME st {};
    GetSystemTime(&st);
    millis = static_cast<int>(st.wMilliseconds);
#else
    struct timespec ts {};
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        millis = static_cast<int>(ts.tv_nsec / 1000000);
    }
#endif
    std::snprintf(dst, dst_size, "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec, millis);
}

}  // namespace

void log_init(LogLevel level, bool with_timestamp)
{
    g_level = level;
    g_with_timestamp = with_timestamp;
}

void log_set_level(LogLevel level)
{
    g_level = level;
}

LogLevel log_level()
{
    return g_level;
}

bool log_enabled(LogLevel level)
{
    return static_cast<int>(level) <= static_cast<int>(g_level);
}

const char *log_level_name(LogLevel level)
{
    switch (level) {
        case LogLevel::kError:
            return "error";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kVerbose:
            return "verbose";
        default:
            return "unknown";
    }
}

bool log_level_parse(const char *text, LogLevel *out)
{
    if ((text == nullptr) || (out == nullptr)) {
        return false;
    }
    if ((std::strcmp(text, "error") == 0) || (std::strcmp(text, "0") == 0)) {
        *out = LogLevel::kError;
        return true;
    }
    if ((std::strcmp(text, "warn") == 0) || (std::strcmp(text, "warning") == 0) ||
        (std::strcmp(text, "1") == 0)) {
        *out = LogLevel::kWarn;
        return true;
    }
    if ((std::strcmp(text, "info") == 0) || (std::strcmp(text, "2") == 0)) {
        *out = LogLevel::kInfo;
        return true;
    }
    if ((std::strcmp(text, "verbose") == 0) || (std::strcmp(text, "debug") == 0) ||
        (std::strcmp(text, "3") == 0)) {
        *out = LogLevel::kVerbose;
        return true;
    }
    return false;
}

void log_write(LogLevel level, const char *module, const char *fmt, ...)
{
    if (!log_enabled(level)) {
        return;
    }

    char line[kLogBufferSize];
    int written = 0;

    if (g_with_timestamp) {
        char stamp[32];
        format_timestamp(stamp, sizeof(stamp));
        written = std::snprintf(line, sizeof(line), "[%s][%s][%s] ", stamp,
                                log_level_name(level), (module != nullptr) ? module : "-");
    } else {
        written = std::snprintf(line, sizeof(line), "[%s][%s] ", log_level_name(level),
                                (module != nullptr) ? module : "-");
    }
    if ((written < 0) || (static_cast<size_t>(written) >= sizeof(line))) {
        written = static_cast<int>(sizeof(line)) - 1;
    }

    std::va_list args;
    va_start(args, fmt);
    int body = std::vsnprintf(line + written, sizeof(line) - static_cast<size_t>(written), fmt, args);
    va_end(args);
    if (body < 0) {
        body = 0;
    }

    size_t total = static_cast<size_t>(written) + static_cast<size_t>(body);
    if (total >= sizeof(line)) {
        total = sizeof(line) - 1u;
    }
    line[total] = '\n';

    /* 单次写：尽量保证多线程日志不会交错成半行。 */
    std::fwrite(line, 1, total + 1u, stderr);
}

}  // namespace womic
