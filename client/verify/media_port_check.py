#!/usr/bin/env python3
"""--media-port 端到端自测：真 womic.exe ↔ 模拟 app（TCP 控制面 + UDP 媒体面）。

为什么要有这个脚本
------------------
真客户端与真 app 的联调需要一台 Android 真机（app 当 TCP server、监听 8125）。
在没有真机的环境里，能拿到的最高档证据是：**真实的 womic.exe 与一个严格按
协议实现的模拟 app 跨网络跑通完整握手与推流**。本脚本就是那个模拟 app +
断言集合，可以在沙箱（Linux + wine）、CI 的 Linux runner、以及真 Windows 上
原样运行。

判据（每一项都是硬断言，任一失败即退出码非 0）
----------------------------------------------
  1. 控制面 101 → 102 → 103 → 105 全部到达（缺一即失败）；
  2. **102 里上报的媒体端口 == --media-port 指定的值**（本次改动的直接判据）；
  3. 模拟 app 往 <对端IP>:<媒体端口> 发 UDP，womic.exe 收到并解码
     （OK 行里的 帧 > 0 且 字节 > 0）；
  4. 不指定 --media-port 时端口仍是随机的，且不等于 102 里那个固定值
     （向后兼容：GUI 与无参调用行为不变）；
  5. 端口被占用时 womic.exe 退出码非 0，且错误串里带 WSA 错误码；
  6. 非法取值（0 / 负数 / >65535 / 非数字）退出码 1，且给出明确报错。

用法
----
  python3 client/verify/media_port_check.py                 # 全跑（默认 wine）
  python3 client/verify/media_port_check.py --case fixed    # 只跑固定端口
  python3 client/verify/media_port_check.py --exe dist/womic.exe --no-wine   # 真 Windows
  python3 client/verify/media_port_check.py --control-port 9125 --media-port 5004

产物：退出码 0 = 全部通过；1 = 有断言失败；2 = 环境/用法错误。
stdout 同时打出 [PASS]/[FAIL]/[INFO] 行，可直接贴进 CI 日志。
"""
from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

RATE = {0: 8000, 1: 16000, 2: 48000}
FAILURES: list[str] = []


def log(tag: str, *a):
    print(f"[{tag}]", *a, flush=True)


def check(ok: bool, name: str, detail: str = "") -> bool:
    if ok:
        log("PASS", name, detail)
    else:
        log("FAIL", name, detail)
        FAILURES.append(name)
    return ok


