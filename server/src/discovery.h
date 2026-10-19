/*
 * discovery.h -- 轻量 UDP 发现服务（可选 P1）。
 *
 * 目的：让手机 App 不需要手填 PC 的 IP 与端口。
 * 协议（极简，明文 ASCII，便于跨端调试与抓包排查）：
 *
 *   手机 -> 255.255.255.255:<discovery_port>   "WOMIC-DISCOVER 1"
 *   服务端 -> 单播回复                          "WOMIC-HERE 1 <port> <rate> <channels> <name>"
 *   服务端（可选，--discovery-interval-ms）    周期性向广播地址发 "WOMIC-HERE ..."
 *
 * 只在同一网段的广播域内工作；不依赖 mDNS/SSDP 等重量级组件。
 */
#ifndef WOMIC_SERVER_DISCOVERY_H
#define WOMIC_SERVER_DISCOVERY_H

#include <atomic>
#include <cstdint>
#include <string>

#include "config.h"
#include "net_socket.h"
#include "threads.h"

namespace womic {

class DiscoveryService {
public:
    DiscoveryService();
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService &) = delete;
    DiscoveryService &operator=(const DiscoveryService &) = delete;

    /** 启动发现线程（未启用时直接返回 true）。 */
    bool start(const ServerConfig &cfg, std::string *err);

    /** 停止并回收线程。 */
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    /** 实际绑定的端口（配置里填 0 由系统分配时用得上，测试也会读它）。 */
    uint16_t bound_port() const { return bound_port_; }

    /** 查询用的广播地址（测试可见）。 */
    static NetEndpoint broadcast_endpoint(uint16_t port);

    /** 构造应答报文。 */
    static std::string build_reply(uint16_t audio_port, uint32_t rate, uint32_t channels,
                                   const std::string &name);

private:
    void run(const std::atomic<bool> &stop);

    ServerConfig cfg_ {};
    SocketHandle sock_ = kInvalidSocket;
    uint16_t bound_port_ = 0u;
    Thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace womic

#endif  /* WOMIC_SERVER_DISCOVERY_H */
