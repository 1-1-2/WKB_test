#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_client.py —— WO Mic WiFi 协议联调脚本（扮演官方 Windows 客户端 6.3 的 PC 角色）

纯 Python 标准库，无第三方依赖。设计在 GitHub Actions ubuntu-latest + Android
模拟器的 CI 作业里闭环运行（见 .github/workflows/android.yml 的 integration-test
作业），也可在本机对真机 / 模拟器手动执行。

角色与拓扑（协议规格 v1 第 1 节，docs/protocol/womic-wifi-protocol-v1.md）：

    ┌────────────────────────────┐   TCP 8125（控制）  ┌────────────────────────────┐
    │ 本脚本（PC = TCP client）   │ ────────────────► │ 自写 app（手机 = TCP server）│
    │  hello/config/start/       │ ◄────────────────┤  WomicSession 状态机        │
    │  stop/heartbeat            │   应答            │  + 5s 心跳看门狗            │
    │                            │ ◄────────────────┤                            │
    │  UDP 接收（格式 C）        │   Opus 媒体流     │  调试音/麦克风 → Opus 编码  │
    └────────────────────────────┘                   └────────────────────────────┘

CI 拓扑（adb forward，app 看到的 TCP 对端是设备侧 127.0.0.1）：

    127.0.0.1:18125 ──adb forward──► 模拟器 127.0.0.1:8125（app TCP server）
    app 的 UDP 目标 = TCP 对端地址 = 127.0.0.1 → 发往 127.0.0.1:<media-port>
    → 由模拟器内 `toybox nc -u -l -p <media-port>` 回收，字节流经 adb stdout 回传本脚本

用法（workflow 已装好 app 并启动服务后）：

    python3 pc_client.py --hold-seconds 12

关键参数：
    --adb PATH             adb 可执行文件（默认取 PATH 里的 adb）
    --serial SERIAL        adb 设备序列号（默认唯一在线设备）
    --device-port 8125     设备侧 TCP 控制端口（app 监听端口）
    --control-port 18125   本机侧 adb forward 端口
    --media-port 5004      CONFIG 协商的 PC 媒体端口（UDP 回收端口）
    --udp-tap auto|adb|host  UDP 回收方式（auto：forward 模式用 adb，直连模式用 host）
    --control-host HOST    TCP 连接目标（默认 127.0.0.1，即走 adb forward；
                            直连模拟器 slirp 时可填 10.0.2.15）
    --hold-seconds 12      推流观察窗口（必须 > 5s 看门狗周期才有意义）
    --volume 5             UDP 头音量字段期望值（须与 app 启动参数一致）
    --heartbeat-interval 1.0  105 心跳间隔（秒）
    --dump-packets N       把前 N 个 UDP 包以十六进制打印（调试用）
    --self-test            不连设备，用合成字节流自测解析/断言逻辑

