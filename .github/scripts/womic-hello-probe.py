#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""womic-hello-probe —— WO Mic 协议 101 hello 连通性探针（tailscale-dual 就绪判据）。

为什么需要它：双机联调里 TCP 通路跨了四段
（womic.exe → tailnet → socat → slirp → app），任何一段没就绪就直接跑
womic.exe，只会拿到一句"连接失败"，无法定位。本探针只做协议里最轻的一次
交换——101 hello——并打印原始应答字节，作为"整条 TCP 通路已通"的二元判据：

    期望应答：65 00000002 00 04   （type=101, len=2, status=0, level=4）
    依据：android/verify/integration/pc_client.py C02 断言 / 规格 v1 §2.2/§2.3；
          client/src/engine.c do_handshake() 对 rsp[0]==0 且 rsp[1]==4 的检查。

使用方：
  - job A（linux-app）：探测 10.0.2.15:8125（slirp 直连）或 redir 兜底后的
    127.0.0.1:18125，决定 TCP 中继的后端地址；也用于自检 socat 出口。
  - job B（windows-client）：探测 <A 的 tailnet IP>:8125，确认跨网段整条
    TCP 通路就绪后才启动真 womic.exe（避免把"通路没就绪"误判成客户端缺陷）。

退出码：0 = hello 握手成功；1 = 预算内未成功；2 = 用法/环境错误。
纯标准库；Windows 与 Linux 均可运行。
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

# Windows 上 Python 的 stdout/stderr 默认用本地代码页（GitHub Windows runner 的
# Python 3.12 是 cp1252），打印中文会抛 UnicodeEncodeError 把真正的失败原因吞掉
# （run 40057271618 实证：B 侧 hello 探活本就超时，却只看到编码 traceback）。
# 显式按 UTF-8 输出，写不出的字符用 ? 替换而不是崩溃。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass


MSG_HELLO = 101  # 0x65
# 官方 Windows 客户端 6.3 的 hello 负载（objdump @0x4147be 实证）：
# payload[0]=4 选 UDP 头格式 C，payload[1..5] 手机端跳过不读。
HELLO_PAYLOAD = bytes([4, 6, 3, 0, 0, 0])
EXPECT_STATUS = 0
EXPECT_LEVEL = 4


def encode_frame(msg_type: int, payload: bytes) -> bytes:
    """[type:u8][length:u32 BE][payload]（规格 v1 第 2.1 节）。"""
    return bytes([msg_type]) + struct.pack(">I", len(payload)) + payload


def hexs(data: bytes) -> str:
    return " ".join("%02X" % b for b in data)


def probe_once(host: str, port: int, io_timeout: float) -> tuple[bool, str]:
    """单次尝试：连接 → hello → 读应答。返回 (ok, 诊断串)。"""
    try:
        sock = socket.create_connection((host, port), timeout=io_timeout)
    except OSError as exc:
        return False, "TCP 连接失败：%s" % exc
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(io_timeout)
        sock.sendall(encode_frame(MSG_HELLO, HELLO_PAYLOAD))
        header = b""
        deadline = time.monotonic() + io_timeout
        while len(header) < 5:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, "读取应答头超时（已收 %d 字节）" % len(header)
            sock.settimeout(remaining)
            chunk = sock.recv(5 - len(header))
            if not chunk:
                return False, "对端关闭连接（EOF，已收 %d 字节）" % len(header)
            header += chunk
        (length,) = struct.unpack(">I", header[1:5])
        payload = b""
        while len(payload) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, "读取应答体超时（len=%d，已收 %d）" % (length, len(payload))
            sock.settimeout(remaining)
            chunk = sock.recv(length - len(payload))
            if not chunk:
                return False, "对端关闭连接（EOF，应答体不完整）"
            payload += chunk
        reply = header + payload
        if header[0] != MSG_HELLO:
            return False, "应答 type=%d ≠ 101：%s" % (header[0], hexs(reply))
        if length < 2:
            return False, "应答 len=%d < 2：%s" % (length, hexs(reply))
        if payload[0] != EXPECT_STATUS:
            return False, "应答 status=%d ≠ 0（对端拒绝 hello）：%s" % (payload[0], hexs(reply))
        if payload[1] != EXPECT_LEVEL:
            return False, "应答 level=%d ≠ 4（头格式不是 C）：%s" % (payload[1], hexs(reply))
        return True, "应答 %s（status=0 level=4）" % hexs(reply)
    except (socket.timeout, TimeoutError):
        return False, "IO 超时（%.1fs）" % io_timeout
    except OSError as exc:
        return False, "IO 错误：%s" % exc
    finally:
        try:
            sock.close()
        except OSError:
            pass


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="WO Mic 101 hello 连通性探针")
    p.add_argument("--host", required=True, help="TCP 目标（app 或中继出口地址）")
    p.add_argument("--port", type=int, default=8125, help="TCP 目标端口（默认 8125）")
    p.add_argument("--timeout", type=float, default=30.0,
                   help="总重试预算（秒）；每次尝试内还有 --io-timeout")
    p.add_argument("--interval", type=float, default=2.0, help="重试间隔（秒）")
    p.add_argument("--io-timeout", type=float, default=5.0, help="单次 IO 超时（秒）")
    p.add_argument("--quiet", action="store_true", help="只打最后一行结果")
    args = p.parse_args(argv)
    if not (1 <= args.port <= 65535):
        p.error("--port 须在 1..65535")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    deadline = time.monotonic() + args.timeout
    attempt = 0
    last = ""
    while True:
        attempt += 1
        ok, detail = probe_once(args.host, args.port, args.io_timeout)
        if ok:
            print("[probe] PASS %s:%d 第 %d 次尝试：%s"
                  % (args.host, args.port, attempt, detail), flush=True)
            return 0
        last = detail
        if not args.quiet:
            print("[probe] 第 %d 次尝试失败（%s:%d）：%s"
                  % (attempt, args.host, args.port, detail), flush=True)
        if time.monotonic() >= deadline:
            break
        time.sleep(args.interval)
    print("[probe] FAIL %.0fs 预算内 %d 次尝试均失败（%s:%d），最后错误：%s"
          % (args.timeout, attempt, args.host, args.port, last), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
