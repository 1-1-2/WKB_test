#!/usr/bin/env python3
"""WO Mic 服务端参考实现（用于客户端联调与 CI，不依赖手机）。

按 docs/protocol/womic-wifi-protocol-v1.md 实现：
  TCP 8125：101 → 102 → 103 → 105(心跳) → 104
  UDP：格式 C  [04 00][len:u16BE][seq:u16BE][ts:u32BE][vol:u8][payload]

payload 用随机长度字节模拟 Opus 包（客户端未装 opus.dll 时以包长估算电平，
装了则以真实解码结果为准）。这样即使没有真实麦克风，也能把整条链路跑通。
"""
from __future__ import annotations

import argparse
import random
import socket
import struct
import sys
import threading
import time

RATE = {0: 8000, 1: 16000, 2: 48000}


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def read_frame(conn: socket.socket):
    head = b""
    while len(head) < 5:
        chunk = conn.recv(5 - len(head))
        if not chunk:
            return None, None
        head += chunk
    mtype = head[0]
    (length,) = struct.unpack(">I", head[1:5])
    payload = b""
    while len(payload) < length:
        chunk = conn.recv(length - len(payload))
        if not chunk:
            return None, None
        payload += chunk
    return mtype, payload


def send_frame(conn: socket.socket, mtype: int, payload: bytes):
    conn.sendall(bytes([mtype]) + struct.pack(">I", len(payload)) + payload)


class UdpPump(threading.Thread):
    """按 20ms 节奏向 PC 下发格式 C 帧。"""

    def __init__(self, host: str, port: int):
        super().__init__(daemon=True)
        self.addr = (host, port)
        self.stop = threading.Event()
        self.seq = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def run(self):
        t0 = time.time()
        next_t = t0
        while not self.stop.is_set():
            # 模拟语音活动：包长在 60~260 之间起伏，形成可见电平变化
            phase = int((time.time() - t0) * 2) % 10
            base = 90 + (abs(5 - phase) * 22)
            paylen = max(40, min(400, base + random.randint(-25, 25)))
            payload = bytes(random.getrandbits(8) for _ in range(paylen))
            ts = int((time.time() - t0) * 1000) & 0xFFFFFFFF
            body = struct.pack(">HIB", self.seq & 0xFFFF, ts, 5) + payload  # seq,ts,vol
            pkt = b"\x04\x00" + struct.pack(">H", len(body)) + body
            try:
                self.sock.sendto(pkt, self.addr)
            except OSError:
                pass
            self.seq += 1
            next_t += 0.020
            sleep = next_t - time.time()
            if sleep > 0:
                time.sleep(sleep)


def serve(host: str, port: int, once: bool = True):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    log(f"监听 {host}:{port}")

    while True:
        conn, peer = srv.accept()
        log(f"客户端接入 {peer}")
        pump = None
        try:
            conn.settimeout(30)
            while True:
                mtype, payload = read_frame(conn)
                if mtype is None:
                    log("控制连接断开")
                    break
                if mtype == 101:
                    variant = payload[0] if payload else 0
                    log(f"101 hello variant={variant} raw={payload.hex()}")
                    if variant < 2:
                        send_frame(conn, 101, bytes([10, variant]))
                        continue
                    send_frame(conn, 101, bytes([0, 4]))
                elif mtype == 102:
                    codec, rate_idx = payload[0], payload[1]
                    media_port = struct.unpack(">I", payload[2:6])[0]
                    log(f"102 codec={codec} rate_idx={rate_idx} media_port={media_port}")
                    if codec not in (2, 100):
                        send_frame(conn, 102, bytes([12]))
                        continue
                    if rate_idx not in RATE:
                        send_frame(conn, 102, bytes([13]))
                        continue
                    send_frame(conn, 102, bytes([0]))
                    pump = UdpPump(peer[0], media_port)
                    pump.start()
                    log(f"UDP 下发开始 → {peer[0]}:{media_port} @ {RATE[rate_idx]}Hz")
                elif mtype == 103:
                    log("103 开始推流")
                    send_frame(conn, 103, bytes([0]))
                elif mtype == 104:
                    log("104 停止推流")
                    if pump:
                        pump.stop.set()
                    send_frame(conn, 102, bytes([0]))   # 官方怪癖：104 应答写成 type 102
                elif mtype == 105:
                    pass                                # 心跳无应答
                else:
                    log(f"未知消息 type={mtype}")
                    send_frame(conn, 0, b"\x01Unknown req")
        except (socket.timeout, ConnectionResetError) as e:
            log(f"连接异常: {e}")
        finally:
            if pump:
                pump.stop.set()
            conn.close()
            log("会话结束")
        if once:
            break


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8125)
    ap.add_argument("--loop", action="store_true", help="接受多轮连接")
    a = ap.parse_args()
    try:
        serve(a.host, a.port, once=not a.loop)
    except KeyboardInterrupt:
        sys.exit(0)
