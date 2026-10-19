/*
 * threads.h -- 线程封装（std::thread 优先，跨 Win/Linux 一致）。
 *
 * 职责：
 *   - 统一“命名 + 优先级 + 停止令牌”三件套，避免每个线程各写一遍；
 *   - 线程入口第一件事就是调用 set_current_thread_priority()（见 threads_sync），
 *     保证优先级在线程真正开始处理音频之前已经生效；
 *   - 停止用 atomic<bool> 令牌 + join()，不用 TerminateThread / pthread_cancel
 *     （后者可能在持有锁或内核句柄时把线程杀掉，驱动句柄会泄漏）。
 */
#ifndef MICLINK_SERVER_THREADS_H
#define MICLINK_SERVER_THREADS_H

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "threads_sync.h"

namespace miclink {

class Thread {
public:
    /** 线程体：收到停止请求后应尽快从循环返回。 */
    using Body = std::function<void(const std::atomic<bool> &stop)>;

    /**
     * @param name 线程名（调试器 / 日志可见）
     * @param prio 优先级：见 THREAD_POLICY 说明
     */
    Thread(std::string name, ThreadPriority prio);

    /** 析构：请求停止并 join（绝不 detach，避免资源泄漏）。 */
    ~Thread();

    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    /** 启动线程；已在运行则返回 false。 */
    bool start(Body body);

    /** 请求停止（设置令牌，不阻塞）。 */
    void request_stop();

    /** 是否已请求停止。 */
    bool stop_requested() const;

    /** 线程是否仍在运行。 */
    bool running() const;

    /** 等待线程结束（最多 timeout_ms 毫秒；0 表示无限等待）。 */
    void join(uint32_t timeout_ms = 0);

    /** 是否可 join。 */
    bool joinable() const;

    const std::string &name() const { return name_; }

private:
    std::string name_;
    ThreadPriority prio_;
    std::atomic<bool> stop_;
    std::atomic<bool> running_;
    std::thread th_;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_THREADS_H */
