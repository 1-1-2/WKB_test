/*
 * net_socket.cpp -- 平台适配层 2/3 的实现（winsock2 vs Berkeley sockets）。
 *
 * 除本文件外的所有服务端代码都不接触 socket API，因此 Linux 上跑的端到端测试
 * 与 Windows 上跑的真实服务走的是同一条网络代码路径。
 */
#include "net_socket.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace womic {

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

void fill_addr(sockaddr_in &addr, const NetEndpoint &ep)
{
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port);
    addr.sin_addr.s_addr = htonl(ep.ipv4);
}

void read_addr(const sockaddr_in &addr, NetEndpoint *out)
{
    if (out == nullptr) {
        return;
    }
    out->ipv4 = ntohl(addr.sin_addr.s_addr);
    out->port = ntohs(addr.sin_port);
}

int last_error()
{
#ifdef _WIN32
    return static_cast<int>(WSAGetLastError());
#else
    return errno;
#endif
}

bool would_block(int err)
{
#ifdef _WIN32
    return (err == WSAEWOULDBLOCK) || (err == WSAEINPROGRESS);
#else
    return (err == EAGAIN) || (err == EWOULDBLOCK) || (err == EINPROGRESS);
#endif
}

}  // namespace

std::string NetEndpoint::to_string() const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ipv4 >> 24) & 0xFFu, (ipv4 >> 16) & 0xFFu,
                  (ipv4 >> 8) & 0xFFu, ipv4 & 0xFFu, static_cast<unsigned>(port));
    return std::string(buf);
}

NetEndpoint net_endpoint_from_ipv4(uint32_t ipv4_host_order, uint16_t port)
{
    NetEndpoint ep;
    ep.ipv4 = ipv4_host_order;
    ep.port = port;
    return ep;
}

bool net_endpoint_parse(const char *text, uint16_t default_port, NetEndpoint *out)
{
    if ((text == nullptr) || (out == nullptr)) {
        return false;
    }
    unsigned octets[4] = {0u, 0u, 0u, 0u};
    unsigned port = default_port;
    int parsed = std::sscanf(text, "%u.%u.%u.%u:%u", &octets[0], &octets[1], &octets[2], &octets[3],
                             &port);
    if (parsed < 4) {
        /* 可能是纯 ":port" 或者主机名；主机名解析不在服务端职责内（发现服务用广播）。 */
        int parsed_port = std::sscanf(text, ":%u", &port);
        if (parsed_port == 1) {
            out->ipv4 = 0u;
            out->port = static_cast<uint16_t>(port);
            return true;
        }
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (octets[i] > 255u) {
            return false;
        }
    }
    if (port > 65535u) {
        return false;
    }
    out->ipv4 = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    out->port = static_cast<uint16_t>(port);
    return true;
}

bool net_startup(std::string *err)
{
#ifdef _WIN32
    WSADATA data {};
    int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        if (err != nullptr) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "WSAStartup 失败，错误码=%d", rc);
            *err = buf;
        }
        return false;
    }
    return true;
#else
    (void)err;
    return true;
#endif
}

void net_cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

SocketHandle udp_open(const UdpOptions &opts, uint16_t *bound_port, std::string *err)
{
    int family = AF_INET;
    int type = SOCK_DGRAM;
    int proto = IPPROTO_UDP;

#ifdef _WIN32
    SOCKET raw = ::socket(family, type, proto);
    if (raw == INVALID_SOCKET) {
        if (err != nullptr) {
            *err = std::string("socket() 失败: ") + net_last_error_string(last_error());
        }
        return kInvalidSocket;
    }
#else
    int raw = ::socket(family, type, proto);
    if (raw < 0) {
        if (err != nullptr) {
            *err = std::string("socket() 失败: ") + net_last_error_string(last_error());
        }
        return kInvalidSocket;
    }
#endif

    SocketHandle sock = static_cast<SocketHandle>(raw);

    if (opts.reuse_addr) {
        int on = 1;
#ifdef _WIN32
        setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&on), sizeof(on));
#else
        setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    }

    if (opts.enable_broadcast) {
        int on = 1;
#ifdef _WIN32
        setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char *>(&on), sizeof(on));
#else
        setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
    }

    if (opts.recv_buf_bytes > 0u) {
        std::string ignored;
        udp_set_recvbuf(sock, opts.recv_buf_bytes, &ignored);
    }

    sockaddr_in addr {};
    NetEndpoint bind_ep = net_endpoint_from_ipv4(opts.bind_ipv4, opts.port);
    fill_addr(addr, bind_ep);
#ifdef _WIN32
    int rc = ::bind(static_cast<SOCKET>(sock), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#else
    int rc = ::bind(static_cast<int>(sock), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#endif
    if (rc != 0) {
        if (err != nullptr) {
            *err = std::string("bind() 失败: ") + net_last_error_string(last_error());
        }
        udp_close(sock);
        return kInvalidSocket;
    }

    if (bound_port != nullptr) {
        sockaddr_in actual {};
        SockLen len = sizeof(actual);
#ifdef _WIN32
        int g = getsockname(static_cast<SOCKET>(sock), reinterpret_cast<sockaddr *>(&actual), &len);
#else
        int g = getsockname(static_cast<int>(sock), reinterpret_cast<sockaddr *>(&actual), &len);
#endif
        if (g == 0) {
            *bound_port = ntohs(actual.sin_port);
        } else {
            *bound_port = opts.port;
        }
    }

    if (opts.non_blocking) {
#ifdef _WIN32
        u_long mode = 1u;
        if (ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode) != 0) {
            if (err != nullptr) {
                *err = std::string("ioctlsocket(FIONBIO) 失败: ") + net_last_error_string(last_error());
            }
            udp_close(sock);
            return kInvalidSocket;
        }
#else
        int flags = fcntl(static_cast<int>(sock), F_GETFL, 0);
        if (flags < 0) {
            flags = 0;
        }
        if (fcntl(static_cast<int>(sock), F_SETFL, flags | O_NONBLOCK) < 0) {
            if (err != nullptr) {
                *err = std::string("fcntl(O_NONBLOCK) 失败: ") + net_last_error_string(last_error());
            }
            udp_close(sock);
            return kInvalidSocket;
        }
#endif
    }

    return sock;
}

