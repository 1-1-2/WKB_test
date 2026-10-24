#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""udp-relay —— 单向 UDP 中继（tailscale-dual 双机联调，job A：Linux/模拟器侧）。

在跨网真链路中的位置（拓扑全貌与选型理由见 .github/workflows/tailscale-dual.yml
顶部注释）：

    模拟器内真 app ──UDP 格式 C──► 10.0.2.2:8126
        （app 的 UDP 目标地址取自 TCP 对端，slirp 把网关地址投递到 host loopback，
          见 android/verify/integration/README.md「直连拓扑」段）
                                        │
                                   udp-relay（本脚本）
                                   bind 0.0.0.0:8126
                                        │
                                        ▼
                     <windows-client 的 tailnet IP>:8126
                                        │
                        womic.exe --media-port 8126 收流 → 解码 → 灌驱动出声

为什么是严格单向中继（不做回程 NAT 表）：
    client/src/engine.c 的媒体面只有 recv(g_udp, ...)（→ on_udp_frame），全文没有
    任何 PC→app 的 UDP 发送；协议规格 v1 的角色图同样只有「手机单向下发」
    （engine.c 顶部注释）。PC 侧不需要回包，因此中继收到什么就转发什么，
    无需按对端地址建会话表，也无需处理回程。

用法：
    python3 udp-relay.py --listen-port 8126 --target 100.x.y.z --target-port 8126 \
        [--stats /tmp/udp-relay-stats.json] [--duration 600] [--bind 0.0.0.0]

行为契约（workflow 依赖这些输出做断言与取证）：
    - 每 5s 向 stdout 打一行进度（累计包数/字节数/转发失败数/最近源地址）；
    - --stats 指向的文件每 5s 落盘一次 JSON，工作流可在中继运行期间随时读取；
    - 收到 SIGTERM/SIGINT 或 --duration 到期：打印最终统计后以退出码 0 结束；
    - 单个转发失败（对端暂不可达等）只计数、不崩溃——联调启动阶段的噪声属预期。
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import sys
import time

DEFAULT_BIND = "0.0.0.0"
DEFAULT_LISTEN_PORT = 8126
DEFAULT_TARGET_PORT = 8126
STATS_INTERVAL_S = 5.0
RECV_BUFSIZE = 65535


