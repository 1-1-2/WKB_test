"""网络链路端到端验证：TCP 8125 控制握手 + UDP 媒体（头格式 C）。

背景：驱动侧（灌音协议）已反汇编还原并实测 PASS；
但 TCP/UDP 这一层此前**从未跑通过一次完整交换**——协议规格只有文档证据。
本脚本在同一进程内同时扮演两端：

  手机端线程 = TCP server(8125) + UDP sender（按协议 v1 实现）
  PC 端      = TCP client + UDP receiver（按协议 v1 解析头格式 C）

验证的不是"能不能和官方客户端互通"，而是"按文档实现的两端能否完成
101→102→103 握手、按 20ms 节奏收到连续 UDP 帧、头格式 C 可被正确解析"。
Opus 负载用填充字节代替（解码属客户端开发范畴，本轮不做）。
"""

import json
import socket
import struct
import sys
import threading
import time

CTL_PORT = 8125
MEDIA_PORT = 8126
FRAME_MS = 20
OPUS_LEN = 100          # 48k/20ms Opus 典型帧长量级（本轮填充，不代表真实分布）
RUN_SECONDS = 3.0

log_lines: list = []
phone_log: list = []


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    log_lines.append(line)
    print(line, flush=True)


def send_frame(sock, typ, payload=b""):
    sock.sendall(bytes([typ]) + struct.pack(">I", len(payload)) + payload)


def recv_frame(sock, timeout=8.0):
    sock.settimeout(timeout)
    hdr = b""
    while len(hdr) < 5:
        chunk = sock.recv(5 - len(hdr))
        if not chunk:
            return None
        hdr += chunk
    typ = hdr[0]
    (ln,) = struct.unpack(">I", hdr[1:5])
    payload = b""
    while len(payload) < ln:
        chunk = sock.recv(ln - len(payload))
        if not chunk:
            break
        payload += chunk
    return typ, payload


# ------------------------------------------------------------------ 手机端
def udp_sender(pc_port, stop_evt, stats):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("127.0.0.1", pc_port))
    except OSError as e:
        phone_log.append(f"udp connect failed: {e}")
        return
    seq = 0
    t0 = time.monotonic()
    payload = bytes(range(OPUS_LEN))  # 填充：本轮不关心音频内容
    while not stop_evt.is_set():
        ts = int((time.monotonic() - t0) * 1000)
        pkt = (b"\x04\x00"
               + struct.pack(">H", OPUS_LEN + 7)
               + struct.pack(">H", seq & 0xFFFF)
               + struct.pack(">I", ts)
               + bytes([5])
               + payload)
        try:
            s.send(pkt)
        except OSError as e:
            phone_log.append(f"udp send err: {e}")
            break
        stats["sent"] = stats.get("sent", 0) + 1
        seq += 1
        time.sleep(FRAME_MS / 1000.0)
    s.close()
    phone_log.append(f"udp sender stopped after {stats.get('sent', 0)} packets")


def phone_server(stop_evt, ready_evt, stats):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", CTL_PORT))
    srv.listen(1)
    srv.settimeout(20)
    ready_evt.set()
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        phone_log.append("phone: no client connected (timeout)")
        return
    phone_log.append(f"phone: accepted {addr}")
    udp_th = None
    try:
        while True:
            f = recv_frame(conn, 12)
            if f is None:
                phone_log.append("phone: peer closed")
                break
            typ, p = f
            phone_log.append(f"phone: <- type={typ} len={len(p)} payload={p.hex()}")
            if typ == 101:
                # payload[0]=variant，必须 >=2（4 = 头格式 C）
                variant = p[0] if p else 0
                if variant < 2:
                    send_frame(conn, 101, bytes([10, 4]))   # status=10: variant<2
                    phone_log.append("phone: -> 101 status=10 (variant illegal)")
                else:
                    send_frame(conn, 101, bytes([0, 4]))    # status=0 成功
            elif typ == 102:
                if len(p) < 6:
                    send_frame(conn, 102, bytes([12]))
                    continue
                codec, rate_idx = p[0], p[1]
                (port,) = struct.unpack(">I", p[2:6])
                if codec != 2:
                    send_frame(conn, 102, bytes([12]))      # codec 非法
                elif rate_idx != 2:
                    send_frame(conn, 102, bytes([13]))     # rateIdx 非法
                else:
                    send_frame(conn, 102, bytes([0]))
                    phone_log.append(f"phone: media port={port} codec={codec} rateIdx={rate_idx}")
            elif typ == 103:
                send_frame(conn, 103, bytes([0]))
                udp_th = threading.Thread(target=udp_sender,
                                          args=(port, stop_evt, stats), daemon=True)
                udp_th.start()
                phone_log.append("phone: udp sender started")
            elif typ == 104:
                stop_evt.set()
                # 官方 app 怪癖：104 的应答写成 type 102
                send_frame(conn, 102, bytes([0]))
                phone_log.append("phone: -> 104 answered with type 102 (official quirk)")
                break
            elif typ == 105:
                stats["heartbeats"] = stats.get("heartbeats", 0) + 1
                # 心跳无应答
            else:
                send_frame(conn, 0, bytes([1]) + b"Unknown req")
    except Exception as e:  # noqa: BLE001
        phone_log.append(f"phone: exception {type(e).__name__}: {e}")
    finally:
        stop_evt.set()
        try:
            conn.close()
        except Exception:  # noqa: BLE001
            pass
        srv.close()