void udp_close(SocketHandle sock)
{
    if (sock == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(sock));
#else
    ::close(static_cast<int>(sock));
#endif
}

int udp_wait_readable(SocketHandle sock, uint32_t timeout_ms, std::string *err)
{
    if (sock == kInvalidSocket) {
        if (err != nullptr) {
            *err = "udp_wait_readable: 无效 socket";
        }
        return -1;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(sock), &read_set);
#else
    FD_SET(static_cast<int>(sock), &read_set);
#endif

    timeval tv {};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000u);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);

#ifdef _WIN32
    int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
    int rc = ::select(static_cast<int>(sock) + 1, &read_set, nullptr, nullptr, &tv);
#endif
    if (rc > 0) {
        return 1;
    }
    if (rc == 0) {
        return 0;
    }
    int e = last_error();
#ifdef _WIN32
    if (e == WSAEINTR) {
        return 0;
    }
#else
    if (e == EINTR) {
        return 0;
    }
#endif
    if (err != nullptr) {
        *err = std::string("select() 失败: ") + net_last_error_string(e);
    }
    return -1;
}

int udp_recvfrom(SocketHandle sock, uint8_t *buf, size_t cap, NetEndpoint *from, std::string *err)
{
    if (sock == kInvalidSocket) {
        if (err != nullptr) {
            *err = "udp_recvfrom: 无效 socket";
        }
        return -1;
    }

    sockaddr_in src {};
    SockLen src_len = sizeof(src);
#ifdef _WIN32
    int n = ::recvfrom(static_cast<SOCKET>(sock), reinterpret_cast<char *>(buf),
                       static_cast<int>(cap), 0, reinterpret_cast<sockaddr *>(&src), &src_len);
#else
    ssize_t n = ::recvfrom(static_cast<int>(sock), buf, cap, 0,
                           reinterpret_cast<sockaddr *>(&src), &src_len);
#endif
    if (n < 0) {
        int e = last_error();
        if (would_block(e)) {
            return 0; /* 非阻塞：当前无数据 */
        }
#ifdef _WIN32
        if (e == WSAECONNRESET) {
            /* Windows UDP 上 ICMP port unreachable 会以 WSAECONNRESET 报出，
               忽略即可（Berkeley sockets 上这次 recvfrom 会直接返回 0 长度）。 */
            return 0;
        }
#endif
        if (err != nullptr) {
            *err = std::string("recvfrom() 失败: ") + net_last_error_string(e);
        }
        return -1;
    }
    read_addr(src, from);
    return static_cast<int>(n);
}

int udp_sendto(SocketHandle sock, const uint8_t *buf, size_t len, const NetEndpoint &to,
               std::string *err)
{
    if (sock == kInvalidSocket) {
        if (err != nullptr) {
            *err = "udp_sendto: 无效 socket";
        }
        return -1;
    }

    sockaddr_in dst {};
    fill_addr(dst, to);
#ifdef _WIN32
    int n = ::sendto(static_cast<SOCKET>(sock), reinterpret_cast<const char *>(buf),
                     static_cast<int>(len), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
#else
    ssize_t n = ::sendto(static_cast<int>(sock), buf, len, 0,
                         reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
#endif
    if (n < 0) {
        int e = last_error();
        if (would_block(e)) {
            return 0;
        }
        if (err != nullptr) {
            *err = std::string("sendto() 失败: ") + net_last_error_string(e);
        }
        return -1;
    }
    return static_cast<int>(n);
}

bool udp_set_recvbuf(SocketHandle sock, uint32_t bytes, std::string *err)
{
    if (sock == kInvalidSocket) {
        return false;
    }
    int value = static_cast<int>(bytes);
#ifdef _WIN32
    int rc = setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                        reinterpret_cast<const char *>(&value), sizeof(value));
#else
    int rc = setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
#endif
    if (rc != 0) {
        if (err != nullptr) {
            *err = std::string("setsockopt(SO_RCVBUF) 失败: ") + net_last_error_string(last_error());
        }
        return false;
    }
    return true;
}

int udp_get_recvbuf(SocketHandle sock)
{
    if (sock == kInvalidSocket) {
        return -1;
    }
    int value = 0;
    SockLen len = sizeof(value);
#ifdef _WIN32
    int rc = getsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                        reinterpret_cast<char *>(&value), &len);
#else
    int rc = getsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVBUF, &value, &len);
#endif
    if (rc != 0) {
        return -1;
    }
    return value;
}

std::string net_last_error_string(int err)
{
#ifdef _WIN32
    char buf[256];
    std::snprintf(buf, sizeof(buf), "WSA error %d", err);
    /* FormatMessage 需要链接开销且返回本地化串，这里给出稳定可检索的码值。 */
    return std::string(buf);
#else
    char buf[320];
    const char *msg = std::strerror(err);
    std::snprintf(buf, sizeof(buf), "errno %d (%s)", err, (msg != nullptr) ? msg : "unknown");
    return std::string(buf);
#endif
}

}  // namespace womic