# ---------------------------------------------------------------- 模拟 app
class FakeApp:
    """按 docs/protocol/womic-wifi-protocol-v1.md 实现的服务端（WO Mic 手机侧）。

    TCP 控制面：101 → 102 → 103 → 105(心跳) → 104；UDP 媒体面：格式 C。
    """

    def __init__(self, host: str, ctrl_port: int, tone_hz: float, amp: int,
                 seconds: float):
        self.host = host
        self.ctrl_port = ctrl_port
        self.tone_hz = tone_hz
        self.amp = amp
        self.seconds = seconds
        self.seen: set[int] = set()          # 收到过的控制消息类型
        self.media_port = -1                 # 102 里上报的媒体端口
        self.rate_idx = -1
        self.codec = -1
        self.stop = threading.Event()
        self.srv: socket.socket | None = None
        self.thread: threading.Thread | None = None
        self.error = ""

    # -- 控制面 ------------------------------------------------------------
    @staticmethod
    def _read_frame(conn: socket.socket):
        head = b""
        while len(head) < 5:
            chunk = conn.recv(5 - len(head))
            if not chunk:
                return None, None
            head += chunk
        (length,) = struct.unpack(">I", head[1:5])
        payload = b""
        while len(payload) < length:
            chunk = conn.recv(length - len(payload))
            if not chunk:
                return None, None
            payload += chunk
        return head[0], payload

    @staticmethod
    def _send_frame(conn: socket.socket, mtype: int, payload: bytes):
        conn.sendall(bytes([mtype]) + struct.pack(">I", len(payload)) + payload)

    def _udp_pump(self, peer_ip: str, port: int, rate: int):
        """按 20 ms 节奏下发格式 C 帧。有 opuslib 就编真实 1 kHz 正弦。"""
        enc = None
        if self.tone_hz > 0:
            try:
                import opuslib
                enc = opuslib.Encoder(rate, 1, "audio")
            except Exception as exc:      # 没有 opuslib 就退化成随机负载
                log("INFO", "opuslib 不可用，改用随机负载：", exc)
        frame = rate // 50
        phase = 0
        seq = 0
        t0 = time.time()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        next_t = t0
        log("INFO", f"UDP 下发开始 → {peer_ip}:{port} @ {rate}Hz")
        while not self.stop.is_set():
            if enc is not None:
                buf = bytearray()
                for i in range(frame):
                    v = int(self.amp * math.sin(2 * math.pi * self.tone_hz
                                                * (phase + i) / rate))
                    buf += struct.pack("<h", max(-32767, min(32767, v)))
                phase = (phase + frame) % rate
                payload = enc.encode(bytes(buf), frame)
            else:
                payload = bytes((seq * 37 + i) & 0xFF for i in range(180))
            ts = int((time.time() - t0) * 1000) & 0xFFFFFFFF
            body = struct.pack(">HIB", seq & 0xFFFF, ts, 5) + payload
            pkt = b"\x04\x00" + struct.pack(">H", len(body)) + body
            try:
                sock.sendto(pkt, (peer_ip, port))
            except OSError:
                pass
            seq += 1
            next_t += 0.020
            sleep = next_t - time.time()
            if sleep > 0:
                time.sleep(sleep)
        sock.close()

    def _serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.ctrl_port))
        srv.listen(1)
        srv.settimeout(self.seconds)
        self.srv = srv
        try:
            conn, peer = srv.accept()
        except socket.timeout:
            self.error = "等客户端接入超时"
            return
        log("INFO", f"客户端接入 {peer}")
        conn.settimeout(self.seconds)
        pump: threading.Thread | None = None
        try:
            while not self.stop.is_set():
                mtype, payload = self._read_frame(conn)
                if mtype is None:
                    break
                self.seen.add(mtype)
                if mtype == 101:
                    self._send_frame(conn, 101, bytes([0, 4]))
                elif mtype == 102:
                    self.codec, self.rate_idx = payload[0], payload[1]
                    self.media_port = struct.unpack(">I", payload[2:6])[0]
                    log("INFO", f"102 codec={self.codec} rate_idx={self.rate_idx} "
                                f"media_port={self.media_port}")
                    self._send_frame(conn, 102, bytes([0]))
                    pump = threading.Thread(
                        target=self._udp_pump,
                        args=(peer[0], self.media_port, RATE.get(self.rate_idx, 48000)),
                        daemon=True)
                    pump.start()
                elif mtype == 103:
                    self._send_frame(conn, 103, bytes([0]))
                elif mtype == 104:
                    self.stop.set()
                    self._send_frame(conn, 102, bytes([0]))
                elif mtype == 105:
                    pass                       # 心跳无应答
                else:
                    self._send_frame(conn, 0, b"\x01Unknown req")
        except (socket.timeout, ConnectionResetError, OSError) as exc:
            log("INFO", f"控制连接结束：{exc}")
        finally:
            self.stop.set()
            conn.close()
            srv.close()

    def start(self):
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        time.sleep(0.2)

    def join(self, timeout: float = 5.0):
        if self.thread:
            self.thread.join(timeout)


# ---------------------------------------------------------------- 执行 exe
def run_exe(exe: str, args: list[str], use_wine: bool, timeout: float):
    cmd = (["wine"] if use_wine else []) + [exe] + args
    log("INFO", "执行:", " ".join(cmd))
    env = dict(os.environ)
    env["WINEDEBUG"] = env.get("WINEDEBUG", "-all")
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT", ""
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    return p.returncode, out, err


OK_RE = re.compile(
    r"OK\s+帧=(\d+)\s+字节=(\d+).*?媒体端口=(\d+)\((\S+)\)")


def parse_ok(stdout: str):
    """返回 (frames, bytes, media_port, mode)；解析不到返回 None。"""
    m = OK_RE.search(stdout)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)