# ------------------------------------------------------------------ PC 端
def pc_side(stats):
    results = {}
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(("127.0.0.1", CTL_PORT))
    log(f"pc: connected to 127.0.0.1:{CTL_PORT}")

    # 1) 101 hello：variant=4 → 头格式 C
    send_frame(s, 101, bytes([4, 6, 3, 0, 0, 0]))
    r = recv_frame(s, 5)
    results["101_reply"] = {"type": r[0] if r else None,
                            "payload": r[1].hex() if r else None,
                            "status_ok": bool(r and r[1] and r[1][0] == 0 and r[1][1] == 4)}
    log(f"pc: 101 reply -> {results['101_reply']}")

    # 2) 102 参数：codec=2(Opus) rateIdx=2(48000) mediaPort=8126
    send_frame(s, 102, bytes([2, 2]) + struct.pack(">I", MEDIA_PORT))
    r = recv_frame(s, 5)
    results["102_reply"] = {"type": r[0] if r else None,
                            "payload": r[1].hex() if r else None,
                            "status_ok": bool(r and r[1] and r[1][0] == 0)}
    log(f"pc: 102 reply -> {results['102_reply']}")

    # 3) 103 开始推流
    send_frame(s, 103, b"")
    r = recv_frame(s, 5)
    results["103_reply"] = {"type": r[0] if r else None,
                            "payload": r[1].hex() if r else None,
                            "status_ok": bool(r and r[1] and r[1][0] == 0)}
    log(f"pc: 103 reply -> {results['103_reply']}")

    # 4) 收 UDP（头格式 C）
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    us.bind(("0.0.0.0", MEDIA_PORT))
    us.settimeout(1.0)
    pkts = []
    t_end = time.monotonic() + RUN_SECONDS
    while time.monotonic() < t_end:
        try:
            data, _ = us.recvfrom(4096)
        except socket.timeout:
            continue
        if len(data) < 11 or data[0:2] != b"\x04\x00":
            pkts.append({"bad": True, "len": len(data), "head": data[:8].hex()})
            continue
        (ln,) = struct.unpack(">H", data[2:4])
        (seq,) = struct.unpack(">H", data[4:6])
        (ts,) = struct.unpack(">I", data[6:10])
        vol = data[10]
        payload_len = len(data) - 11
        pkts.append({"len": ln, "seq": seq, "ts": ts, "vol": vol,
                     "payload_len": payload_len,
                     "len_consistent": ln == payload_len + 7})
    us.close()

    seqs = [p["seq"] for p in pkts if "seq" in p]
    tss = [p["ts"] for p in pkts if "ts" in p]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) & 0xFFFF != 1) if seqs else -1
    results["udp"] = {
        "packets": len(pkts),
        "bad_packets": sum(1 for p in pkts if p.get("bad")),
        "seq_gaps": gaps,
        "seq_first": seqs[0] if seqs else None,
        "seq_last": seqs[-1] if seqs else None,
        "ts_span_ms": (tss[-1] - tss[0]) if len(tss) > 1 else 0,
        "vol_values": sorted({p["vol"] for p in pkts if "vol" in p}),
        "len_consistent_all": all(p.get("len_consistent") for p in pkts if "seq" in p),
        "expected_packets": int(RUN_SECONDS * 1000 / FRAME_MS),
    }
    log(f"pc: udp -> {results['udp']}")

    # 5) 心跳
    send_frame(s, 105, b"")
    time.sleep(0.3)

    # 6) 104 停止（官方 app 应答写成 type 102）
    send_frame(s, 104, b"")
    r = recv_frame(s, 5)
    results["104_reply"] = {"type": r[0] if r else None,
                            "payload": r[1].hex() if r else None,
                            "type_is_102_as_documented": bool(r and r[0] == 102)}
    log(f"pc: 104 reply -> {results['104_reply']}")

    try:
        s.close()
    except Exception:  # noqa: BLE001
        pass
    return results


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    stats = {}
    stop_evt = threading.Event()
    ready_evt = threading.Event()
    ph = threading.Thread(target=phone_server, args=(stop_evt, ready_evt, stats), daemon=True)
    ph.start()
    ready_evt.wait(5)
    time.sleep(0.3)

    results = pc_side(stats)
    stop_evt.set()
    time.sleep(0.5)

    results["phone_log"] = phone_log
    results["phone_stats"] = stats
    print("\n===== NETLINK_JSON =====")
    print(json.dumps(results, ensure_ascii=False, indent=1))
    print("\n===== 判读 =====")
    ok = (results["101_reply"]["status_ok"]
          and results["102_reply"]["status_ok"]
          and results["103_reply"]["status_ok"]
          and results["udp"]["packets"] > 50
          and results["udp"]["bad_packets"] == 0
          and results["udp"]["len_consistent_all"])
    print(f"RESULT={'PASS' if ok else 'FAIL'}")
    print(f"握手 101/102/103: "
          f"{results['101_reply']['status_ok']}/"
          f"{results['102_reply']['status_ok']}/"
          f"{results['103_reply']['status_ok']}")
    print(f"UDP: {results['udp']['packets']} 包 / "
          f"期望 {results['udp']['expected_packets']} / "
          f"序号断点 {results['udp']['seq_gaps']} / "
          f"时间跨度 {results['udp']['ts_span_ms']}ms")
    print(f"104 应答写成 type 102（官方怪癖）: {results['104_reply']['type_is_102_as_documented']}")


if __name__ == "__main__":
    main()
