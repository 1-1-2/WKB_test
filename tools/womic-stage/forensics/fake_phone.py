#!/usr/bin/env python3
"""假 WO Mic 手机端（TCP 8125 server + UDP 媒体发送方）。

按 docs/protocol/womic-wifi-protocol-v1.md 实现：
  TCP 帧格式 [type:u8][len:u32BE][payload]
  101 -> 回 `65 00000002 00 04`
  102 -> 记录 mediaPort，回 `66 00000001 00`
  103 -> 回 `67 00000001 00`，启动 UDP 推流（头格式 C，音量 5）
  105 -> 心跳置位（无应答），5s 看门狗
  104 -> 停推流
日志全量落 D:/a/_temp/fake_phone.log。
"""
import json
import socket
import struct
import sys
import threading
import time

LOG = r"D:\a\_temp\fake_phone.log"
UDP_PAYLOAD_SIZES = [80, 96, 64, 112, 88]   # 模拟 Opus 20ms 包大小分布


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


class PhoneServer:
    def __init__(self):
        self.media_port = None
        self.streaming = False
        self.udp_target = None
        self.last_heartbeat = time.time()
        self.lock = threading.Lock()

    def watchdog(self):
        while True:
            time.sleep(1)
            with self.lock:
                if self.streaming and time.time() - self.last_heartbeat > 5:
                    log("WATCHDOG: 5s 无心跳，断流")
                    self.streaming = False

    def udp_sender(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        seq = 0
        t0 = time.time()
        i = 0
        while self.streaming:
            size = UDP_PAYLOAD_SIZES[i % len(UDP_PAYLOAD_SIZES)]
            i += 1
            payload = bytes((seq * 7 + j) % 251 for j in range(size))  # 伪随机"Opus"
            hdr = struct.pack(">bbHHIB", 4, 0, size + 7, seq & 0xFFFF,
                              int((time.time() - t0) * 1000) & 0xFFFFFFFF, 5)
            pkt = hdr + payload
            try:
                s.sendto(pkt, self.udp_target)
            except OSError as e:
                log(f"UDP send error: {e}")
                break
            seq += 1
            time.sleep(0.02)
        log(f"UDP 推流结束，共 {seq} 包")

    def handle(self, conn):
        buf = b""
        conn.settimeout(30)
        log("TCP 已连接")
        while True:
            try:
                data = conn.recv(4096)
            except socket.timeout:
                log("TCP recv 超时")
                return
            if not data:
                log("TCP 对端关闭")
                return
            buf += data
            log(f"TCP 收 {len(data)}B: {data.hex()}")
            while len(buf) >= 5:
                mtype = buf[0]
                (mlen,) = struct.unpack(">I", buf[1:5])
                if len(buf) < 5 + mlen:
                    break
                payload = buf[5:5 + mlen]
                buf = buf[5 + mlen:]
                if mtype == 101:
                    log(f"101 hello: {payload.hex()} -> 应答 00 04")
                    conn.sendall(struct.pack(">bI", 101, 2) + b"\x00\x04")
                elif mtype == 102:
                    (port,) = struct.unpack(">I", payload[2:6])
                    log(f"102 参数: codec={payload[0]} rateIdx={payload[1]} "
                        f"mediaPort={port} -> 应答 00")
                    with self.lock:
                        self.media_port = port
                    conn.sendall(struct.pack(">bI", 102, 1) + b"\x00")
                elif mtype == 103:
                    log(f"103 开始推流 -> 应答 00, UDP -> 127.0.0.1:{self.media_port}")
                    conn.sendall(struct.pack(">bI", 103, 1) + b"\x00")
                    with self.lock:
                        self.streaming = True
                        self.udp_target = ("127.0.0.1", self.media_port or 8126)
                    threading.Thread(target=self.udp_sender, daemon=True).start()
                elif mtype == 104:
                    log("104 停止推流 -> 应答（官方怪癖：写 type 102）")
                    conn.sendall(struct.pack(">bI", 102, 1) + b"\x00")
                    with self.lock:
                        self.streaming = False
                elif mtype == 105:
                    with self.lock:
                        self.last_heartbeat = time.time()
                else:
                    log(f"未知消息 type={mtype} len={mlen}")


def main():
    ps = PhoneServer()
    threading.Thread(target=ps.watchdog, daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 8125))
    srv.listen(2)
    log("假手机端监听 127.0.0.1:8125")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=ps.handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
