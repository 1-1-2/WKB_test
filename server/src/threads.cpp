/*
 * threads.cpp -- Thread 实现。
 *
 * 线程模型（详见 docs 与 main.cpp）：
 *   [rx]      UDP 接收 + Opus 解码 + 抖动缓冲播放时钟 + 重采样 -> 环形缓冲（生产者）
 *             THREAD_PRIORITY_TIME_CRITICAL
 *   [inject]  环形缓冲 -> DeviceIoControl(WRITE_PCM) + 周期 GET_STATUS
 *             THREAD_PRIORITY_ABOVE_NORMAL
 *   [main]    状态打印与 Ctrl+C（普通优先级）
 *   [discovery]（可选）广播应答，THREAD_PRIORITY_BELOW_NORMAL
 *
 * 为什么 rx 最高而不是 inject 最高：
 *   rx 线程承载**播放时钟**（womic_jb_get 以它被调用的时刻推进播放时间轴），
 *   它的唤醒抖动会 1:1 变成输出抖动；且它的单次工作量小（收包 + 一帧解码），
 *   给它最高优先级不会饿死别人。inject 线程背后有驱动侧环形缓冲
 *   （GET_CAPS.ringCapacityFrames，通常数百毫秒）做吸震，晚一点点不会断流。
 */
#include "threads.h"

#include <chrono>
#include <cstdio>
#include <utility>

#include "log.h"

namespace womic {

static constexpr char kWomicLogModule[] = "thread";

Thread::Thread(std::string name, ThreadPriority prio)
    : name_(std::move(name)), prio_(prio), stop_(false), running_(false)
{
}

Thread::~Thread()
{
    request_stop();
    join(5000);
}

bool Thread::start(Body body)
{
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    th_ = std::thread([this, body]() {
        std::string err;
        if (!set_current_thread_priority(prio_, &err) && !err.empty()) {
            WOMIC_LOGW("线程 %s 设置优先级失败（降级继续）：%s", name_.c_str(), err.c_str());
        }
        set_current_thread_name(name_.c_str());
        WOMIC_LOGV("线程 %s 启动", name_.c_str());

        body(stop_);

        WOMIC_LOGV("线程 %s 退出", name_.c_str());
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void Thread::request_stop()
{
    stop_.store(true, std::memory_order_release);
}

bool Thread::stop_requested() const
{
    return stop_.load(std::memory_order_acquire);
}

bool Thread::running() const
{
    return running_.load(std::memory_order_acquire);
}

bool Thread::joinable() const
{
    return th_.joinable();
}

void Thread::join(uint32_t timeout_ms)
{
    if (!th_.joinable()) {
        return;
    }
    if (timeout_ms == 0u) {
        th_.join();
        return;
    }

    /*
     * std::thread 没有超时 join。这里先给线程 timeout_ms 的时间自己退出；
     * 若它没退出，说明可能卡在阻塞调用上，此时**不** detach（会泄漏栈与句柄），
     * 而是记录一条警告并无限等待 —— 服务端退出路径上宁可慢一点也要干净。
     */
    uint32_t waited = 0u;
    while (running_.load(std::memory_order_acquire) && (waited < timeout_ms)) {
        sleep_ms(5);
        waited += 5u;
    }
    if (running_.load(std::memory_order_acquire)) {
        WOMIC_LOGW("线程 %s 在 %ums 内未退出，转入阻塞等待", name_.c_str(),
                   static_cast<unsigned>(timeout_ms));
    }
    th_.join();
}

}  // namespace womic
