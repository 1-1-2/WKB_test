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
    → 由模拟器内 `toybox nc -u -l -p <media-port>` 回收，字节流经 adb exec-out
      （无 PTY、不经 ONLCR，见 AdbUdpTap 文档字符串）回传本脚本

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
    --c05-tap-retries 2    C05 未收到首包时，restart tap 后重试的次数
                          （tap 启动竞态自救；0 = 关闭；不影响 workflow 层重试）

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

# UDP tap 的 bind 就绪探测（A 项；run 40049236444 C05 红轮根因修复）：
# 旧实现 sleep(0.6) + proc.poll() is None 只能证明 adb 进程活着，不能
# 证明 nc 已 bind 媒体端口（shell fork 慢/系统繁忙时会误判"启动成功"，随后 tap
# 静默死亡，C05 红轮被误导向"app 没发包"）。现在以 /proc/net/udp(6) 出现媒体
# 端口 hex 监听行为准（5004 → 138C）。
BIND_READY_TIMEOUT = 10.0     # 累计等待上限（与 C05 首包 10s 窗口匹配）
BIND_POLL_INTERVAL = 0.25     # 轮询间隔（adb 往返约百 ms，兼顾响应与开销）
UDP_PROC_FILES = ("/proc/net/udp", "/proc/net/udp6")  # nc -6 时才进 udp6

# 读不到 /proc/net/udp 时的降级确认窗（run 40061050114 回归修复）：
# bind 就绪探测依赖读 /proc/net/udp(6)，但权限收紧的 ROM / SELinux 策略 /
# 定制镜像可能根本不允许 adb shell 读它。此时"没有监听行"不能证明 nc 没
# bind 上——若一律按未就绪处理，tap 会完全起不来，比修复前更糟（修复前
# 至少 poll() 能放过大多数情况）。因此：两个 proc 文件都读不到内容
# （blind）且 nc 进程连续存活超过本窗口时，按旧判据（进程活着）降级放行，
# 并打印明确的"降级"日志（不静默放弃 bind 就绪探测的保护）。
BIND_DEGRADE_CONFIRM = 2.0    # 降级确认窗（秒）；调小时须保证 < BIND_READY_TIMEOUT


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


def fmt_exit_code(code):
    """subprocess 退出码 → 人类可读（None = 尚未收割；负数 = 被信号杀死）。"""
    if code is None:
        return "未知"
    if code < 0:
        return "%d（被信号 %d 终止）" % (code, -code)
    return str(code)


def udp_port_hex(port):
    """/proc/net/udp 的 local_address 端口段：4 位十六进制（5004 → 138C）。

    内核把 16 位端口按网络序原样打印成一个 hex 数（无需大小端转换）；
    比较时大小写不敏感，防内核/toybox 版本差异。
    """
    return "%04X" % port


def parse_udp_listen_lines(text, port):
    """从 /proc/net/udp(6) 文本中挑出 local 端口 == port 的行。

    列格式：sl local_address rem_address st tx_queue:rx_queue ...
    local_address = "hexip:hexport"（如 00000000:138C；udp6 为 32 位 hex ip）。
    返回 [(st, rx_queue, 原始行), ...]；rx_queue 解析失败记为 -1。兼容老内核
    "tx_queue rx_queue 分列"格式。端口比较大小写不敏感（5004=138C=138c）。
    """
    want = udp_port_hex(port)
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 5 or ":" not in parts[1]:
            continue  # 表头/畸形行
        if parts[1].rsplit(":", 1)[1].upper() != want:
            continue
        try:
            qfield = parts[4]
            if ":" in qfield:            # 新内核：tx_queue:rx_queue 单列
                rx = int(qfield.split(":")[1], 16)
            else:                        # 老内核：tx_queue rx_queue 分列
                rx = int(parts[5], 16)
        except (IndexError, ValueError):
            rx = -1
        rows.append((parts[3], rx, line.rstrip()))
    return rows


