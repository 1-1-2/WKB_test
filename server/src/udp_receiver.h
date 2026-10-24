/*
 * udp_receiver.h -- UDP 接收与"客户端连接"维护。
 *
 * 约定：
 *   - 首次收到某个地址的包 => 该地址成为"当前客户端"（连接建立）；
 *   - 已连接期间来自其它地址的包一律忽略并计数（防止多路流互相打断）；
 *   - 超过 client_timeout_ms 没收到当前客户端的包 => 视为断开，等待下一个客户端。
 *
 * socket 是非阻塞的（ioctlsocket FIONBIO / fcntl O_NONBLOCK），等待由
 * udp_wait_readable(select) 完成，因此收包线程可以在超时后顺带推进播放时钟。
 */
#ifndef MICLINK_SERVER_UDP_RECEIVER_H
#define MICLINK_SERVER_UDP_RECEIVER_H

#include <cstdint>
#include <string>

#include "net_socket.h"
#include "stats.h"

namespace miclink {

class UdpReceiver {
public:
    /** 单个收到的数据报（协议头最大 256 + 负载上限 4096）。 */
    struct Packet {
        static constexpr size_t kCapacity = 256u + 4096u;
        uint8_t data[kCapacity];
        size_t size = 0u;
        NetEndpoint from;
        uint32_t arrival_ms = 0u;
    };

    UdpReceiver() = default;
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver &) = delete;
    UdpReceiver &operator=(const UdpReceiver &) = delete;

    /**
     * @brief 打开并绑定 UDP socket。
     * @param port            监听端口
     * @param bind_ipv4       绑定地址（0 = 通配）
     * @param recv_buf_bytes  SO_RCVBUF 目标大小
     */
    bool open(uint16_t port, uint32_t bind_ipv4, uint32_t recv_buf_bytes, std::string *err);

    void close();

    bool is_open() const { return sock_ != kInvalidSocket; }

    /**
     * @brief 等待并收取一个包。
     * @return 1 收到包（写入 out）；0 超时；-1 致命错误
     */
    int poll(uint32_t timeout_ms, Packet *out, Stats *stats);

    /** 当前是否已有客户端。 */
    bool connected() const;

    /** 当前客户端端点（未连接时返回空端点）。 */
    NetEndpoint client() const;

    /** 最近一次收到客户端包的时间（steady ms）。 */
    uint32_t last_packet_ms() const;

    /**
     * @brief 检查客户端是否超时。
     * @return true 表示本次调用触发了"断开"事件
     */
    bool check_client_timeout(uint32_t now_ms, uint32_t timeout_ms, Stats *stats);

    /** 已绑定的实际端口（打开后有效）。 */
    uint16_t bound_port() const { return bound_port_; }

    /** 内核实际接受的 SO_RCVBUF（启动日志核对用）。 */
    int recv_buf_bytes() const;

private:
    SocketHandle sock_ = kInvalidSocket;
    uint16_t bound_port_ = 0u;
    bool has_client_ = false;
    NetEndpoint client_;
    uint32_t last_packet_ms_ = 0u;
};

}  // namespace miclink

#endif  /* MICLINK_SERVER_UDP_RECEIVER_H */