# ---------------------------------------------------------------- 用例
def case_fixed(cfg) -> None:
    """固定媒体端口：握手完整 + 102 上报端口一致 + UDP 真的收到。"""
    log("CASE", f"A. --media-port {cfg.media_port}（固定端口全链路）")
    app = FakeApp(cfg.host, cfg.control_port, cfg.tone, cfg.amp, cfg.seconds)
    app.start()
    rc, out, err = run_exe(
        cfg.exe,
        ["--host", cfg.host, "--port", str(cfg.control_port),
         "--media-port", str(cfg.media_port), "--connect", "--no-gui"],
        cfg.wine, cfg.timeout)
    app.stop.set()
    app.join()
    if err.strip():
        log("INFO", "stderr:", err.strip()[:400])
    log("INFO", "womic.exe 输出:", out.strip().replace("\n", " | ")[:400])

    check(rc == 0, "A1 exe 退出码为 0", f"rc={rc}")
    for t, name in ((101, "101 hello"), (102, "102 音频参数"),
                    (103, "103 开始"), (105, "105 心跳")):
        check(t in app.seen, f"A2 控制面收到 {name}", f"seen={sorted(app.seen)}")
    check(app.media_port == cfg.media_port,
          "A3 102 上报的媒体端口等于 --media-port",
          f"上报={app.media_port} 期望={cfg.media_port}")
    parsed = parse_ok(out)
    check(parsed is not None, "A4 exe 打印 OK 行（含媒体端口）", out.strip()[:200])
    if parsed:
        frames, nbytes, mport, mode = parsed
        check(mport == cfg.media_port and mode == "固定",
              "A5 OK 行标注媒体端口为固定值", f"media_port={mport} mode={mode}")
        check(frames > 0, "A6 收到 UDP 帧（帧数 > 0）", f"帧={frames} 字节={nbytes}")
        check(nbytes > 0, "A7 收到 UDP 字节（字节数 > 0）", f"字节={nbytes}")


def case_random(cfg) -> None:
    """不指定 --media-port：仍是随机端口，且不等于上一个固定值（向后兼容）。"""
    log("CASE", "B. 不带 --media-port（默认随机，行为不得变）")
    app = FakeApp(cfg.host, cfg.control_port, cfg.tone, cfg.amp, cfg.seconds)
    app.start()
    rc, out, err = run_exe(
        cfg.exe,
        ["--host", cfg.host, "--port", str(cfg.control_port),
         "--connect", "--no-gui"],
        cfg.wine, cfg.timeout)
    app.stop.set()
    app.join()
    log("INFO", "womic.exe 输出:", out.strip().replace("\n", " | ")[:400])
    check(rc == 0, "B1 exe 退出码为 0", f"rc={rc}")
    check(1 <= app.media_port <= 65535, "B2 102 上报了合法端口",
          f"media_port={app.media_port}")
    parsed = parse_ok(out)
    if parsed:
        frames, nbytes, mport, mode = parsed
        check(mode == "随机", "B3 OK 行标注为随机端口", f"mode={mode}")
        check(mport == app.media_port, "B4 OK 行端口与 102 一致",
              f"ok={mport} 102={app.media_port}")
        check(frames > 0, "B5 随机端口下也能收到 UDP 帧", f"帧={frames}")
    else:
        check(False, "B3 exe 未打印 OK 行", out.strip()[:200])


def case_busy(cfg) -> None:
    """端口被占用：必须带 WSA 错误码报错，退出码非 0，且不静默回退。"""
    log("CASE", f"C. --media-port {cfg.media_port} 但端口已被占用")
    squatter = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        squatter.bind((cfg.host, cfg.media_port))
    except OSError as exc:
        log("INFO", f"无法占用 {cfg.media_port}（{exc}），跳过本用例")
        return
    try:
        rc, out, err = run_exe(
            cfg.exe,
            ["--host", cfg.host, "--port", str(cfg.control_port),
             "--media-port", str(cfg.media_port), "--connect", "--no-gui"],
            cfg.wine, cfg.timeout)
        allout = out + err
        log("INFO", "womic.exe 输出:", allout.strip().replace("\n", " | ")[:400])
        check(rc != 0, "C1 端口占用时退出码非 0", f"rc={rc}")
        check("WSA=" in allout, "C2 错误串里带 WSA 错误码",
              allout.strip()[:200])
        check("10048" in allout or "48" in allout,
              "C3 WSA 错误码指向端口被占用(10048)", allout.strip()[:200])
        check("媒体端口" in allout or "FAIL" in allout,
              "C4 明确点出是媒体端口绑定失败", allout.strip()[:200])
        ok_line = parse_ok(out)
        check(ok_line is None, "C5 不静默回退到随机端口后成功", out.strip()[:200])
    finally:
        squatter.close()


def case_invalid(cfg) -> None:
    """非法取值：退出码 1 + 明确报错（不是崩溃、不是静默回退）。"""
    log("CASE", "D. --media-port 非法取值")
    for bad in ("0", "-5", "65536", "70000", "abc", "5x"):
        rc, out, err = run_exe(cfg.exe, ["--media-port", bad], cfg.wine, 30)
        first = (out + err).strip().splitlines()
        head = first[0] if first else ""
        check(rc == 1, f"D[{bad}] 退出码为 1（参数错误）", f"rc={rc}")
        check("参数错误" in head and "未知参数" not in head,
              f"D[{bad}] 给出针对 --media-port 的明确报错", head[:160])


