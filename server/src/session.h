/*
 * session.h -- 会话编排：把 UDP 接收、流水线、注入器与两个工作线程组装起来。
 *
 * 线程模型（详见 threads.cpp 顶部注释）：
 *   [rx]      TIME_CRITICAL：收包 -> 解码 -> 抖动缓冲播放时钟 -> 重采样 -> 环形缓冲
 *   [inject]  ABOVE_NORMAL ：环形缓冲 -> WRITE_PCM；周期 GET_STATUS
 *   [discovery]（可选）BELOW_NORMAL：广播应答
 *   [main]    普通：状态打印、Ctrl+C
 *
 * main.cpp 与 tests/test_pipeline.cpp 都用这个类，因此测试跑的就是
 * 生产环境同一套线程编排。
 */
#ifndef WOMIC_SERVER_SESSION_H
#define WOMIC_SERVER_SESSION_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "discovery.h"
#include "injector.h"
#include "pipeline.h"
#include "stats.h"
#include "threads.h"
#include "threads_sync.h"
#include "udp_receiver.h"

namespace womic {

class ServerSession {
public:
    ServerSession();
    ~ServerSession();

    ServerSession(const ServerSession &) = delete;
    ServerSession &operator=(const ServerSession &) = delete;

    /** 打开设备并完成启动时序；不启动线程。 */
    bool init(const ServerConfig &cfg, std::string *err);

    /** 打开 socket 并启动 rx / inject / discovery 线程。 */
    bool start(std::string *err);

    /** 请求停止（不阻塞）。 */
    void request_stop();

    /** 等待所有线程退出并做收尾（flush 重采样残留、关闭设备）。 */
    void join();

    bool running() const { return running_.load(std::memory_order_acquire); }

    Stats &stats() { return stats_; }
    AudioPipeline &pipeline() { return pipeline_; }
    Injector &injector() { return injector_; }
    UdpReceiver &receiver() { return receiver_; }
    DiscoveryService &discovery() { return discovery_; }
    const ServerConfig &config() const { return cfg_; }

    /** 最近一次产出帧的 seq（用于日志与测试观察）。 */
    uint32_t last_produced_seq() const { return last_produced_seq_; }

    /** 估算端到端延迟（ms）：抖动目标 + 用户态环形 + 内核环形。 */
    double estimate_latency_ms(const StatsSnapshot &snap) const;

private:
    void rx_loop(const std::atomic<bool> &stop);
    void inject_loop(const std::atomic<bool> &stop);

    ServerConfig cfg_ {};

    Stats stats_;
    AudioPipeline pipeline_;
    Injector injector_;
    UdpReceiver receiver_;
    DiscoveryService discovery_;

    Thread rx_thread_;
    Thread inject_thread_;

    std::atomic<bool> running_{false};
    uint32_t last_produced_seq_ = 0u;
    uint32_t produced_frame_count_ = 0u;
    WOMIC_STATUS last_status_ {};
    std::vector<int16_t> inject_buf_;
};

}  // namespace womic

#endif  /* WOMIC_SERVER_SESSION_H */
