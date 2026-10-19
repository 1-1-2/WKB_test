/*
 * discovery.cpp -- 发现服务实现（平台中立，socket 细节在 net_socket）。
 */
#include "discovery.h"

#include <cstring>

#include "log.h"
#include "threads_sync.h"

namespace womic {

static constexpr char kWomicLogModule[] = "discov";

namespace {
constexpr uint32_t kBroadcastIpv4 = 0xFFFFFFFFu; /* 255.255.255.255 */
constexpr uint32_t kPollTimeoutMs = 200u;
constexpr size_t kQueryBufferSize = 512u;
}  // namespace

DiscoveryService::DiscoveryService() : thread_("discovery", ThreadPriority::kLow) {}

DiscoveryService::~DiscoveryService()
{
    stop();
}

NetEndpoint DiscoveryService::broadcast_endpoint(uint16_t port)
{
    return net_endpoint_from_ipv4(kBroadcastIpv4, port);
}

std::string DiscoveryService::build_reply(uint16_t audio_port, uint32_t rate, uint32_t channels,
                                          const std::string &name)
{
    char buf[512];
    /* 名称中的空格会破坏分词解析，统一替换成 '_'。 */
    std::string safe_name = name;
    for (char &c : safe_name) {
        if ((c == ' ') || (c == '\t')) {
            c = '_';
        }
    }
    std::snprintf(buf, sizeof(buf), "WOMIC-HERE 1 %u %u %u %s", static_cast<unsigned>(audio_port),
                  rate, channels, safe_name.c_str());
    return std::string(buf);
}

bool DiscoveryService::start(const ServerConfig &cfg, std::string *err)
{
    if (!cfg.discovery_enabled) {
        return true;
    }
    cfg_ = cfg;

    UdpOptions opts;
    opts.port = cfg.discovery_port;
    opts.bind_ipv4 = 0u;
    opts.non_blocking = true;
    opts.reuse_addr = true;
    opts.enable_broadcast = true;
    opts.recv_buf_bytes = 0u;

    uint16_t bound = 0u;
    sock_ = udp_open(opts, &bound, err);
    if (sock_ == kInvalidSocket) {
        return false;
    }
    bound_port_ = bound;

    running_.store(true, std::memory_order_release);
    if (!thread_.start([this](const std::atomic<bool> &stop) { this->run(stop); })) {
        running_.store(false, std::memory_order_release);
        if (err != nullptr) {
            *err = "发现线程启动失败";
        }
        return false;
    }
    WOMIC_LOGI("发现服务已启动：UDP :%u（音频端口 %u，名称 %s）",
               static_cast<unsigned>(bound), static_cast<unsigned>(cfg.listen_port),
               cfg.discovery_name.c_str());
    return true;
}

void DiscoveryService::stop()
{
    running_.store(false, std::memory_order_release);
    thread_.request_stop();
    thread_.join(3000u);
    if (sock_ != kInvalidSocket) {
        udp_close(sock_);
        sock_ = kInvalidSocket;
    }
}

void DiscoveryService::run(const std::atomic<bool> &stop)
{
    uint32_t last_broadcast_ms = 0u;

    while (!stop.load(std::memory_order_acquire)) {
        uint8_t query[kQueryBufferSize];
        NetEndpoint from;
        std::string err;

        std::string wait_err;
        if (udp_wait_readable(sock_, kPollTimeoutMs, &wait_err) > 0) {
            int n = udp_recvfrom(sock_, query, sizeof(query), &from, &err);
            if (n > 0) {
                std::string text(reinterpret_cast<const char *>(query), static_cast<size_t>(n));
                if (text.compare(0, 14, "WOMIC-DISCOVER") == 0) {
                    const std::string reply = build_reply(
                        cfg_.listen_port, cfg_.endpoint_sample_rate, cfg_.endpoint_channels,
                        cfg_.discovery_name);
                    std::string send_err;
                    int sent = udp_sendto(sock_, reinterpret_cast<const uint8_t *>(reply.data()),
                                          reply.size(), from, &send_err);
                    if (sent < 0) {
                        WOMIC_LOGW("发现应答发送失败：%s", send_err.c_str());
                    } else {
                        WOMIC_LOGV("应答发现请求：%s -> %s", from.to_string().c_str(),
                                   reply.c_str());
                    }
                }
            }
        }

        if (cfg_.discovery_interval_ms > 0u) {
            const uint32_t now = steady_now_ms();
            if ((now - last_broadcast_ms) >= cfg_.discovery_interval_ms) {
                last_broadcast_ms = now;
                const std::string announcement = build_reply(
                    cfg_.listen_port, cfg_.endpoint_sample_rate, cfg_.endpoint_channels,
                    cfg_.discovery_name);
                std::string send_err;
                udp_sendto(sock_, reinterpret_cast<const uint8_t *>(announcement.data()),
                           announcement.size(), broadcast_endpoint(cfg_.discovery_port),
                           &send_err);
            }
        }
    }
}

}  // namespace womic