def case_real(cfg) -> None:
    """--no-fake：对端是**真 app**（模拟器/真机），本脚本只负责拉起 exe 并断言。

    CI 拓扑（ubuntu runner + 模拟器，见 android/verify/integration/README.md）：
      wine womic.exe --host 10.0.2.15 --port 8125 --media-port 5004
      app 的 TCP 对端 = 10.0.2.2 → UDP 发往 10.0.2.2:5004 → 落到 host loopback
    """
    log("CASE", f"R. 真 app 模式（--no-fake，对端 {cfg.host}:{cfg.control_port}）")
    rc, out, err = run_exe(
        cfg.exe,
        ["--host", cfg.host, "--port", str(cfg.control_port),
         "--media-port", str(cfg.media_port), "--connect", "--no-gui"],
        cfg.wine, cfg.timeout)
    if err.strip():
        log("INFO", "stderr:", err.strip()[:400])
    log("INFO", "womic.exe 输出:", out.strip().replace("\n", " | ")[:400])
    check(rc == 0, "R1 exe 退出码为 0", f"rc={rc}")
    parsed = parse_ok(out)
    check(parsed is not None, "R2 exe 打印 OK 行", out.strip()[:200])
    if parsed:
        frames, nbytes, mport, mode = parsed
        check(mport == cfg.media_port and mode == "固定",
              "R3 OK 行媒体端口为固定值", f"media_port={mport} mode={mode}")
        check(frames > 0, "R4 收到 UDP 帧", f"帧={frames} 字节={nbytes}")


# ---------------------------------------------------------------- 入口
def build_cfg(a) -> argparse.Namespace:
    exe = a.exe
    if not exe:
        here = os.path.dirname(os.path.abspath(__file__))
        exe = os.path.join(os.path.dirname(here), "build", "womic.exe")
    exe = os.path.abspath(exe)
    if not os.path.isfile(exe):
        sys.exit(f"[ERROR] 找不到 {exe}（先用 client/build.sh 编译，或用 --exe 指定）")
    wine = a.wine
    if wine is None:
        wine = (os.name != "nt") and bool(shutil.which("wine"))
    cfg = argparse.Namespace(
        exe=exe, wine=wine, host=a.host, control_port=a.control_port,
        media_port=a.media_port, tone=a.tone, amp=a.amp,
        seconds=a.seconds, timeout=a.timeout,
    )
    log("INFO", f"exe={exe} wine={wine} 控制端口={cfg.control_port} "
                f"媒体端口={cfg.media_port}")
    return cfg


def main() -> int:
    ap = argparse.ArgumentParser(description="--media-port 端到端自测")
    ap.add_argument("--exe", default="", help="womic.exe 路径（默认 ../build/womic.exe）")
    ap.add_argument("--wine", dest="wine", action="store_true", default=None)
    ap.add_argument("--no-wine", dest="wine", action="store_false")
    ap.add_argument("--host", default="127.0.0.1", help="模拟 app 监听地址")
    ap.add_argument("--control-port", type=int, default=8125, help="TCP 控制端口")
    ap.add_argument("--media-port", type=int, default=5004, help="期望的 UDP 媒体端口")
    ap.add_argument("--tone", type=float, default=1000.0,
                    help=">0 时下发该频率的真实 Opus 正弦（需 opuslib）")
    ap.add_argument("--amp", type=int, default=12000)
    ap.add_argument("--seconds", type=float, default=25.0, help="模拟 app 存活上限")
    ap.add_argument("--timeout", type=float, default=40.0, help="单次 exe 调用超时")
    ap.add_argument("--no-fake", action="store_true",
                    help="不启动模拟 app：对端是真 app（模拟器/真机），只跑 exe 并断言")
    ap.add_argument("--case", default="all",
                    choices=["all", "fixed", "random", "busy", "invalid", "real"])
    a = ap.parse_args()

    cfg = build_cfg(a)
    # "real"（--no-fake）不在 all 里：它要求对端已经有真 app 在听，
    # 默认套件里没有，放进去只会假失败。单独用 --case real 或 --no-fake 跑。
    cases = {
        "fixed": case_fixed, "random": case_random,
        "busy": case_busy, "invalid": case_invalid,
    }
    todo = cases.values() if a.case == "all" else [cases[a.case]]
    if a.no_fake:
        todo = [case_real]
    for fn in todo:
        fn(cfg)

    print("")
    if FAILURES:
        log("RESULT", f"FAIL（{len(FAILURES)} 项）: " + "; ".join(FAILURES))
        return 1
    log("RESULT", "PASS  全部断言通过（真 womic.exe ↔ 模拟 app 全链路）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