class UdpRelay:
    """收 listen 端口的数据报 → 原样转发到 (target, target_port)。"""

    def __init__(self, bind_host: str, listen_port: int,
                 target: str, target_port: int, stats_path: str = "",
                 duration: float = 0.0):
        self.bind_host = bind_host
        self.listen_port = listen_port
        self.target = target
        self.target_port = target_port
        self.stats_path = stats_path
        self.duration = duration
        self.stop_requested = False
        self.sock: socket.socket | None = None
        self.started_at = time.time()
        self.packets = 0            # 收到的数据报数（来自 app 侧）
        self.bytes_in = 0           # 收到的总字节数
        self.forwarded = 0          # 成功转发到 target 的数据报数
        self.bytes_out = 0          # 成功转发的总字节数
        self.send_errors = 0        # 转发失败次数（容忍，不致命）
        self.sources: set[str] = set()   # 出现过的源地址（ip:port）
        self.last_peer = ""

    # ------------------------------------------------------------ 信号
    def install_signal_handlers(self) -> None:
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                signal.signal(sig, self._on_signal)
            except (ValueError, OSError):
                # 非主线程等场景下注册失败：不影响运行，靠 --duration 兜底
                pass

    def _on_signal(self, signum, _frame) -> None:  # noqa: ANN001
        print("[relay] 收到信号 %d，准备收尾" % signum, flush=True)
        self.stop_requested = True

    # ------------------------------------------------------------ 生命周期
    def setup(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.bind_host, self.listen_port))
        # 大接收缓冲：联调窗口内 app 以 ~50 pps 推送，突发也不会丢
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        except OSError:
            pass
        self.sock.settimeout(0.5)

    def run(self) -> int:
        self.setup()
        self.install_signal_handlers()
        print("[relay] UDP %s:%d -> %s:%d（%s）"
              % (self.bind_host, self.listen_port, self.target, self.target_port,
                 "限时 %.0fs" % self.duration if self.duration else "直到收到停止信号"),
              flush=True)
        deadline = (time.time() + self.duration) if self.duration > 0 else None
        last_stats = 0.0
        try:
            while not self.stop_requested:
                if deadline is not None and time.time() >= deadline:
                    print("[relay] 到达 --duration 时限，收尾", flush=True)
                    break
                try:
                    data, peer = self.sock.recvfrom(RECV_BUFSIZE)
                except socket.timeout:
                    data, peer = None, None
                except OSError as exc:
                    # 收包错误只记录：单次 recvfrom 失败不该终结中继
                    print("[relay] recvfrom 错误（继续）：%s" % exc, flush=True)
                    data, peer = None, None
                if data:
                    self.packets += 1
                    self.bytes_in += len(data)
                    self.sources.add("%s:%d" % peer)
                    self.last_peer = "%s:%d" % peer
                    try:
                        sent = self.sock.sendto(data, (self.target, self.target_port))
                        self.forwarded += 1
                        self.bytes_out += sent
                    except OSError as exc:
                        self.send_errors += 1
                        if self.send_errors <= 3:
                            print("[relay] 转发到 %s:%d 失败（第 %d 次）：%s"
                                  % (self.target, self.target_port,
                                     self.send_errors, exc), flush=True)
                now = time.time()
                if now - last_stats >= STATS_INTERVAL_S:
                    last_stats = now
                    self.print_progress()
                    self.write_stats()
        finally:
            self.print_progress(final=True)
            self.write_stats()
            try:
                self.sock.close()
            except OSError:
                pass
        return 0

    # ------------------------------------------------------------ 统计
    def snapshot(self) -> dict:
        return {
            "bind": "%s:%d" % (self.bind_host, self.listen_port),
            "target": "%s:%d" % (self.target, self.target_port),
            "packets": self.packets,
            "bytes_in": self.bytes_in,
            "forwarded": self.forwarded,
            "bytes_out": self.bytes_out,
            "send_errors": self.send_errors,
            "sources": sorted(self.sources),
            "last_peer": self.last_peer,
            "elapsed_s": round(time.time() - self.started_at, 1),
        }

    def print_progress(self, final: bool = False) -> None:
        tag = "final" if final else "progress"
        print("[relay:%s] 收包=%d 入字节=%d 转发=%d 出字节=%d 转发失败=%d "
              "最近源=%s 运行=%.0fs"
              % (tag, self.packets, self.bytes_in, self.forwarded,
                 self.bytes_out, self.send_errors, self.last_peer or "-",
                 time.time() - self.started_at), flush=True)

    def write_stats(self) -> None:
        if not self.stats_path:
            return
        try:
            tmp = self.stats_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.snapshot(), f, ensure_ascii=False, indent=2)
            os.replace(tmp, self.stats_path)  # 原子替换：读者不会读到半文件
        except OSError as exc:
            print("[relay] 统计落盘失败（不影响中继）：%s" % exc, flush=True)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="WO Mic 双机联调单向 UDP 中继（app → PC）")
    p.add_argument("--bind", default=DEFAULT_BIND,
                   help="监听地址（默认 0.0.0.0；slirp 把 10.0.2.2 的 UDP 投递到 "
                        "host loopback，绑 0.0.0.0 才能收到）")
    p.add_argument("--listen-port", type=int, default=DEFAULT_LISTEN_PORT,
                   help="监听端口（默认 8126，与 ACL 放行端口、协商媒体端口一致）")
    p.add_argument("--target", required=True,
                   help="转发目标 IP（windows-client 的 tailnet IPv4）")
    p.add_argument("--target-port", type=int, default=DEFAULT_TARGET_PORT,
                   help="转发目标端口（默认 8126，对端 womic.exe --media-port）")
    p.add_argument("--stats", default="",
                   help="统计 JSON 落盘路径（可选；workflow 据此断言流量）")
    p.add_argument("--duration", type=float, default=0.0,
                   help="运行秒数上限（0 = 直到收到 SIGTERM/SIGINT）")
    args = p.parse_args(argv)
    if not (1 <= args.listen_port <= 65535):
        p.error("--listen-port 须在 1..65535")
    if not (1 <= args.target_port <= 65535):
        p.error("--target-port 须在 1..65535")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    relay = UdpRelay(args.bind, args.listen_port, args.target,
                     args.target_port, args.stats, args.duration)
    return relay.run()


if __name__ == "__main__":
    sys.exit(main())