def filter_nc_ps_lines(ps_out):
    """从 `ps -A` 输出中挑出真正的 nc 进程行（D 项，降噪）。

    裸 `grep nc` 会把 incidentd/gnss-service/launcher3 等名字含 "nc" 子串的
    系统进程全匹配进来，真正的 nc 被噪声淹没（run 40049236444 实证：ps 里
    "没有任何真正的 nc 进程"被一堆含 nc 子串的系统进程掩盖，误导排查方向）。
    这里按**整词** nc（token 精确匹配）或参数里的 "nc -u"/"nc -l" 形式过滤，
    并滤掉 grep 自身。
    """
    rows = []
    for line in ps_out.splitlines():
        s = line.strip()
        if not s or "grep" in s:
            continue
        tokens = s.split()
        if "nc" in tokens or "nc -u" in s or "nc -l" in s:
            rows.append(s)
    return rows


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

    必须在 103 START 之前启动：app 的 UDP socket 未 connect，Linux 不会把
    ICMP port-unreachable 上报给发送方，但监听就绪再 START 更接近真实场景
    （真实 PC 客户端总是先占住媒体端口）。

    **为什么必须 exec-out 而不是 shell（C05 偶发失败的真正根因，2026-10-24
    沙箱实验钉死）**：`adb shell` 会在设备端为命令分配 **PTY**，PTY 默认开启
    **ONLCR**——输出中的每个 `0x0A` 字节都被改写成 `0x0D 0x0A`（一个字节变
    两个，后续内容全部错位）。沙箱实测（host `nc -u -l` + python pty，发包
    `0a 0d 41 0a 42 00 ff`）：PTY 后 `0d0a0d410d0a4200ff`（字节数增加、内容
    错位），PIPE 后 `0a0d410a4200ff`（字节一致）。Opus 是二进制流：单个
    160 字节帧不含 `0x0A` 的概率约 (255/256)^160 ≈ **53%**——即约 **47% 的
    首包必然含 `0x0A`**，帧的长度前缀与实际内容错位，`FormatCParser` 解析
    失败。这解释了历史上所有的"C05 抖动"（10-21 全绿、10-23 连续红、10-24
    一红一绿）：同代码一红一绿，**与 android/ 的代码改动毫无关系**（UI 改造
    是巧合，不是原因），"链路抖动""模拟器麦克风无声"等解释均为错误方向。
    `adb exec-out` 是官方为二进制流设计的通道，不在设备端分配 PTY，字节原样
    回传（其 stderr 不走 stdout，本类本就 `stderr=subprocess.DEVNULL`，无
    影响）。**结论：C05 的偶发失败是 harness 缺陷，从来不是 app 缺陷。**
    回归判据：`test_pc_client.py` 的 `ExecOutChannelTest` 与 `run-tests.sh`
    的 T20/T21——`_start_process()` 一旦回退到 shell，这些用例转红。

    **bind 就绪探测（run 40049236444 根因修复）**：`sleep(0.6)` + `poll()`
    只能证明 adb 进程活着，不能证明 nc 已 bind 媒体端口——adb fork
    慢/系统繁忙时会误判"启动成功"，随后 tap 静默死亡，C05 红轮被误导向
    "app 没发包"。`_start_process()` 现在轮询 `/proc/net/udp(6)`，直到出现
    媒体端口的 hex 监听行（5004 → 138C）且 nc 进程存活；累计 10s 未就绪的
    候选会被 kill 并换下一个，全部失败抛 `EnvError`（exit=2 环境错误，
    不混为断言失败 exit=1）。

    **读不到 /proc/net/udp 时降级（run 40061050114 回归修复）**：bind 就绪
    探测依赖读 `/proc/net/udp(6)`，但权限收紧的 ROM / SELinux 策略 / 定制
    镜像可能根本不允许 adb shell 读它——若把"读不到"当成"没 bind 上"，
    tap 会完全起不来，比修复前更糟（修复前至少 poll() 能放过大多数情况）。
    `_probe_udp_listen()` 因此区分两种"无监听行"：(a) 读到了内容但没有该
    端口的行（继续等到超时，这才是真的没 bind 上）；(b) 两个 proc 文件都
    读不到内容（没有能见度，无法证伪 nc 是否已 bind）。属于 (b) 且 nc 进程
    连续存活超过 `BIND_DEGRADE_CONFIRM`（2s）时，按旧判据（进程活着）降级
    放行，并打印 `[tap] 降级：无法读取 /proc/net/udp(6)（原因：...）`——
    降级不静默，使用者能从日志知道这次没享受到 bind 就绪探测的保护。

    **tap 死亡监控（同一次红轮的定向修复）**：`_pump` 读到 EOF（nc 退出）时
    置线程安全标志并记录退出码；C05 失败信息据此区分"harness 问题"（tap 已
    死）与"app 发包但 tap 未收到"（tap 活着），不再输出误导人的"app 在跑但
    没发包"。

    **已知局限（重连会话必须 restart）**：toybox `nc -u -l` 收到**首个**数据报后
    会把 socket connect 到该对端（netcat.c：recvfrom(..., MSG_PEEK) 取到对端
    地址后 xconnect），之后内核只交付该对端的包。而 app 每个会话都会新建
    DatagramSocket（UdpSender.open() 先 close 再建），第二会话的源端口必然
    不同 → 旧监听把新会话的包全部丢弃。run 39473353065 实证：重连后 app 侧
    "已打开 UDP -> 127.0.0.1:5004" + "合成 440.0Hz 正弦"日志齐全、UdpSender
    无任何发送错误，PC 却 10s 收不到一个包（C15 udp=none）。这不是 app 缺陷
    （真实 PC 客户端用未 connect 的 UDP socket，从任何源端口都能收），而是本
    harness 的局限，所以重连前必须 restart() 换一个未 connect 的新监听。
    """

    MODE = "adb"

    def __init__(self, adb, media_port, bind_timeout=BIND_READY_TIMEOUT,
                 bind_poll_interval=BIND_POLL_INTERVAL,
                 bind_degrade_confirm=BIND_DEGRADE_CONFIRM):
        self.adb = adb
        self.media_port = media_port
        self.proc = None
        self.cmd = None
        self.parser = FormatCParser()
        self.queue = queue.Queue()
        self.received = 0
        self.bind_timeout = bind_timeout
        self.bind_poll_interval = bind_poll_interval
        # 读不到 /proc/net/udp 时的降级确认窗（秒）：blind 且 nc 连续存活
        # 这么久 → 按进程存活判据放行（见 _wait_bind_ready）
        self.bind_degrade_confirm = bind_degrade_confirm
        # 本次启动是否走了降级判据（没验证端口 bind）——进 describe_tap_health
        self.bind_degraded = False
        # tap 死亡监控（B 项）：EOF 时置位；death_* 记录退出码与原因
        self._dead = threading.Event()
        self.death_reason = None
        self.death_exit_code = None
        # 世代号：restart() 换新进程后，旧 pump 线程的死亡上报不污染新会话
        self._generation = 0
        self._stopping = False
        self._pump_thread = None
        self._last_probe_error = None
        self._start_process()

    # ---- 启动：bind 就绪探测（A 项）----

    def _start_process(self):
        # 先做一次功能性预检：命令形式不对时进程会立即退出
        candidates = [
            "toybox nc -u -l -p %d" % self.media_port,
            "toybox nc -ulp %d" % self.media_port,
        ]
        # 清掉可能残留的旧 nc（上一次 attempt 被 timeout 杀掉来不及收尾时，
        # 端口被占会导致新 nc bind 失败退出）。UDP socket close 即释放端口，
        # pkill 后立刻 bind 不会 EADDRINUSE。
        self._kill_stale_nc()
        started_at = time.monotonic()
        deadline = started_at + self.bind_timeout
        last_diag = "未尝试任何候选命令"
        for cmd in candidates:
            try:
                # exec-out 而非 shell：adb shell 会在设备端分配 PTY，PTY 默认
                # 开启 ONLCR——输出中每个 0x0A 字节被改写成 0x0D 0x0A，Opus
                # 二进制帧被毁（C05 偶发失败的真正根因，详见类文档字符串）。
                # exec-out 是官方为二进制流设计的通道，不在设备端分配 PTY，
                # 字节原样回传；其 stderr 不走 stdout，本就走 DEVNULL，无影响。
                proc = subprocess.Popen(
                    self.adb.base + ["exec-out", cmd],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                )
            except FileNotFoundError:
                raise EnvError("找不到 adb 可执行文件：%s" % self.adb.path)
            ready, degraded, last_diag = self._wait_bind_ready(proc, cmd, deadline)
            if ready:
                self._generation += 1
                self._dead.clear()
                self.death_reason = None
                self.death_exit_code = None
                self._stopping = False
                self.bind_degraded = degraded
                self.proc = proc
                self.cmd = cmd
                # proc/generation 以参数传入：restart() 换新进程后，旧线程仍读
                # 旧管道，不会串到新会话的字节流上；旧世代的死亡上报也会因
                # generation 不匹配被丢弃
                self._pump_thread = threading.Thread(
                    target=self._pump, args=(proc, self._generation),
                    name="udp-tap", daemon=True)
                self._pump_thread.start()
                print("[tap] UDP 监听就绪：%s（端口 %d，hex %s）%s"
                      % (cmd, self.media_port, udp_port_hex(self.media_port),
                         "（降级：读不到 /proc/net/udp，按进程存活判据放行）"
                         if degraded else ""),
                      flush=True)
                return
            # 未就绪：杀掉这个候选，把剩余预算给下一个
            self._terminate(proc)
        raise EnvError(
            "设备上 UDP 监听未就绪（媒体端口 %d / hex %s，累计等待 %.1fs）。"
            "已尝试命令：%s。最后状态：%s。"
            "请确认系统镜像是标准 Android 镜像（含 toybox nc）。"
            "此为环境错误（exit=2），不是断言失败。"
            % (self.media_port, udp_port_hex(self.media_port),
               time.monotonic() - started_at, " / ".join(candidates), last_diag))

    def _wait_bind_ready(self, proc, cmd, deadline):
        """轮询 /proc/net/udp(6) 直到 nc 真的 bind 了媒体端口（A 项核心）。

        就绪判据（按判定顺序）：
        1. **bind 就绪探测（正判据）**：出现该端口的 hex 监听行 **且** nc
           进程仍活着。两个条件都必要：进程死了监听行会随之消失（tap 静默
           死亡）；端口被别的进程占着时我们的 nc 会 bind 失败退出，单靠
           "有监听行"会造成假就绪；
        2. **降级判据（blind 兜底）**：/proc/net/udp(6) 读不到内容（blind，
           见 _probe_udp_listen）且 nc 进程连续存活超过 bind_degrade_confirm
           （或总预算耗尽）——按旧判据（进程活着）放行，打印明确的"降级"
           日志（不静默）；
        3. **进程退出**：未就绪（优先级高于降级：死进程不得放行）；
        4. **总超时**：读得到内容但始终没有该端口的行，未就绪——这才是真的
           没 bind 上。

        返回 (ready, degraded, diag)；degraded=True 表示本次就绪走了降级
        判据（没享受到 bind 就绪探测的保护）。diag 是人类可读的现场描述
        （进 EnvError 与日志，探测本身也是诊断资产）。
        """
        port_hex = udp_port_hex(self.media_port)
        started_at = deadline - self.bind_timeout
        next_log = time.monotonic() + 2.0
        while True:
            rows, blind, probe_reason = self._probe_udp_listen()
            if proc.poll() is not None:
                return False, False, (
                    "nc 进程已退出（exit=%s），/proc/net/udp(6)%s 端口 %s 的"
                    "监听行（候选命令：%s）"
                    % (fmt_exit_code(proc.poll()),
                       "有" if rows else "无", port_hex, cmd))
            if rows:
                return True, False, (
                    "监听行已出现（%d 行，首行 st=%s rx_queue=%s）"
                    % (len(rows), rows[0][0], rows[0][1]))
            now = time.monotonic()
            alive_for = now - started_at
            if blind:
                # (b) 根本读不到内容：没有能见度，无法证伪 nc 已 bind。nc 连续
                # 存活超过确认窗（或总预算耗尽）→ 降级按进程存活判据放行。
                if alive_for >= self.bind_degrade_confirm or now >= deadline:
                    print("[tap] 降级：无法读取 /proc/net/udp(6)（原因：%s），"
                          "nc 已连续存活 %.1fs，退回进程存活判据——本次启动未"
                          "享受 bind 就绪探测的保护"
                          % (probe_reason or "未知原因", alive_for), flush=True)
                    return True, True, (
                        "降级就绪：无法读取 /proc/net/udp(6)（原因：%s），"
                        "nc 连续存活 %.1fs，按进程存活判据放行"
                        % (probe_reason or "未知原因", alive_for))
                if now >= next_log:
                    print("[tap] 无法读取 /proc/net/udp(6)（%s），nc 已连续"
                          "存活 %.1fs；持续读不到将在 %.1fs 后按进程存活判据"
                          "降级放行"
                          % (probe_reason or "未知原因", alive_for,
                             max(self.bind_degrade_confirm - alive_for, 0.0)),
                          flush=True)
                    next_log = now + 2.0
                time.sleep(self.bind_poll_interval)
                continue
            # (a) 读到了内容但没有该端口的行：继续等，直到超时
            if now >= deadline:
                return False, False, (
                    "nc 进程存活，但等待 %.1fs 后 /proc/net/udp(6) 仍无端口 %d"
                    "（hex %s）的监听行%s"
                    % (self.bind_timeout, self.media_port, port_hex,
                       ("；最后探测错误：%s" % self._last_probe_error)
                       if self._last_probe_error else ""))
            if now >= next_log:
                print("[tap] 仍在等待 nc bind 端口 %d（hex %s），已 %.1fs…"
                      % (self.media_port, port_hex, alive_for), flush=True)
                next_log = now + 2.0
            time.sleep(self.bind_poll_interval)

    def _probe_udp_listen(self):
        """读 /proc/net/udp 与 /proc/net/udp6，返回 (rows, blind, reason)。

        区分两种"无监听行"（run 40061050114 回归修复的关键）：
          - **blind=False**：至少一个 proc 文件读到了非空内容（rc=0）。此时
            rows 为空 = (a)"读到了内容但没有该端口的行" → 真的没 bind 上，
            调用方继续等到超时；
          - **blind=True**：两个文件都读不到内容（adb 报错非零退出 / 超时 /
            文件不存在 / 输出为空）= (b)"根本读不到内容"。可能是设备不允许
            读 /proc/net/udp（权限收紧的 ROM、SELinux 策略、定制镜像），
            此时"没有监听行"不能证明 nc 没 bind——调用方按降级判据处理。

        reason 是 blind 时的人类可读原因：每条 adb 命令的退出码与 stderr 都
        记录在案，足以区分"命令不存在"与"权限拒绝"；非 blind 时为 None。
        """
        rows = []
        errors = []
        readable = False
        for proc_file in UDP_PROC_FILES:
            try:
                # 用 shell() 而非 shell_text()：需要退出码与 stderr 才能定位
                # "读不到"的原因（Permission denied / No such file / ...）
                proc = self.adb.shell("cat %s" % proc_file, timeout=5,
                                      check=False)
            except EnvError as e:
                errors.append("%s：%s" % (proc_file, e))
                continue
            if proc.returncode != 0:
                stderr = proc.stderr.decode("utf-8", "replace").strip()
                errors.append("%s：cat 退出码 %d%s"
                              % (proc_file, proc.returncode,
                                 "（%s）" % stderr if stderr else ""))
                continue
            text = proc.stdout.decode("utf-8", "replace")
            if not text.strip():
                # rc=0 但输出为空：真实 /proc/net/udp 至少有表头行，视为读不到
                errors.append("%s：cat 退出码 0 但输出为空" % proc_file)
                continue
            readable = True
            rows.extend(parse_udp_listen_lines(text, self.media_port))
        blind = not readable
        reason = "；".join(errors) if errors else None
        self._last_probe_error = reason
        return rows, blind, reason

    def _kill_stale_nc(self):
        """尽力清掉设备内残留的 nc（覆盖两种候选命令形式的匹配）。"""
        for pattern in ("nc -u -l -p %d" % self.media_port,
                        "nc -ulp %d" % self.media_port):
            try:
                self.adb.shell("pkill -f '%s'" % pattern, timeout=5,
                               check=False)
            except EnvError:
                pass

    @staticmethod
    def _terminate(proc):
        """先礼后兵终结合适进程：TERM → 等 3s → KILL。"""
        try:
            proc.terminate()
        except OSError:
            pass
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                pass

    # ---- 数据泵：EOF 即 tap 死亡（B 项）----

    def _pump(self, proc, generation):
        # read1（单次语义）而非 read（凑满语义）：proc.stdout 是二进制管道上的
        # io.BufferedReader，read(n) 会阻塞直到凑满 n 字节或 EOF——tap 回传实测
        # 约 8.5KB/s，凑满 64KB 需约 7.7s，每轮阻塞都会拖过 C05 断言的 10s
        # 观察窗；推流速率一旦掉到 <6.5KB/s，C05 即假红（帧其实到了，只是没在
        # 窗口期内被读出）。read1 最多做一次底层 raw read，有数据即返回、不凑满，
        # 单次语义不会拖过观察窗。FormatCParser 本就是流式重分帧，短读无副作用。
        # getattr 回退：真实管道恒为 BufferedReader（必有 read1）；单测 mock 流
        # 只实现 read，回退让 mock 不因缺方法炸线程，生产语义不变。
        stream = proc.stdout
        read_chunk = getattr(stream, "read1", stream.read)
        while True:
            try:
                chunk = read_chunk(65536)
            except OSError as e:
                self._mark_dead("stdout 读取错误：%s" % e, proc, generation)
                return
            if not chunk:
                self._mark_dead("EOF（tap 进程退出）", proc, generation)
                return
            self.queue.put(chunk)

    def _mark_dead(self, reason, proc, generation):
        """线程安全地记录 tap 死亡：标志 + 退出码 + 原因（B 项）。

        - 旧世代（restart 前）的 pump 不上报：generation 不匹配直接丢弃，
          避免旧 tap 的死亡被算到新会话头上；
        - close() 主动杀的记录为"主动停止"，不置死亡标志、不报警；
        - 已意外死亡过的不重复上报。
        """
        if generation != self._generation or self._dead.is_set():
            return
        code = proc.poll()
        if code is None:
            # EOF 后 adb exec-out 可能还没退出：最多再等 1s 拿真实退出码
            try:
                code = proc.wait(timeout=1.0)
            except (subprocess.TimeoutExpired, OSError, ValueError):
                code = proc.poll()
        if self._stopping:
            self.death_reason = "主动停止（%s）" % reason
            self.death_exit_code = code
            return
        self.death_reason = reason
        self.death_exit_code = code
        self._dead.set()
        print("[TAP-DEAD] UDP tap 进程退出：%s（exit=%s）。若 app 侧 logcat 有"
              "'首个 UDP 包已发送'，这是 harness 问题（tap 启动竞态），不是 app 问题。"
              % (reason, fmt_exit_code(code)), flush=True)

    @property
    def dead(self):
        """tap 是否已意外死亡（EOF/读取错误，且非主动停止）。"""
        return self._dead.is_set()

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

    def restart(self, attempts=4):
        """换一个全新的设备内监听（重连会话 / C05 失败自救专用）。

        原理见类文档字符串：toybox nc 的 connect-to-first-peer 语义会让旧监听
        丢掉 app 第二会话（新 DatagramSocket、新源端口）的全部包；C05 红轮的
        已知根因（tap 启动竞态，run 40049236444）也靠换一个新 nc 来绕过——
        重启后必须是全新的、未 connect 的监听，绝不复用旧 socket。这里关旧起新，
        并把 parser/queue 一起换掉，杜绝把上个会话的残字节喂给新会话。

        调用时机：app 的 CONFIG 与 START 之间——那段时间 app 不发 UDP，
        更换监听零丢包风险。旧 nc 退出到端口释放可能有几百 ms 延迟，
        新监听起不来时自动重试（_start_process 内含 bind 就绪探测）。
        """
        self.close()
        last_err = None
        for _ in range(attempts):
            # 换干净的解析器/队列：杜绝把上个会话的残字节喂给新会话
            self.parser = FormatCParser()
            self.queue = queue.Queue()
            self.received = 0
            try:
                self._start_process()
                return
            except EnvError as e:
                last_err = e
                time.sleep(0.8)
        raise last_err if last_err else EnvError("UDP 监听重启失败")

    def close(self):
        self._stopping = True
        if self.proc is not None:
            self._terminate(self.proc)
            self.proc = None
        # 等旧读取线程自然退出（管道 EOF）：否则 restart 后两个线程会同时
        # 往 queue 里灌新旧会话的字节
        t = self._pump_thread
        if t is not None and t is not threading.current_thread():
            t.join(timeout=3)
        self._pump_thread = None
        # 兜底：确保设备侧 nc 也退出（覆盖两种命令形式的匹配）
        self._kill_stale_nc()


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


def describe_tap_health(tap):
    """一句话描述 tap 健康态（C05 失败信息定向用，B 项）。

    三种语义，杜绝旧信息"app 在跑但没发包"式误导（run 40049236444 教训）：
      - adb tap 已意外死亡/未运行 → harness 问题（tap 启动竞态），不是 app 问题；
      - adb tap 活着但没收到 → app 发包但 tap 未收到，指向 app 侧；
      - host tap 是 UDP socket，没有进程生命周期可言。
    """
    if getattr(tap, "MODE", None) != "adb":
        return "UDP tap 为 host socket 模式（无进程生命周期；收不到即链路/app 侧问题）"
    if getattr(tap, "proc", None) is None:
        return ("tap 未运行（设备内无 nc 进程）——若 app 侧 logcat 有"
                "'首个 UDP 包已发送'，这是 harness 问题（tap 启动竞态），"
                "不是 app 问题")
    if getattr(tap, "dead", False):
        return ("tap 进程已退出（exit=%s，%s）——若 app 侧 logcat 有"
                "'首个 UDP 包已发送'，这是 harness 问题（tap 启动竞态），"
                "不是 app 问题"
                % (fmt_exit_code(getattr(tap, "death_exit_code", None)),
                   getattr(tap, "death_reason", None) or "原因未知"))
    if getattr(tap, "bind_degraded", False):
        # 读不到 /proc/net/udp 时的降级就绪：进程活着，但端口未必真 bind 上了
        return ("tap 进程存活（降级就绪：设备读不到 /proc/net/udp，未验证端口"
                " %d 已 bind）但未收到数据 → app 发包但 tap 未收到：查 app 是否"
                "发包（logcat tag=MicCaptureService）；另注意本次 bind 就绪探测"
                "已降级，端口未必真的 bind 上" % tap.media_port)
    return ("tap 进程存活（端口 %d 已 bind）但未收到数据 → app 发包但 tap 未收到："
            "查 app 是否发包（logcat tag=MicCaptureService）" % tap.media_port)


def capture_c05_diagnostics(adb, media_port, results):
    """C05 判定失败时抓现场（**必须在 tap.close() 之前调用**）。

    背景（QA 2026-10-24 定性报告 §4 风险 1）：C05 红轮历来无法区分
    "app 没发包"还是"tap 没收到"——收集诊断工件时设备内 nc 进程已退出、
    app 侧也没有首包日志。本函数在杀死 tap 之前抓两样东西：

      1) `ps -A | grep -E '(^| )nc( |$)|nc -u'`：tap（设备内 toybox nc）进程
         是否还活着。**不用裸 `grep nc`**：incidentd/gnss-service/launcher3
         等系统进程名含 "nc" 子串，噪声会淹没真正的 nc（run 40049236444
         实证：ps 里"没有任何真正的 nc 进程"被一堆含 nc 子串的系统进程
         掩盖，把排查引向错误方向）；
      2) `cat /proc/net/udp` 与 `/proc/net/udp6`：媒体端口（16 进制，大小写
         不敏感）的监听行与队列计数——有监听行且 rx_queue 非零 = 包到达过
         内核但没人读（adb stdout 回传链路卡住）；无监听行 = tap 从未监听
         成功或已退出。

    与 app 侧"首个 UDP 包已发送"logcat（MicCaptureService）互补：
    C05 变红时，两相印证即可分钟级定性是 app 侧还是 tap 侧。
    全部为只读操作，单条失败只记录、不影响收尾流程。
    """
    results.info("---- C05 失败现场抓取（kill tap 之前，仅 adb tap 模式）----")

    # 1) tap 进程是否还活着（D 项：精确匹配，避开含 "nc" 子串的系统进程噪声）
    try:
        ps_out = adb.shell_text("ps -A | grep -E '(^| )nc( |$)|nc -u'",
                                timeout=10, check=False)
        ps_lines = filter_nc_ps_lines(ps_out)
        if ps_lines:
            for l in ps_lines:
                results.info("  ps: %s" % l)
        else:
            results.info("  ps: 设备内无 nc 进程（tap 未运行或已退出 → tap 侧问题）")
    except EnvError as e:
        results.info("  ps -A | grep -E '(^| )nc( |$)|nc -u' 失败：%s" % e)

    # 2) /proc/net/udp(6)：媒体端口监听行（local_address 列为 hex ip:port）
    for proc_file in UDP_PROC_FILES:
        try:
            udp_out = adb.shell_text("cat %s" % proc_file, timeout=10,
                                     check=False)
        except EnvError as e:
            results.info("  %s 读取失败：%s" % (proc_file, e))
            continue
        rows = parse_udp_listen_lines(udp_out, media_port)
        if rows:
            for _st, _rx, line in rows:
                results.info("  udp: %s" % line)
            results.info("  udp: 端口 %d（hex %s）在 %s 有监听行（st=%s）；"
                         "rx_queue 非零 = 包到过内核但无人读"
                         "（adb stdout 回传链路卡住）"
                         % (media_port, udp_port_hex(media_port), proc_file,
                            rows[0][0]))
        else:
            results.info("  udp: %s 无端口 %d（hex %s）的监听行"
                         "（tap 未监听成功或已退出）"
                         % (proc_file, media_port, udp_port_hex(media_port)))

    results.info("  判定参考：app 侧是否发包见 logcat '首个 UDP 包已发送'（tag=MicCaptureService）")


def c05_wait_first_packet(tap, adb, results, timeout, retries):
    """C05 首包等待，未收到时 restart tap 自救后重试（C 项）。

    背景（run 40049236444）：C05 红轮的已知根因是 tap 启动竞态——app 已
    发包（logcat "首个 UDP 包已发送"）但设备内无人监听媒体端口，包被内核
    丢弃。workflow 层的重试是重跑整个 pc_client.py（约 15s + 退避 5s）；
    更对症的做法是这里先 restart() 换一个**全新的、未 connect 的** nc 监听
    （toybox nc 的 connect-to-first-peer 语义要求必须是新 socket，见
    AdbUdpTap 文档字符串），再重试首包等待。app 此刻仍在持续发包（START 已
    发），新监听能直接收到；更换窗口内的丢包对"首包到达"断言无影响。
    workflow 层重试保留为双保险，不删除。

    返回 (pkt, healed)：healed=True 表示首包是 restart 之后才收到的——
    tap 启动竞态自愈的信号（harness 自愈，不是 app 行为变化）。
    """
    pkt = tap.read_packet(timeout=timeout)
    if pkt is not None:
        return pkt, False
    if getattr(tap, "MODE", None) != "adb" or retries <= 0:
        return None, False
    for attempt in range(1, retries + 1):
        results.info("C05 未收到首包（tap 健康态：%s）："
                     "第 %d/%d 次 restart tap 后重试"
                     % (describe_tap_health(tap), attempt, retries))
        # 先抓旧 tap 的现场（必须在 restart 杀它之前，否则 ps 看不到进程、
        # /proc/net/udp 看不到监听行）
        capture_c05_diagnostics(adb, tap.media_port, results)
        try:
            tap.restart()
        except EnvError as e:
            results.info("restart tap 失败（%s），停止 C05 自救" % e)
            return None, False
        pkt = tap.read_packet(timeout=timeout)
        if pkt is not None:
            results.info("C05 自救成功：restart tap 后收到首包——tap 启动竞态"
                         "自愈，非 app 修复（详见 README『已知故障模式与自愈』）")
            return pkt, True
    return None, False


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
    # bind 未就绪（10s 内 /proc/net/udp 无监听行）= 环境错误 EnvError（exit=2），
    # 不是断言失败——run 40049236444 的教训：tap 没起来时 C05 的红是假红，
    # 会把排查引向"app 没发包"的错误方向。
    try:
        if args.udp_tap == "host":
            tap = HostUdpTap(args.media_port, args.bind_host)
        else:
            tap = AdbUdpTap(adb, args.media_port)
    except OSError as e:
        raise EnvError("host UDP tap 绑定 %s:%d 失败：%s"
                       % (args.bind_host, args.media_port, e))
    except EnvError:
        # tap 起不来就别把 TCP 连接和心跳线程泄漏给进程退出
        heartbeats.stop()
        try:
            client.close()
        except OSError:
            pass
        raise
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
    # C 项：C05 未收到首包时先 restart tap 再重试（tap 启动竞态自救），
    # 比 workflow 层重跑整个脚本更对症更快；--skip-adb-setup 时没有可用
    # adb，不试（交给 workflow 层重试兜底）。
    c05_retries = 0 if args.skip_adb_setup else args.c05_tap_retries
    pkt, c05_self_healed = c05_wait_first_packet(
        tap, adb, results, args.first_packet_timeout, c05_retries)
    first_packet_failed = pkt is None
    if first_packet_failed:
        # 没有 UDP 流，后续断言全部失去意义：探一下 TCP 是否还活着，给出指向性诊断后收尾。
        # 注意：对端关闭时 recv_frame 抛 ConnectionClosed，必须兜住（否则整个脚本崩掉）。
        try:
            alive = client.recv_frame(timeout=1.0) is None
            state = "仍存活（app TCP 侧在线）" if alive else "已收到帧（异常）"
        except ConnectionClosed:
            state = "已断开（app 侧主动关闭：看门狗超时？或 app 报错退出，查 logcat）"
        # B 项：失败信息按 tap 健康态定向——tap 已死/未运行 = harness 问题
        # （tap 启动竞态）；tap 活着没收到 = app 发包但 tap 未收到。旧信息
        # "app 在跑但没发包"会把排查引向错误方向（run 40049236444 教训）。
        results.check("C05", "udp_first_packet_format_c", False,
                      "%ds 内未收到任何 UDP 包；TCP 控制通道%s；%s"
                      % (args.first_packet_timeout, state,
                         describe_tap_health(tap)))
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
        if c05_self_healed:
            results.info("注意：C05 首包是 restart tap 之后才收到的（tap 启动竞态自愈）——"
                         "这是 harness 自愈，不是 app 行为变化（详见 README『已知故障模式与自愈』）")

    if first_packet_failed:
        # C05 失败现场抓取：必须在 tap.close()（杀 nc）之前执行，否则 ps 看不到
        # tap 进程、/proc/net/udp 看不到监听行。与 app 侧"首个 UDP 包已发送"
        # logcat 互补，用于区分"app 没发包"与"tap 没收到"（QA 报告 §4 风险 1）。
        if tap.MODE == "adb" and not args.skip_adb_setup:
            capture_c05_diagnostics(adb, args.media_port, results)
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
    # 看门狗第一个检查点在连接后 5s；若 6s 之后仍在收包，说明心跳把它喂住了。
    # 之前用"最后一包距现在 < 2s"做判据会误判：nc 的 stdout 有缓冲，数据可能
    # 成批到达，最后一包的到达时刻不代表窗口末段没有流量（run 39470634964 实证）。
    late_count = 0
    while time.monotonic() < deadline:
        pkt = tap.read_packet(timeout=0.25)
        if pkt is None:
            continue
        tracker.observe(pkt)
        if (time.monotonic() - window_start) > (WATCHDOG_MS / 1000.0) + 1.0:
            late_count += 1
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
    # 看门狗生存：窗口跨过第 1 个检查点（5s）之后仍在持续收包 → 心跳喂住了看门狗
    alive_after_watchdog = (
        window_seconds > (WATCHDOG_MS / 1000.0) + 1.0 and late_count > 0
    )
    results.check("C11", "heartbeat_survives_watchdog", alive_after_watchdog,
                  "窗口 %.1fs > 看门狗 %ds；其后（>6s）又收到 %d 包，心跳已发 %d 个"
                  % (window_seconds, WATCHDOG_MS // 1000, late_count, heartbeats.sent))
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
        # 重连 = 全新会话，app 会起一个**新的**看门狗，因此心跳必须重新发，
        # 否则 5s 后又被断（run 39470634964 日志实证：重连后 5s 看门狗超时）。
        heartbeats2 = Heartbeats(client2, interval=args.heartbeat_interval)
        heartbeats2.start()
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
        # app 每个会话新建 DatagramSocket（新源端口），旧 nc 已 connect 到上个
        # 会话的源端口、收不到新会话的包（toybox netcat.c xconnect 语义；
        # run 39473353065 实证 C15 udp=none）。在 START 之前换一个新监听：
        # app 在 CONFIG→START 之间不发 UDP，此时更换零丢包风险。
        if tap.MODE == "adb":
            tap.restart()
            results.info("C15：已更换 UDP 监听以适配新会话源端口（toybox nc connect 语义）")
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
        results.check("C15", "reconnect_full_session", ok, "，".join(detail)
                      + ("，心跳 %d 个" % heartbeats2.sent))
        heartbeats2.stop()
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
    p.add_argument("--c05-tap-retries", type=int, default=2,
                   help="C05 未收到首包时，restart tap 后重试的次数"
                        "（tap 启动竞态自救；0=关闭，默认 2；不影响 workflow 层重试）")
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
    if args.c05_tap_retries < 0:
        p.error("--c05-tap-retries 不能为负")
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
