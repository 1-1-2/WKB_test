/*
 * net_socket.h -- 平台适配层 2/3：UDP socket。
 *
 * 只有 net_socket.cpp 含有 Win32 特有代码（winsock2 / WSAPoll / closesocket /
 * WSAGetLastError / ioctlsocket FIONBIO）；Linux 分支使用 Berkeley sockets
 * （close / errno / fcntl O_NONBLOCK）。等待可读统一用 select()，两者语义一致。
 *
 * 关键差异备忘（互联时最容易踩的坑）：
 *   Windows: SOCKET 是无符号句柄，INVALID_SOCKET != -1；closesocket() 而非 close()；
 *            错误码取 WSAGetLastError()，EWOULDBLOCK 叫 WSAEWOULDBLOCK；
 *            非阻塞用 ioctlsocket(FIONBIO)；必须先 WSAStartup()。
 *   Linux  : fd 是 int，-1 表示无效；close()；errno == EAGAIN/EWOULDBLOCK；
 *            fcntl(O_NONBLOCK) 或 MSG_DONTWAIT。
 */
#ifndef WOMIC_SERVER_NET_SOCKET_H
#define WOMIC_SERVER_NET_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace womic {

/** 跨平台 socket 句柄抽象。Windows 上是 SOCKET，Linux 上是 int（均非负）。 */
using SocketHandle = std::uintptr_t;
constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

/** 平台中立的 IPv4 端点（ipv4 为主机字节序，port 为主机字节序）。 */
struct NetEndpoint {
    uint32_t ipv4 = 0;   /* 主机序，如 0x7F000001 表示 127.0.0.1 */
    uint16_t port = 0;   /* 主机序 */

    bool valid() const { return (ipv4 != 0u) || (port != 0u); }
    bool operator==(const NetEndpoint &o) const
    {
        return (ipv4 == o.ipv4) && (port == o.port);
    }
    bool operator!=(const NetEndpoint &o) const { return !(*this == o); }
    /** 形如 "192.168.1.x:5004"。 */
    std::string to_string() const;
};

/** 解析 "a.b.c.d" 或 "a.b.c.d:port"（port 省略时用 default_port）。 */
bool net_endpoint_parse(const char *text, uint16_t default_port, NetEndpoint *out);

/** 由无符号主机序地址构造端点（便于测试传 INADDR_LOOPBACK 等价物）。 */
NetEndpoint net_endpoint_from_ipv4(uint32_t ipv4_host_order, uint16_t port);

/** UDP socket 打开参数。 */
struct UdpOptions {
    uint16_t port = 0;                 /* 0 = 由系统分配（返回实际端口需再查询） */
    uint32_t bind_ipv4 = 0;            /* 0 = 绑定通配地址 */
    bool non_blocking = true;          /* 非阻塞：recvfrom 立即返回 */
    bool reuse_addr = true;            /* SO_REUSEADDR */
    bool enable_broadcast = false;     /* SO_BROADCAST（发现服务用） */
    uint32_t recv_buf_bytes = 0;       /* 0 = 使用系统默认；建议 256KB */
};

/**
 * @brief 进程级网络初始化（Windows: WSAStartup；Linux: no-op）。
 * 必须在使用任何 socket API 之前调用一次。
 */
bool net_startup(std::string *err);

/** 与 net_startup 配对（Windows: WSACleanup）。 */
void net_cleanup();

/** 打开并绑定一个 UDP socket。失败返回 kInvalidSocket。 */
SocketHandle udp_open(const UdpOptions &opts, uint16_t *bound_port, std::string *err);

/** 关闭 socket（Windows: closesocket；Linux: close）。 */
void udp_close(SocketHandle sock);

/**
 * @brief 等待可读（select 封装）。
 * @return 1 可读；0 超时；-1 致命错误（err 写出原因）
 */
int udp_wait_readable(SocketHandle sock, uint32_t timeout_ms, std::string *err);

/**
 * @brief 收一个包（非阻塞 socket 上立即返回）。
 * @return >0 收到的字节数；0 当前无数据（EWOULDBLOCK）；-1 错误
 */
int udp_recvfrom(SocketHandle sock, uint8_t *buf, size_t cap, NetEndpoint *from,
                 std::string *err);

/**
 * @brief 发一个包。
 * @return 发送字节数；-1 错误
 */
int udp_sendto(SocketHandle sock, const uint8_t *buf, size_t len, const NetEndpoint &to,
               std::string *err);

/**
 * @brief 调大内核接收缓冲区（SO_RCVBUF）。
 *
 * 【为什么要调大】UDP 没有流控：手机侧是等时（isochronous）发包，Wi-Fi 上常见
 * 20~80ms 的突发抖动 —— 几十个包会在几毫秒内一起到达。若内核接收队列只有默认
 * 的几十 KB，突发期间队列溢出后内核直接丢弃整个数据报，且**不会**有任何通知，
 * 表现为 jitter buffer 里成片 seq 丢失、听感上是连续爆音。
 * 256KB 在 20ms/64kbps（160 字节/包）下约合 1600 包 = 32 秒音频，
 * 足以吃掉任何可预期的 Wi-Fi 突发，代价只是一块内核非分页内存。
 */
bool udp_set_recvbuf(SocketHandle sock, uint32_t bytes, std::string *err);

/** 查询当前 SO_RCVBUF（字节，用于启动日志核对内核是否真的接受了设置值）。 */
int udp_get_recvbuf(SocketHandle sock);

/** 把最近的 socket 错误码翻译成人类可读字符串。 */
std::string net_last_error_string(int err);

}  // namespace womic

#endif  /* WOMIC_SERVER_NET_SOCKET_H */