退出码：0 = 全部通过；1 = 存在失败项；2 = 环境/用法错误。
"""

import argparse
import queue
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# 协议常量（与 android/core 的 WomicProtocol.kt / 规格 v1 第 2 节一一对应）
# ---------------------------------------------------------------------------

MSG_HELLO = 101       # 0x65 版本/UDP 头格式协商
MSG_CONFIG = 102      # 0x66 codec/采样率/媒体端口
MSG_START = 103       # 0x67 开始推流
MSG_STOP = 104        # 0x68 停止推流
MSG_HEARTBEAT = 105   # 0x69 心跳（无应答）
MSG_ERROR = 0         # 手机→PC 错误串

CODEC_OPUS = 2        # Opus（唯一被本工程接受的 codec）
RATE_INDEX_48K = 2    # 48000 Hz

# 官方 Windows 客户端 6.3 的 hello 负载（objdump @0x4147be 实证）：
# payload[0]=4 选 UDP 头格式 C，payload[1..5] 手机端跳过不读。
HELLO_PAYLOAD = bytes([4, 6, 3, 0, 0, 0])

# UDP 格式 C 头（规格 v1 第 3.3 节）：04 00 | len(u16) | seq(u16) | ts(u32) | vol(u8)
FMT_C_MAGIC = b"\x04\x00"
FMT_C_HEADER_SIZE = 11
FMT_C_VOL_UNITY = 5  # 官方默认单位增益

# app 侧看门狗周期（BuildConfig.HEARTBEAT_TIMEOUT_MS，证据 V1/k.java）
WATCHDOG_MS = 5000

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_ENV = 2


class EnvError(Exception):
    """环境/用法错误（adb 缺失、设备不在线、缺少 toybox nc 等）。"""


class ConnectionClosed(Exception):
    """TCP 对端关闭连接（读到 EOF）。"""


# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------

def u16be(buf, off):
    return (buf[off] << 8) | buf[off + 1]


def u32be(buf, off):
    return (buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]


def encode_frame(msg_type, payload):
    """[type:u8][length:u32 BE][payload]（规格 v1 第 2.1 节）。"""
    return bytes([msg_type]) + len(payload).to_bytes(4, "big") + payload


def hexs(data):
    return " ".join("%02X" % b for b in data)


class Results:
    """检查项收集器：逐项打印 [PASS]/[FAIL]，最后汇总。"""

    def __init__(self):
        self.rows = []

    def check(self, cid, name, ok, detail=""):
        ok = bool(ok)
        self.rows.append((cid, name, ok, detail))
        line = "[%s] %s %s" % ("PASS" if ok else "FAIL", cid, name)
        if detail:
            line += " -- " + detail
        print(line, flush=True)
        return ok

    def info(self, text):
        print("       " + text, flush=True)

    @property
    def failures(self):
        return [r for r in self.rows if not r[2]]

    def summary(self):
        total = len(self.rows)
        bad = self.failures
        print("", flush=True)
        print("================ 联调结果 ================", flush=True)
        for cid, name, ok, detail in self.rows:
            print("  [%s] %s %s" % ("PASS" if ok else "FAIL", cid, name), flush=True)
        print("------------------------------------------", flush=True)
        print("  合计 %d 项：通过 %d，失败 %d" % (total, total - len(bad), len(bad)), flush=True)
        print("==========================================", flush=True)
        return EXIT_OK if not bad else EXIT_FAILED


# ---------------------------------------------------------------------------
# adb 封装
# ---------------------------------------------------------------------------

class Adb:
    def __init__(self, adb_path, serial=None):
        self.path = adb_path
        self.serial = serial
        self.base = [adb_path] + (["-s", serial] if serial else [])

    def run(self, args, timeout=120, check=True):
        try:
            proc = subprocess.run(
                self.base + args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            raise EnvError("adb %s 超时（%ds）" % (" ".join(args), timeout))
        except FileNotFoundError:
            raise EnvError("找不到 adb 可执行文件：%s（用 --adb 指定路径）" % self.path)
        if check and proc.returncode != 0:
            raise EnvError(
                "adb %s 失败（退出码 %d）：%s"
                % (
                    " ".join(args),
                    proc.returncode,
                    proc.stderr.decode("utf-8", "replace").strip(),
                )
            )
        return proc

    def shell(self, command, timeout=120, check=True):
        return self.run(["shell", command], timeout=timeout, check=check)

    def shell_text(self, command, timeout=120, check=True):
        return self.shell(command, timeout=timeout, check=check).stdout.decode("utf-8", "replace")

    def forward(self, local_port, device_port):
        self.run(["forward", "--remove", "tcp:%d" % local_port], check=False)
        self.run(["forward", "tcp:%d" % local_port, "tcp:%d" % device_port])

    def online_serials(self):
        out = self.run(["devices"]).stdout.decode("utf-8", "replace")
        serials = []
        for line in out.splitlines()[1:]:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) >= 2 and parts[1] == "device":
                serials.append(parts[0])
        return serials


# ---------------------------------------------------------------------------
# TCP 控制客户端（PC 角色）
# ---------------------------------------------------------------------------

class ControlClient:
    """阻塞式 TCP 控制通道：发请求帧、收应答帧。"""

    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self._buf = bytearray()

    def _read_exact(self, n, timeout):
        deadline = time.monotonic() + timeout
        while len(self._buf) < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("读取 %d 字节超时（已缓冲 %d）" % (n, len(self._buf)))
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionClosed("TCP 对端关闭连接（EOF）")
            self._buf += chunk
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def send_frame(self, msg_type, payload=b""):
        self.sock.sendall(encode_frame(msg_type, payload))

    def recv_frame(self, timeout=10.0):
        """返回 (type, payload)；超时返回 None；EOF 抛 ConnectionClosed。"""
        try:
            hdr = self._read_exact(5, timeout)
        except TimeoutError:
            return None
        length = int.from_bytes(hdr[1:5], "big")
        payload = self._read_exact(length, timeout) if length else b""
        return hdr[0], payload

    def wait_closed(self, timeout):
        """阻塞等待对端关闭连接。

        用于看门狗断链验证：STOP 之后 app 不应再发任何帧，因此此期间收到的
        任何字节都是"意外帧"（记录并继续等 EOF）。
        返回 (closed, unexpected_chunks)。ECONNRESET 视为对端关闭。
        """
        deadline = time.monotonic() + timeout
        unexpected = []
        if self._buf:
            unexpected.append(bytes(self._buf))
            self._buf = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, unexpected
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, TimeoutError):
                return False, unexpected
            except OSError:
                return True, unexpected  # ECONNRESET 等：对端已消失
            if not chunk:
                return True, unexpected
            unexpected.append(chunk)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect_with_retry(host, port, total_timeout, interval=0.5):
    """app 可能刚启动、TCP server 还没 listen：轮询重连。"""
    deadline = time.monotonic() + total_timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return ControlClient(host, port, timeout=5.0)
        except OSError as e:
            last = e
            time.sleep(interval)
    raise EnvError(
        "%ds 内无法连接 %s:%d（最后错误：%s）——app 的 TCP server 未就绪？"
        % (total_timeout, host, port, last)
    )


class Heartbeats:
    """105 心跳线程：可启停；连接断开后自行退场。"""

    def __init__(self, client, interval=1.0):
        self.client = client
        self.interval = interval
        self._stop = threading.Event()
        self.sent = 0
        self._thread = threading.Thread(target=self._run, name="heartbeats", daemon=True)

    def _run(self):
        while not self._stop.wait(self.interval):
            try:
                self.client.send_frame(MSG_HEARTBEAT)
                self.sent += 1
            except OSError:
                return

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=3)


# ---------------------------------------------------------------------------
# UDP 媒体流回收（两种 tap）
# ---------------------------------------------------------------------------

class FormatCParser:
    """把字节流重分帧为格式 C 报文。

    自定长：total = len + 4（11 字节头 + payload，payload 长 = len - 7），
    因此 adb stdout 的字节流即使跨包粘联/拆包也能无损还原（规格 v1 第 3.3 节）。
    """

    def __init__(self):
        self.buf = bytearray()
        self.resyncs = 0

    def feed(self, chunk):
        self.buf += chunk

    def _resync(self):
        """流错位时寻找下一个 04 00（loopback 下不应发生，仅兜底）。"""
        idx = self.buf.find(FMT_C_MAGIC, 1)
        if idx < 0:
            # 没有可疑魔数：只保留尾部 3 字节（可能是不完整魔数的开头）
            tail = bytes(self.buf[-3:]) if len(self.buf) > 3 else bytes(self.buf)
            self.buf = bytearray(tail)
            return None
        self.resyncs += 1
        del self.buf[:idx]
        return True

    def pop(self):
        while True:
            if len(self.buf) < 4:
                return None
            if bytes(self.buf[0:2]) != FMT_C_MAGIC:
                if self._resync() is None:
                    return None
                continue
            length = u16be(self.buf, 2)
            if length < 7:
                # len 字段非法（真实包 len >= 7）：错位，重同步
                if self._resync() is None:
                    return None
                continue
            total = length + 4
            if len(self.buf) < total:
                return None
            pkt = bytes(self.buf[:total])
            del self.buf[:total]
            return pkt


class AdbUdpTap:
    """模拟器内 `toybox nc -u -l -p PORT`，回收的字节流重分帧。

    必须在 103 START 之前启动：app 的 UDP 是 connect 语义，监听不存在时会
    收到 ICMP port-unreachable 并把 socket 打坏（UdpSender.send 连续失败）。
    """

    MODE = "adb"

    def __init__(self, adb, media_port):
        self.adb = adb
        self.media_port = media_port
        self.proc = None
        self.parser = FormatCParser()
        self.queue = queue.Queue()
        self.received = 0
        self._start_process()

    def _start_process(self):
        # 先做一次功能性预检：命令形式不对时进程会立即退出
        candidates = [
            "toybox nc -u -l -p %d" % self.media_port,
            "toybox nc -ulp %d" % self.media_port,
        ]
        for cmd in candidates:
            try:
                proc = subprocess.Popen(
                    self.adb.base + ["shell", cmd],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                )
            except FileNotFoundError:
                raise EnvError("找不到 adb 可执行文件：%s" % self.adb.path)
            time.sleep(0.6)
            if proc.poll() is None:
                self.proc = proc
                self.cmd = cmd
                threading.Thread(target=self._pump, name="udp-tap", daemon=True).start()
                return
            proc.kill()
        raise EnvError(
            "设备上无法启动 UDP 监听（已尝试：%s）。"
            "请确认系统镜像是标准 Android 镜像（含 toybox nc）。" % " / ".join(candidates)
        )

    def _pump(self):
        while True:
            try:
                chunk = self.proc.stdout.read(65536)
            except OSError:
                return
            if not chunk:
                return
            self.queue.put(chunk)

    def read_packet(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            pkt = self.parser.pop()
            if pkt is not None:
                self.received += 1
                return pkt
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                chunk = self.queue.get(timeout=min(remaining, 0.25))
            except queue.Empty:
                continue
            self.parser.feed(chunk)

    def close(self):
        if self.proc is not None:
            try:
                self.proc.terminate()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        # 兜底：确保设备侧 nc 也退出（覆盖两种命令形式的匹配）
        for pattern in ("nc -u -l -p %d" % self.media_port, "nc -ulp %d" % self.media_port):
            try:
                self.adb.shell("pkill -f '%s'" % pattern, check=False)
            except EnvError:
                pass


class HostUdpTap:
    """host 侧 UDP socket 回收（直连模拟器 slirp 拓扑：app 的 UDP 发往 10.0.2.2）。"""

    MODE = "host"

    def __init__(self, media_port, bind_host="0.0.0.0"):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind((bind_host, media_port))
        self.sock.settimeout(0.25)
        self.received = 0

    def read_packet(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(min(remaining, 0.25))
            try:
                data, _ = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return None
            self.received += 1
            return data

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def parse_format_c(pkt):
    """解析格式 C 头，返回字段 dict；结构非法抛 ValueError。"""
    if len(pkt) < FMT_C_HEADER_SIZE:
        raise ValueError("包长 %d < 头部 %d" % (len(pkt), FMT_C_HEADER_SIZE))
    if pkt[0:2] != FMT_C_MAGIC:
        raise ValueError("magic %s != 04 00" % hexs(pkt[0:2]))
    length = u16be(pkt, 2)
    if length < 7:
        raise ValueError("len 字段 %d < 7" % length)
    payload_len = length - 7
    total = length + 4
    if len(pkt) != total:
        raise ValueError("总长 %d != len(%d)+4" % (len(pkt), length))
    return {
        "magic": pkt[0:2],
        "length": length,
        "seq": u16be(pkt, 4),
        "ts": u32be(pkt, 6),
        "vol": pkt[10],
        "payload": pkt[11:],
        "payload_len": payload_len,
        "total": total,
    }


class StreamTracker:
    """推流窗口内的连续断言状态（seq 连续 / ts 递增 / 音量一致 / 结构自洽）。"""

    def __init__(self, expected_volume):
        self.expected_volume = expected_volume
        self.reset()
        self.bad_packets = []  # (序号, 原因)

    def reset(self):
        self.count = 0
        self.first_ts = None
        self.prev_seq = None
        self.prev_ts = None
        self.seq_gaps = []
        self.ts_regressions = []
        self.vol_mismatch = 0
        self.struct_errors = 0
        self.payload_lens = []
        self.first_packet_at = None
        self.last_packet_at = None

    def observe(self, pkt, now=None):
        """喂入一个原始 UDP 报文；返回解析后的 dict（结构非法时返回 None）。"""
        if now is None:
            now = time.monotonic()
        self.count += 1
        if self.first_packet_at is None:
            self.first_packet_at = now
        self.last_packet_at = now
        try:
            hdr = parse_format_c(pkt)
        except ValueError as e:
            self.struct_errors += 1
            if len(self.bad_packets) < 5:
                self.bad_packets.append((self.count, "结构错误：%s；原始 %s" % (e, hexs(pkt[:16]))))
            return None
        if len(self.payload_lens) < 4096:
            self.payload_lens.append(hdr["payload_len"])
        # seq 连续（u16 回绕感知）
        if self.prev_seq is not None:
            delta = (hdr["seq"] - self.prev_seq) % 65536
            if delta != 1:
                self.seq_gaps.append((self.prev_seq, hdr["seq"], delta))
        self.prev_seq = hdr["seq"]
        # ts 单调递增（u32 回绕感知）
        if self.prev_ts is not None and hdr["ts"] < self.prev_ts:
            wrapped = self.prev_ts > 0xF0000000 and hdr["ts"] < 0x10000000
            if not wrapped:
                self.ts_regressions.append((self.prev_ts, hdr["ts"]))
        self.prev_ts = hdr["ts"]
        if self.first_ts is None:
            self.first_ts = hdr["ts"]
        # 音量
        if hdr["vol"] != self.expected_volume:
            self.vol_mismatch += 1
        return hdr

    def rate_per_sec(self, window_seconds):
        return self.count / window_seconds if window_seconds > 0 else 0.0


# ---------------------------------------------------------------------------
# 自测（--self-test）：不连设备，用合成字节流验证解析与断言逻辑
# ---------------------------------------------------------------------------

def build_format_c(seq, ts, payload, volume=FMT_C_VOL_UNITY):
    length = len(payload) + 7
    return FMT_C_MAGIC + length.to_bytes(2, "big") + seq.to_bytes(2, "big") \
        + ts.to_bytes(4, "big") + bytes([volume]) + payload


def self_test():
    results = Results()
    print("== pc_client.py 自测（合成字节流，不连设备） ==", flush=True)

    # 1) 单包解析
    pkt = build_format_c(0x1234, 0xDEADBEEF, b"\xAB\xCD\xEF", volume=5)
    hdr = parse_format_c(pkt)
    results.check("S01", "format_c_single_packet",
                  hdr["seq"] == 0x1234 and hdr["ts"] == 0xDEADBEEF
                  and hdr["vol"] == 5 and hdr["payload"] == b"\xAB\xCD\xEF"
                  and hdr["length"] == 10 and hdr["total"] == 14,
                  "seq=%04X ts=%d vol=%d len=%d total=%d"
                  % (hdr["seq"], hdr["ts"], hdr["vol"], hdr["length"], hdr["total"]))

    # 2) 流式重分帧：粘联 + 逐字节喂入都应还原
    pkts = [build_format_c(i, 1000 + 20 * i, bytes([i & 0xFF]) * (3 + i % 5)) for i in range(50)]
    stream = b"".join(pkts)
    parser = FormatCParser()
    parser.feed(stream)
    got = []
    while True:
        p = parser.pop()
        if p is None:
            break
        got.append(p)
    results.check("S02", "stream_reframe_coalesced", got == pkts,
                  "喂入 %d 字节，还原 %d 包" % (len(stream), len(got)))

    parser = FormatCParser()
    got2 = []
    for i in range(len(stream)):
        parser.feed(stream[i:i + 1])
        while True:
            p = parser.pop()
            if p is None:
                break
            got2.append(p)
    results.check("S03", "stream_reframe_byte_dribble", got2 == pkts,
                  "逐字节喂入，还原 %d 包" % len(got2))

    # 3) tracker：连续 seq / 递增 ts / 音量一致
    tracker = StreamTracker(expected_volume=5)
    ok = True
    for i, p in enumerate(pkts):
        if tracker.observe(p) is None:
            ok = False
    results.check("S04", "tracker_clean_stream",
                  ok and tracker.count == 50 and not tracker.seq_gaps
                  and not tracker.ts_regressions and tracker.vol_mismatch == 0
                  and tracker.struct_errors == 0,
                  "count=%d gaps=%d ts_regress=%d vol_bad=%d"
                  % (tracker.count, len(tracker.seq_gaps),
                     len(tracker.ts_regressions), tracker.vol_mismatch))

    # 4) tracker：seq 跳号 / ts 回退 / 音量错 都能被抓到
    bad = [build_format_c(0, 100, b"\x01"), build_format_c(2, 90, b"\x02", volume=3),
           build_format_c(3, 200, b"\x03")]
    tracker2 = StreamTracker(expected_volume=5)
    for p in bad:
        tracker2.observe(p)
    results.check("S05", "tracker_detects_anomalies",
                  len(tracker2.seq_gaps) == 1 and len(tracker2.ts_regressions) == 1
                  and tracker2.vol_mismatch == 1,
                  "gaps=%s ts_regress=%s vol_bad=%d"
                  % (tracker2.seq_gaps, tracker2.ts_regressions, tracker2.vol_mismatch))

    # 5) u16 seq 回绕连续
    wrap = [build_format_c(0xFFFF, 10, b"\x01"), build_format_c(0x0000, 30, b"\x02"),
            build_format_c(0x0001, 50, b"\x03")]
    tracker3 = StreamTracker(expected_volume=5)
    for p in wrap:
        tracker3.observe(p)
    results.check("S06", "tracker_seq_wrap_ok", not tracker3.seq_gaps,
                  "seq 0xFFFF→0x0000→0x0001 无跳号")

    # 6) 坏流：magic 错位可重同步
    corrupt = b"\xFF\xFF" + build_format_c(7, 70, b"\x09")
    parser = FormatCParser()
    parser.feed(corrupt)
    p = parser.pop()
    results.check("S07", "parser_resync_after_garbage",
                  p is not None and u16be(p, 4) == 7 and parser.resyncs >= 1,
                  "resyncs=%d" % parser.resyncs)

    # 7) 官方字节：hello / 帧编码
    hello = encode_frame(MSG_HELLO, HELLO_PAYLOAD)
    results.check("S08", "official_hello_bytes",
                  hello == bytes([0x65, 0, 0, 0, 6, 4, 6, 3, 0, 0, 0]),
                  "hello 帧 = %s" % hexs(hello))
    config = encode_frame(MSG_CONFIG, bytes([2, 2]) + (5004).to_bytes(4, "big"))
    results.check("S09", "config_bytes_big_endian_port",
                  config == bytes([0x66, 0, 0, 0, 6, 2, 2, 0, 0, 0x13, 0x8C]),
                  "config 帧 = %s" % hexs(config))

    return results.summary()


# ---------------------------------------------------------------------------
# 联调主流程
# ---------------------------------------------------------------------------

def run_interop(args):
    results = Results()
    adb = Adb(args.adb, args.serial)

    # ---- 环境检查 ----
    if not args.skip_adb_setup:
        serials = adb.online_serials()
        if not serials:
            raise EnvError("没有在线 adb 设备（adb devices 为空）")
        if args.serial is None and len(serials) > 1:
            raise EnvError("检测到多个设备（%s），请用 --serial 指定" % ", ".join(serials))
        if args.serial:
            adb.serial = args.serial
            adb.base = [args.adb, "-s", args.serial]
        results.info("设备：%s" % (args.serial or serials[0]))

        # ---- TCP 通道（adb forward）----
        adb.forward(args.control_port, args.device_port)
        results.info("adb forward tcp:%d -> tcp:%d" % (args.control_port, args.device_port))
    else:
        results.info("已跳过 adb 设备检查与 forward 设置（--skip-adb-setup）")

    client = connect_with_retry(args.control_host, args.control_port, args.connect_timeout)
    results.check("C01", "tcp_connect",
                  True, "%s:%d 已连接" % (args.control_host, args.control_port))

    # ---- 心跳必须**连接即开始** ----
    # 官方 app 的看门狗是连接级的（accept 后立刻开始计时，每 5s 检查一次），
    # 与推流与否无关。上版把心跳放在"等首包 10s"之后才启动，结果 app 在
    # 连接后 5s 就把链断了（run 39464501266 日志：看门狗超时 → 采集线程退出）。
    heartbeats = Heartbeats(client, interval=args.heartbeat_interval)
    heartbeats.start()

    # ---- hello（101）----
    client.send_frame(MSG_HELLO, HELLO_PAYLOAD)
    reply = client.recv_frame(timeout=10)
    ok = reply is not None and reply[0] == MSG_HELLO and reply[1] == bytes([0, 4])
    results.check("C02", "hello_reply", ok,
                  "期望 65 00000002 00 04，实得 %s"
                  % ("None" if reply is None else "%02X %s %s"
                     % (reply[0], len(reply[1]).to_bytes(4, "big").hex(), hexs(reply[1]))))

    # ---- UDP tap 必须在 config/start 之前就绪 ----
    if args.udp_tap == "host":
        tap = HostUdpTap(args.media_port, args.bind_host)
    else:
        tap = AdbUdpTap(adb, args.media_port)
    results.info("UDP 回收方式：%s（媒体端口 %d）" % (tap.MODE, args.media_port))

    # ---- config（102）----
    client.send_frame(MSG_CONFIG,
                      bytes([args.codec, args.rate_index]) + args.media_port.to_bytes(4, "big"))
    reply = client.recv_frame(timeout=10)
    ok = reply is not None and reply[0] == MSG_CONFIG and reply[1] == bytes([0])
    results.check("C03", "config_reply", ok,
                  "期望 66 00000001 00，实得 %s"
                  % ("None" if reply is None else "%02X len=1 %s" % (reply[0], hexs(reply[1]))))

    # ---- start（103）----
    client.send_frame(MSG_START)
    reply = client.recv_frame(timeout=10)
    ok = reply is not None and reply[0] == MSG_START and reply[1] == bytes([0])
    results.check("C04", "start_reply", ok,
                  "期望 67 00000001 00，实得 %s"
                  % ("None" if reply is None else "%02X len=1 %s" % (reply[0], hexs(reply[1]))))

    # ---- 首个 UDP 包：格式 C 字节级 ----
    tracker = StreamTracker(expected_volume=args.volume)
    pkt = tap.read_packet(timeout=args.first_packet_timeout)
    first_packet_failed = False
    if pkt is None:
        first_packet_failed = True
        # 没有 UDP 流，后续断言全部失去意义：探一下 TCP 是否还活着，给出指向性诊断后收尾。
        # 注意：对端关闭时 recv_frame 抛 ConnectionClosed，必须兜住（否则整个脚本崩掉）。
        try:
            alive = client.recv_frame(timeout=1.0) is None
            state = "仍存活（app 在跑但没发包：查 OpusEncoder/UdpSender）" \
                if alive else "已收到帧（异常）"
        except ConnectionClosed:
            state = "已断开（app 侧主动关闭：看门狗超时？或 app 报错退出，查 logcat）"
        results.check("C05", "udp_first_packet_format_c", False,
                      "%ds 内未收到任何 UDP 包；TCP 控制通道%s"
                      % (args.first_packet_timeout, state))
        results.info("诊断建议：adb logcat -d | grep -iE 'womic|UdpSender|OpusEncoder|MicCapture'")
    else:
        hdr = tracker.observe(pkt)
        if hdr is None:
            first_packet_failed = True
            results.check("C05", "udp_first_packet_format_c", False,
                          "首包结构错误：%s" % (tracker.bad_packets[0][1] if tracker.bad_packets else "?"))
        else:
            ok = (hdr["magic"] == FMT_C_MAGIC and hdr["length"] == hdr["payload_len"] + 7
                  and hdr["total"] == hdr["length"] + 4 and hdr["vol"] == args.volume
                  and hdr["payload_len"] >= 1)
            results.check("C05", "udp_first_packet_format_c", ok,
                          "magic=0400 len=%d seq=%d ts=%d vol=%d payload=%dB total=%dB"
                          % (hdr["length"], hdr["seq"], hdr["ts"], hdr["vol"],
                             hdr["payload_len"], hdr["total"]))
            if args.dump_packets > 0:
                print("       [dump] %s" % hexs(pkt[:64]), flush=True)

    if first_packet_failed:
        # 提前收尾：UDP 流不存在时，推流质量/看门狗/重连断言都无从谈起
        heartbeats.stop()
        try:
            client.close()
        except OSError:
            pass
        tap.close()
        if not args.skip_adb_setup:
            try:
                adb.run(["forward", "--remove", "tcp:%d" % args.control_port], check=False)
            except EnvError:
                pass
        return results.summary()

    # ---- 推流观察窗口（心跳跨看门狗 + 流质量断言）----
    # 心跳线程已在 C01 之后启动（见上），这里直接进入观察窗口
    window_start = time.monotonic()
    deadline = window_start + args.hold_seconds
    dumped = 1 if args.dump_packets > 0 else 0
    while time.monotonic() < deadline:
        pkt = tap.read_packet(timeout=0.25)
        if pkt is None:
            continue
        tracker.observe(pkt)
        if dumped < args.dump_packets:
            print("       [dump] seq=%d %s" % (tracker.prev_seq, hexs(pkt[:48])), flush=True)
            dumped += 1
    window_seconds = time.monotonic() - window_start

    results.check("C06", "udp_seq_continuity", not tracker.seq_gaps,
                  "%d 包，跳号 %d 处%s"
                  % (tracker.count, len(tracker.seq_gaps),
                     ("（%s）" % tracker.seq_gaps[:3]) if tracker.seq_gaps else ""))
    results.check("C07", "udp_ts_monotonic", not tracker.ts_regressions,
                  "ts 回退 %d 处%s"
                  % (len(tracker.ts_regressions),
                     ("（%s）" % tracker.ts_regressions[:3]) if tracker.ts_regressions else ""))
    results.check("C08", "udp_volume_unity", tracker.vol_mismatch == 0,
                  "音量 != %d 的包 %d 个" % (args.volume, tracker.vol_mismatch))
    results.check("C09", "udp_structure_all_valid", tracker.struct_errors == 0,
                  "结构错误 %d 个" % tracker.struct_errors)
    rate = tracker.rate_per_sec(window_seconds)
    results.check("C10", "udp_rate_sane", rate >= args.min_rate,
                  "%.1f 包/秒（窗口 %.1fs，共 %d 包，阈值 %.1f）"
                  % (rate, window_seconds, tracker.count, args.min_rate))
    # 看门狗生存：窗口 > 5s（看门狗周期）且末段仍有包 → 心跳跨过了看门狗
    alive_after_watchdog = (
        window_seconds > (WATCHDOG_MS / 1000.0) + 1.0
        and tracker.last_packet_at is not None
        and (time.monotonic() - tracker.last_packet_at) < 2.0
    )
    results.check("C11", "heartbeat_survives_watchdog", alive_after_watchdog,
                  "窗口 %.1fs > 看门狗 %ds，且连接仍存活（心跳已发 %d 个）"
                  % (window_seconds, WATCHDOG_MS // 1000, heartbeats.sent))
    results.info("payload 长度分布：min=%dB max=%dB avg=%dB"
                 % (min(tracker.payload_lens) if tracker.payload_lens else 0,
                    max(tracker.payload_lens) if tracker.payload_lens else 0,
                    int(sum(tracker.payload_lens) / len(tracker.payload_lens))
                    if tracker.payload_lens else 0))

    # ---- stop（104）：官方怪癖——应答是 type 102 帧 ----
    client.send_frame(MSG_STOP)
    reply = client.recv_frame(timeout=10)
    ok = reply is not None and reply[0] == MSG_CONFIG and reply[1] == bytes([0])
    results.check("C12", "stop_reply_is_type102_quirk", ok,
                  "期望 66 00000001 00（官方 d2/e 无参构造怪癖，规格 v1 第 2.2 节），实得 %s"
                  % ("None" if reply is None else "%02X len=1 %s" % (reply[0], hexs(reply[1]))))

    # ---- 停止后静默（给采集线程留 join 时间，再取基线）----
    time.sleep(1.8)
    baseline = tracker.count
    time.sleep(1.5)
    results.check("C13", "silence_after_stop", tracker.count == baseline,
                  "停止后又到达 %d 个包（采集线程 join 窗口已过）" % (tracker.count - baseline))

    # ---- 停心跳 → 看门狗断链 ----
    heartbeats.stop()
    heartbeats_stopped_at = time.monotonic()
    closed, unexpected = client.wait_closed(args.watchdog_timeout)
    if unexpected:
        results.info("停心跳后收到意外帧数据：%s" % hexs(b"".join(unexpected)[:32]))
    disconnect_delay = (time.monotonic() - heartbeats_stopped_at) if closed else -1.0
    # 官方行为：看门狗每 5s 检查一次，末次心跳后 5~10s 内断链；过早断开同样是缺陷
    timing_ok = closed and 4.0 <= disconnect_delay <= args.watchdog_timeout
    results.check("C14", "watchdog_disconnect_on_heartbeat_loss", timing_ok,
                  "%s：末次心跳后 %.1fs 断开（看门狗周期 %ds，预期 5~10s）"
                  % ("已断开" if closed else "连接未断开（超时 %.0fs）" % args.watchdog_timeout,
                     disconnect_delay, WATCHDOG_MS // 1000))
    client.close()

    # ---- 重连：完整会话再来一轮 ----
    tracker.reset()
    try:
        client2 = connect_with_retry(args.control_host, args.control_port, args.reconnect_timeout)
        ok = True
        detail = []
        client2.send_frame(MSG_HELLO, HELLO_PAYLOAD)
        r = client2.recv_frame(timeout=10)
        ok = ok and r is not None and r[0] == MSG_HELLO and r[1] == bytes([0, 4])
        detail.append("hello=%s" % ("ok" if ok else "bad"))
        client2.send_frame(MSG_CONFIG,
                           bytes([args.codec, args.rate_index]) + args.media_port.to_bytes(4, "big"))
        r = client2.recv_frame(timeout=10)
        ok = ok and r is not None and r[0] == MSG_CONFIG and r[1] == bytes([0])
        detail.append("config=%s" % ("ok" if r and r[0] == MSG_CONFIG and r[1] == bytes([0]) else "bad"))
        client2.send_frame(MSG_START)
        r = client2.recv_frame(timeout=10)
        ok = ok and r is not None and r[0] == MSG_START and r[1] == bytes([0])
        detail.append("start=%s" % ("ok" if r and r[0] == MSG_START and r[1] == bytes([0]) else "bad"))
        pkt = tap.read_packet(timeout=args.first_packet_timeout)
        got_packet = pkt is not None and tracker.observe(pkt) is not None
        ok = ok and got_packet
        detail.append("udp=%s" % ("ok" if got_packet else "none"))
        client2.send_frame(MSG_STOP)
        r = client2.recv_frame(timeout=10)
        ok = ok and r is not None and r[0] == MSG_CONFIG and r[1] == bytes([0])
        detail.append("stop_quirk=%s" % ("ok" if r and r[0] == MSG_CONFIG else "bad"))
        results.check("C15", "reconnect_full_session", ok, "，".join(detail))
        client2.close()
    except (EnvError, ConnectionClosed) as e:
        results.check("C15", "reconnect_full_session", False, str(e))

    # ---- 收尾 ----
    tap.close()
    if not args.skip_adb_setup:
        try:
            adb.run(["forward", "--remove", "tcp:%d" % args.control_port], check=False)
        except EnvError:
            pass
    if tap.MODE == "adb" and tap.parser.resyncs:
        results.info("注意：UDP 字节流发生 %d 次重同步（loopback 下不应出现）" % tap.parser.resyncs)

    return results.summary()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def parse_args(argv):
    p = argparse.ArgumentParser(
        description="WO Mic WiFi 协议联调脚本（PC 客户端角色，纯 stdlib）")
    p.add_argument("--adb", default="adb", help="adb 可执行文件路径（默认 PATH 查找）")
    p.add_argument("--serial", default=None, help="adb 设备序列号（默认唯一在线设备）")
    p.add_argument("--control-host", default="127.0.0.1",
                   help="TCP 控制通道目标（默认 127.0.0.1 走 adb forward；"
                        "直连模拟器可填 10.0.2.15）")
    p.add_argument("--control-port", type=int, default=18125, help="本机侧 forward 端口")
    p.add_argument("--device-port", type=int, default=8125, help="设备侧 app 控制端口")
    p.add_argument("--media-port", type=int, default=5004, help="CONFIG 协商的媒体端口")
    p.add_argument("--bind-host", default="0.0.0.0", help="host tap 的绑定地址")
    p.add_argument("--udp-tap", choices=["auto", "adb", "host"], default="auto",
                   help="UDP 回收方式（默认 auto）")
    p.add_argument("--volume", type=int, default=FMT_C_VOL_UNITY, help="期望的 UDP 音量字段")
    p.add_argument("--codec", type=int, default=CODEC_OPUS, help="CONFIG codec（默认 2=Opus）")
    p.add_argument("--rate-index", type=int, default=RATE_INDEX_48K,
                   help="CONFIG 采样率索引（默认 2=48000）")
    p.add_argument("--hold-seconds", type=float, default=12.0,
                   help="推流观察窗口秒数（须 > 5s 看门狗周期）")
    p.add_argument("--heartbeat-interval", type=float, default=1.0, help="105 心跳间隔（秒）")
    p.add_argument("--min-rate", type=float, default=15.0, help="推流速率下限（包/秒）")
    p.add_argument("--first-packet-timeout", type=float, default=10.0, help="首包等待（秒）")
    p.add_argument("--watchdog-timeout", type=float, default=15.0,
                   help="停心跳后等待看门狗断链的上限（秒）")
    p.add_argument("--connect-timeout", type=float, default=30.0, help="TCP 连接重试总时长（秒）")
    p.add_argument("--reconnect-timeout", type=float, default=15.0, help="重连等待（秒）")
    p.add_argument("--dump-packets", type=int, default=0, help="十六进制转印前 N 个 UDP 包")
    p.add_argument("--skip-adb-setup", action="store_true",
                   help="跳过 adb 设备检查与 forward 设置（自行准备好 TCP/UDP 通路时用，"
                        "例如对 mock server 做本地干跑）")
    p.add_argument("--self-test", action="store_true", help="不连设备，自测解析/断言逻辑")
    args = p.parse_args(argv)

    if args.udp_tap == "auto":
        # adb forward 模式下 app 的 UDP 目标是设备 loopback → 必须用设备内 nc 回收；
        # 直连模式（control-host 指向模拟器 IP）下目标是 10.0.2.2 → host 侧回收。
        args.udp_tap = "adb" if args.control_host in ("127.0.0.1", "localhost") else "host"
    if args.volume not in range(0, 256):
        p.error("--volume 必须在 0..255")
    if args.hold_seconds <= WATCHDOG_MS / 1000.0:
        p.error("--hold-seconds 必须大于看门狗周期 %ds" % (WATCHDOG_MS // 1000))
    return args


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if args.self_test:
        return self_test()
    try:
        return run_interop(args)
    except EnvError as e:
        print("[ENV-ERROR] %s" % e, flush=True)
        return EXIT_ENV
    except KeyboardInterrupt:
        print("[ABORT] 用户中断", flush=True)
        return EXIT_ENV


if __name__ == "__main__":
    sys.exit(main())
