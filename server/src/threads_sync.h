/*
 * threads_sync.h -- 平台适配层 1/3：同步原语 / 睡眠 / 单调时钟 / 线程优先级。
 *
 * 只有本文件的 .cpp 含有 Win32 特有代码（CRITICAL_SECTION、CONDITION_VARIABLE、
 * SetThreadPriority、QueryPerformanceCounter）；Linux 分支使用等价实现
 * （std::mutex / std::condition_variable / sched_setscheduler / steady_clock）。
 *
 * 设计取舍：
 *   - 音频热路径（jitter -> ring -> IOCTL）上**不使用**本文件的任何锁：
 *     jitter 与 ring 都是 SPSC 无锁结构，Stats 使用原子量，因此不存在
 *     优先级反转的可能（见 THREADS.md 说明）。本文件的锁只用于
 *     UdpReceiver 的端点快照、Discovery 这类低频路径。
 */
#ifndef MICLINK_SERVER_THREADS_SYNC_H
#define MICLINK_SERVER_THREADS_SYNC_H

#include <atomic>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <condition_variable>
#include <mutex>
#endif

#include <string>

namespace miclink {

/** 线程优先级枚举。映射到 Windows 的 THREAD_PRIORITY_* 或 Linux 的 SCHED_FIFO / nice。 */
enum class ThreadPriority : int {
    kLow = 0,      /* 后台：发现服务、状态打印 */
    kNormal = 1,   /* 默认 */
    kHigh = 2,     /* ABOVE_NORMAL：注入线程 */
    kCritical = 3, /* TIME_CRITICAL：UDP 接收 + 播放时钟线程 */
};

#ifdef _WIN32
/** Windows 分支：CRITICAL_SECTION（不可跨进程、不可递归等待，但比 Mutex 快且无句柄）。 */
class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock();
    void unlock();

private:
    friend class CondVar;
    CRITICAL_SECTION cs_;
};

/** Windows 分支：CONDITION_VARIABLE + SleepConditionVariableCS。 */
class CondVar {
public:
    CondVar();
    ~CondVar();
    CondVar(const CondVar &) = delete;
    CondVar &operator=(const CondVar &) = delete;
    /** 等待，最多 timeout_ms 毫秒；被唤醒返回 true，超时返回 false。 */
    bool wait(Mutex &m, uint32_t timeout_ms);
    void notify_one();
    void notify_all();

private:
    CONDITION_VARIABLE cv_;
};
#else
/** Linux 分支：std::mutex（pthread_mutex 的等价封装）。 */
class Mutex {
public:
    Mutex() = default;
    ~Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock() { mu_.lock(); }
    void unlock() { mu_.unlock(); }

private:
    friend class CondVar;
    std::mutex mu_;
};

/** Linux 分支：std::condition_variable + wait_for。 */
class CondVar {
public:
    CondVar() = default;
    ~CondVar() = default;
    CondVar(const CondVar &) = delete;
    CondVar &operator=(const CondVar &) = delete;
    bool wait(Mutex &m, uint32_t timeout_ms);
    void notify_one() { cv_.notify_one(); }
    void notify_all() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
};
#endif

/** RAII 加锁。 */
class MutexGuard {
public:
    explicit MutexGuard(Mutex &m) : m_(m) { m_.lock(); }
    ~MutexGuard() { m_.unlock(); }
    MutexGuard(const MutexGuard &) = delete;
    MutexGuard &operator=(const MutexGuard &) = delete;

private:
    Mutex &m_;
};

/**
 * @brief 单调毫秒时钟（jitter buffer 的播放时钟基准）。
 *
 * 单位毫秒、uint32_t 在 49 天后自然回绕，与 miclink_jitter 的 uint32_t now_ms
 * 约定一致（其差值运算对回绕安全）。
 */
uint32_t steady_now_ms();

/** 单调微秒时钟（用于统计细粒度耗时）。 */
uint64_t steady_now_us();

/** 睡眠毫秒。 */
void sleep_ms(uint32_t ms);

/** 让出当前时间片。 */
void yield_now();

/**
 * @brief 设置**当前**线程的优先级（由线程入口自己调用）。
 * @param p      目标优先级
 * @param err    失败原因（可为 NULL）
 * @return true 成功或“尽力而为后不影响正确性”的降级成功
 */
bool set_current_thread_priority(ThreadPriority p, std::string *err);

/** 设置当前线程名（调试用，Linux 截断到 15 字节）。 */
void set_current_thread_name(const char *name);

/** 停止令牌：各线程循环用其 load() 判断是否退出。 */
using StopFlag = std::atomic<bool>;

}  // namespace miclink

#endif  /* MICLINK_SERVER_THREADS_SYNC_H */
