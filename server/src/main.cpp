/*
 * main.cpp -- womicserver 入口：解析参数 -> 校验 -> 启动会话 -> 状态打印 -> 优雅退出。
 *
 * 退出路径（Ctrl+C / SIGINT / SIGTERM）：
 *   设置停止标志 -> 停止 rx/inject 线程 -> flush 重采样残留 -> STOP/RESET/CloseHandle。
 * 绝不使用 TerminateThread / ExitProcess 这类粗暴方式：那会让驱动端点停在
 * RUNNING 状态、环形缓冲留有脏数据，下一次启动会听到上一轮的残余音频。
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"
#include "net_socket.h"
#include "session.h"
#include "threads_sync.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

std::atomic<bool> g_stop_requested{false};

void request_stop()
{
    g_stop_requested.store(true, std::memory_order_release);
}

#ifdef _WIN32
/*
 * Windows 控制台控制处理器：运行在**系统创建的独立线程**中，
 * 因此只能做异步信号安全的操作（设置标志 + 返回 TRUE），
 * 不能 join 线程、不能做 I/O、不能分配内存。
 */
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            request_stop();
            return TRUE;
        default:
            return FALSE;
    }
}

bool install_signal_handlers(std::string *err)
{
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        if (err != nullptr) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "SetConsoleCtrlHandler 失败，GetLastError=%lu",
                          static_cast<unsigned long>(GetLastError()));
            *err = buf;
        }
        return false;
    }
    return true;
}

void uninstall_signal_handlers()
{
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
}
#else
void womic_posix_signal_handler(int signo)
{
    (void)signo;
    request_stop();
}

bool install_signal_handlers(std::string *err)
{
    struct sigaction sa {};
    sa.sa_handler = womic_posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if ((sigaction(SIGINT, &sa, nullptr) != 0) || (sigaction(SIGTERM, &sa, nullptr) != 0)) {
        if (err != nullptr) {
            *err = "sigaction(SIGINT/SIGTERM) 失败";
        }
        return false;
    }
    return true;
}

void uninstall_signal_handlers()
{
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}
#endif

/** 打印一条状态行（含可选电平与延迟估计）。 */
void print_status_line(womic::ServerSession &session, const womic::ServerConfig &cfg)
{
    const womic::StatsSnapshot snap = session.stats().snapshot();
    const double latency_ms = session.estimate_latency_ms(snap);

    std::string line = session.stats().format_line(0.0);
    std::fprintf(stdout, "  %s | 延迟~%.1f ms", line.c_str(), latency_ms);
    if (cfg.enable_level_log) {
        std::fprintf(stdout, " | RMS %.3f peak %.3f", static_cast<double>(snap.level_rms),
                     static_cast<double>(snap.level_peak));
    }
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv)
{
    womic::ServerConfig cfg;
    std::string err;

    if (!womic::config_parse(argc, argv, &cfg, &err)) {
        std::fprintf(stderr, "参数解析失败: %s\n\n", err.c_str());
        womic::config_print_usage((argc > 0) ? argv[0] : "womicserver");
        return 2;
    }

    if (cfg.show_help) {
        womic::config_print_usage((argc > 0) ? argv[0] : "womicserver");
        return 0;
    }
    if (cfg.show_version) {
        std::fprintf(stdout, "%s\n", womic::config_version_string());
        return 0;
    }

    womic::log_init(static_cast<womic::LogLevel>(static_cast<int>(cfg.log_level)),
                    cfg.log_timestamp);

    if (!womic::config_validate(cfg, &err)) {
        std::fprintf(stderr, "配置校验失败: %s\n", err.c_str());
        return 2;
    }

    if (cfg.dry_run) {
        std::fprintf(stdout, "%s\n", womic::config_to_string(cfg).c_str());
        return 0;
    }

    if (!womic::net_startup(&err)) {
        std::fprintf(stderr, "网络初始化失败: %s\n", err.c_str());
        return 1;
    }

    womic::ServerSession session;
    if (!session.init(cfg, &err)) {
        std::fprintf(stderr, "启动失败: %s\n", err.c_str());
        womic::net_cleanup();
        return 1;
    }
    if (!session.start(&err)) {
        std::fprintf(stderr, "启动失败: %s\n", err.c_str());
        womic::net_cleanup();
        return 1;
    }

    if (!install_signal_handlers(&err)) {
        std::fprintf(stderr, "警告：无法安装退出信号处理器: %s\n", err.c_str());
    }

    std::fprintf(stdout, "%s\n", womic::config_to_string(cfg).c_str());
    std::fprintf(stdout, "运行中，按 Ctrl+C 退出。\n");

    uint32_t last_status_ms = womic::steady_now_ms();
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        womic::sleep_ms(100);
        if (cfg.status_interval_ms == 0u) {
            continue;
        }
        const uint32_t now = womic::steady_now_ms();
        if ((now - last_status_ms) >= cfg.status_interval_ms) {
            last_status_ms = now;
            print_status_line(session, cfg);
        }
    }

    std::fprintf(stdout, "\n正在停止...\n");
    session.request_stop();
    session.join();
    uninstall_signal_handlers();

    std::fprintf(stdout, "---- 运行汇总 ----\n%s\n", session.stats().format_summary().c_str());
    womic::net_cleanup();
    return 0;
}
