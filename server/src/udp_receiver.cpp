/*
 * udp_receiver.cpp -- UDP 接收实现（平台中立，socket 细节在 net_socket 里）。
 */
#include "udp_receiver.h"

#include <cstring>

#include "log.h"
#include "threads_sync.h"

namespace womic {

static constexpr char kWomicLogModule[] = "udp";

UdpReceiver::~UdpReceiver()
{
    close();
}

bool UdpReceiver::open(uint16_t port, uint32_t bind_ipv4, uint32_t recv_buf_bytes, std::string *err)
{
    UdpOptions opts;
    opts.port = port;
    opts.bind_ipv4 = bind_ipv4;
    opts.non_blocking = true;
    opts.reuse_addr = true;
    opts.enable_broadcast = false;
    opts.recv_buf_bytes = recv_buf_bytes;

    sock_ = udp_open(opts, &bound_port_, err);
    if (sock_ == kInvalidSocket) {
        return false;
    }

    const int actual = udp_get_recvbuf(sock_);
    WOMIC_LOGI("UDP 监听 :%u（请求 SO_RCVBUF=%u，内核实际=%d 字节）",
               static_cast<unsigned>(bound_port_), recv_buf_bytes, actual);
    if ((actual > 0) && (static_cast<uint32_t>(actual) < recv_buf_bytes)) {
        /*
         * Linux 内核会把 SO_RCVBUF 翻倍计入开销，且受 rmem_max 限制；
         * Windows 上若设置值小于默认值也可能被忽略。这里只提示，不失败。
         */
        WOMIC_LOGW("SO_RCVBUF 未达到请求值（%d < %u）：突发抖动时可能有内核丢包，"
                   "可调大 net.core.rmem_max",
                   actual, recv_buf_bytes);
    }
    return true;
}

void UdpReceiver::close()
{
    if (sock_ != kInvalidSocket) {
        udp_close(sock_);
        sock_ = kInvalidSocket;
    }
    has_client_ = false;
    client_ = NetEndpoint();
}

int UdpReceiver::poll(uint32_t timeout_ms, Packet *out, Stats *stats)
{
    if (sock_ == kInvalidSocket) {
        return -1;
    }

    std::string wait_err;
    int ready = udp_wait_readable(sock_, timeout_ms, &wait_err);
    if (ready <= 0) {
        if (ready < 0) {
            WOMIC_LOGW("等待可读失败：%s", wait_err.c_str());
        }
        return ready;
    }

    Packet packet;
    std::string err;
    int n = udp_recvfrom(sock_, packet.data, Packet::kCapacity, &packet.from, &err);
    if (n == 0) {
        return 0; /* 非阻塞：刚好没数据（Windows 上 ICMP 不可达也会走到这里） */
    }
    if (n < 0) {
        WOMIC_LOGW("收包失败：%s", err.c_str());
        return -1;
    }

    packet.size = static_cast<size_t>(n);
    packet.arrival_ms = steady_now_ms();

    /* ---- 客户端连接状态机 ---- */
    if (!has_client_) {
        has_client_ = true;
        client_ = packet.from;
        stats->on_client_connected();
        WOMIC_LOGI("客户端已连接：%s", client_.to_string().c_str());
    } else if (!(packet.from == client_)) {
        /* 已连接的客户端未超时前，忽略其它地址，避免两路流互相打断。 */
        if (stats != nullptr) {
            stats->on_packet_ignored();
        }
        return 0;
    }

    last_packet_ms_ = packet.arrival_ms;
    if (stats != nullptr) {
        stats->on_packet_received(packet.size);
    }
    if (out != nullptr) {
        std::memcpy(out->data, packet.data, packet.size);
        out->size = packet.size;
        out->from = packet.from;
        out->arrival_ms = packet.arrival_ms;
    }
    return 1;
}

bool UdpReceiver::connected() const
{
    return has_client_;
}

NetEndpoint UdpReceiver::client() const
{
    return client_;
}

uint32_t UdpReceiver::last_packet_ms() const
{
    return last_packet_ms_;
}

bool UdpReceiver::check_client_timeout(uint32_t now_ms, uint32_t timeout_ms, Stats *stats)
{
    (void)stats;
    if (!has_client_) {
        return false;
    }
    if (timeout_ms == 0u) {
        return false;
    }
    /* uint32 回绕安全的差值比较。 */
    if ((now_ms - last_packet_ms_) > timeout_ms) {
        WOMIC_LOGI("客户端超时断开：%s（%ums 未收到包）", client_.to_string().c_str(),
                   static_cast<unsigned>(now_ms - last_packet_ms_));
        has_client_ = false;
        client_ = NetEndpoint();
        return true;
    }
    return false;
}

int UdpReceiver::recv_buf_bytes() const
{
    return udp_get_recvbuf(sock_);
}

}  // namespace womic
