#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pc_client.py 离线单测（mock adb / mock Popen，不连设备、无第三方依赖）。

重点覆盖 C05 红轮根因（run 40049236444：harness 的 UDP tap 启动竞态）的
两个 CI 实证场景：

  场景1 "adb shell 活着但 nc 从未 bind"（旧实现 sleep(0.6)+poll() 会误判
        "启动成功"）：bind 就绪探测必须判失败（EnvError，exit=2），并把
        /proc/net/udp 的现场（端口 hex、有无监听行）带进错误信息；
  场景2 "nc bind 后立刻退出"（tap 静默死亡）：tap 死亡监控必须置位、记录
        退出码，且健康态描述明确指向"harness 问题，不是 app 问题"。

另覆盖：/proc/net/udp(6) 解析（大小写不敏感 / udp6 / 老内核分列 / rx_queue）、
ps 噪声过滤（D 项）、C05 restart 自救（C 项）、restart 清死亡状态。

运行：
    python3 test_pc_client.py            # 或 python3 -m unittest -v
"""

import contextlib
import io
import subprocess
import threading
import time
import unittest

import pc_client

# 媒体端口 5004 的 hex（内核 /proc/net/udp local_address 端口段）
PORT = 5004
PORT_HEX = "138C"

UDP_HEADER = ("  sl  local_address rem_address   st tx_queue:rx_queue"
              " tr:tm->when retrnsmt   uid  timeout inode")
# 只有 445(0x01BD)，没有 5004(0x138C)
UDP_NO_LINE = "\n".join([
    UDP_HEADER,
    "   0: 00000000:01BD 00000000:0000 07 00000000:00000000 00:00000000"
    "     0        0 12345 1 0000000000000000 100 0",
]) + "\n"
# 445 + 5004 监听行（st=07 LISTEN，rx_queue=0）
UDP_HAS_LINE = UDP_NO_LINE + (
    "   1: 00000000:138C 00000000:0000 07 00000000:00000000 00:00000000"
    "     0        0 12346 1 0000000000000000 100 0\n")
# 小写 hex + udp6 32 位地址 + rx_queue 非零（0x0A00 字节待读）
UDP6_LINE_LOWERCASE_RX = (
    "   2: 00000000000000000000000000000000:138c"
    " 00000000000000000000000000000000:0000 07 00000000:00000A00"
    " 00:00000000     0        0 12347 1 0000000000000000 100 0\n")
# 老内核格式：tx_queue rx_queue 分列
UDP_OLD_KERNEL = "\n".join([
    UDP_HEADER,
    "   0: 00000000:138C 00000000:0000 07 00000000 00000A00 00:00000000"
    "     0        0 12348 1 0000000000000000 100 0",
]) + "\n"

PS_NOISE = """USER           PID  PPID     VSZ    RSS WCHAN            ADDR S NAME
root            1     0    1234     567 0                   0 S init
system        812     1    9999    8888 0                   0 S incidentd
root          913     1    8888    7777 0                   0 S gnss-service
u0_a45        944     1    7777    6666 0                   0 S launcher3
root          955     1    6000    5000 0                   0 S nfc
root         1234  1000    4321    1234 0                   0 S nc
"""


# ---------------------------------------------------------------------------
# mock 基建
# ---------------------------------------------------------------------------

class FakeStdout:
    """mock proc.stdout：按脚本返回字节；可配置为阻塞（模拟活管道不 EOF）。

    - 脚本耗尽后默认返回 b""（EOF）并置 eof_seen（"bind 后立刻退出"场景）；
    - block_after=True 时脚本耗尽后阻塞在 stop 事件上（"nc 活着"场景），
      close() 前由测试置位 stop 放开，避免 pump 线程永久阻塞。
    """

    def __init__(self, script=None, block_after=False):
        self._script = list(script or [])
        self.block_after = block_after
        self.stop = threading.Event()
        self.eof_seen = False

    def read(self, n=-1):
        if self._script:
            return self._script.pop(0)
        if self.block_after:
            self.stop.wait()
            return b""
        self.eof_seen = True
        return b""


class FakePopen:
    """mock subprocess.Popen。

    behavior:
      "alive"          poll() 恒 None（adb shell/nc 进程存活）
      "dead"           poll() 恒为 exit_code（进程已退出）
      "die_after_read" stdout 读到 EOF 后进程才退出（"bind 成功后立刻退出"）
    """

    def __init__(self, behavior, exit_code=1, stdout=None):
        self.behavior = behavior
        self.exit_code = exit_code
        self.stdout = stdout if stdout is not None else FakeStdout()
        self._returncode = None

    def poll(self):
        if self._returncode is not None:
            return self._returncode
        if self.behavior == "dead":
            return self.exit_code
        if self.behavior == "die_after_read" and self.stdout.eof_seen:
            return self.exit_code
        return None

    def wait(self, timeout=None):
        code = self.poll()
        if code is None:
            raise subprocess.TimeoutExpired(cmd="fake", timeout=timeout or 0)
        return code

    def terminate(self):
        # 模拟"TERM 会关闭管道"：阻塞中的 read 会返回 EOF
        self.stdout.stop.set()
        if self.behavior != "alive":
            self._returncode = -15

    def kill(self):
        self._returncode = -9
        self.stdout.stop.set()


class MockAdb:
    """mock Adb：记录 shell 命令，按 provider 返回 /proc/net/udp 内容。

    udp_provider: callable(command, call_index) -> text；None 时一律返回 ""。
    events 与 _FakeTap 共享时可验证"先诊断后 restart"的调用次序。
    """

    def __init__(self, udp_provider=None, ps_text="", events=None):
        self.base = ["mock-adb"]
        self.path = "mock-adb"
        self.udp_provider = udp_provider
        self.ps_text = ps_text
        self.events = events if events is not None else []
        self.shell_calls = []

    def shell(self, command, timeout=120, check=True):
        self.shell_calls.append(command)
        self.events.append("shell:" + command)
        return None

    def shell_text(self, command, timeout=120, check=True):
        self.shell_calls.append(command)
        self.events.append("shell:" + command)
        if "/proc/net/udp" in command and self.udp_provider is not None:
            return self.udp_provider(command, len(self.shell_calls))
        if command.startswith("ps "):
            return self.ps_text
        return ""


@contextlib.contextmanager
def mock_popen_sequence(procs):
    """把 pc_client.subprocess.Popen 换成依次返回 procs 的假实现。"""
    it = iter(procs)
    created = []

    def fake_popen(*args, **kwargs):
        proc = next(it)
        created.append(proc)
        return proc

    orig = pc_client.subprocess.Popen
    pc_client.subprocess.Popen = fake_popen
    try:
        yield created
    finally:
        pc_client.subprocess.Popen = orig


def make_tap(adb, procs, bind_timeout=2.0, bind_poll_interval=0.02):
    """用 mock Popen 序列构造 AdbUdpTap（bind 超时缩短，测试跑得快）。"""
    with mock_popen_sequence(procs) as _:
        return pc_client.AdbUdpTap(adb, PORT, bind_timeout=bind_timeout,
                                   bind_poll_interval=bind_poll_interval)


def release_tap(tap):
    """收尾：close()（fake 的 terminate 会放开阻塞读，pump 随即退出）。"""
    tap.close()


class _FakeTap:
    """mock tap：read_packet 按剧本返回；restart 计数（C 项单测用）。"""

    def __init__(self, behavior, mode="adb", media_port=PORT, events=None):
        self.behavior = behavior
        self.MODE = mode
        self.media_port = media_port
        self.restart_calls = 0
        self.dead = False
        self.events = events

    def read_packet(self, timeout):
        if self.behavior == "dead_then_ok":
            return None if self.restart_calls == 0 else b"pkt"
        return None

    def restart(self):
        self.restart_calls += 1
        if self.events is not None:
            self.events.append("restart")
        if self.behavior == "restart_raises":
            raise pc_client.EnvError("mock restart 失败")


class _HostStub:
    MODE = "host"


# ---------------------------------------------------------------------------
# A 项：bind 就绪探测
# ---------------------------------------------------------------------------

class BindReadyProbeTest(unittest.TestCase):
    """A 项：/proc/net/udp 轮询探测 bind 就绪（run 40049236444 根因修复）。"""

    def test_shell_alive_but_nc_never_binds_fails(self):
        """场景1（CI 实证）：adb shell 进程活着，nc 从未 bind → 必须判失败。

        旧实现 sleep(0.6)+poll() is None 在这里会误判"启动成功"，随后 tap
        静默死亡、C05 红轮被误导向"app 没发包"。
        """
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_NO_LINE)
        with self.assertRaises(pc_client.EnvError) as ctx:
            make_tap(adb, [FakePopen("alive", stdout=FakeStdout(block_after=True)),
                           FakePopen("alive", stdout=FakeStdout(block_after=True))],
                     bind_timeout=0.3)
        msg = str(ctx.exception)
        self.assertIn("未就绪", msg)
        self.assertIn(PORT_HEX, msg)          # 诊断资产：端口 hex 进错误信息
        self.assertIn("/proc/net/udp", msg)   # 探测对象可追溯
        self.assertIn("toybox nc -u -l -p %d" % PORT, msg)  # 候选命令可追溯
        # 探测真的去读了 /proc/net/udp（不是只靠 sleep+poll）
        self.assertTrue(any("/proc/net/udp" in c for c in adb.shell_calls))

    def test_both_candidates_tried_then_env_error(self):
        """两个候选命令都起不来 → EnvError（exit=2 环境错误），且都尝试过。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_NO_LINE)
        procs = [FakePopen("dead", exit_code=1), FakePopen("dead", exit_code=1)]
        with self.assertRaises(pc_client.EnvError) as ctx:
            with mock_popen_sequence(procs):
                pc_client.AdbUdpTap(adb, PORT, bind_timeout=0.3,
                                    bind_poll_interval=0.02)
        msg = str(ctx.exception)
        self.assertIn("toybox nc -u -l -p %d" % PORT, msg)
        self.assertIn("toybox nc -ulp %d" % PORT, msg)
        self.assertIn("exit=2", msg)  # 明确告知是环境错误而非断言失败

    def test_start_succeeds_once_port_listening(self):
        """happy path：出现监听行 + nc 进程存活 → 启动成功。"""
        calls = {"n": 0}

        def provider(cmd, n):
            calls["n"] += 1
            return UDP_HAS_LINE if calls["n"] >= 3 else UDP_NO_LINE

        adb = MockAdb(udp_provider=provider)
        tap = make_tap(adb, [FakePopen("alive", stdout=FakeStdout(block_after=True))])
        try:
            self.assertIsNotNone(tap.proc)
            self.assertFalse(tap.dead)
            self.assertIsNone(tap.death_exit_code)
        finally:
            release_tap(tap)

    def test_dead_proc_rejected_even_with_stale_listen_line(self):
        """端口被别的进程占着（我们的 nc bind 失败退出）但 /proc/net/udp 有
        别人的监听行：不能假就绪——就绪判据要求 nc 进程也活着。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_HAS_LINE)
        with self.assertRaises(pc_client.EnvError):
            with mock_popen_sequence(
                    [FakePopen("dead", exit_code=1),
                     FakePopen("dead", exit_code=1)]):
                pc_client.AdbUdpTap(adb, PORT, bind_timeout=0.3,
                                    bind_poll_interval=0.02)


# ---------------------------------------------------------------------------
# B 项：tap 死亡监控与上报
# ---------------------------------------------------------------------------

class TapDeathMonitorTest(unittest.TestCase):
    """B 项：EOF → 死亡标志 + 退出码；失败信息定向 harness/app。"""

    def test_nc_dies_right_after_bind_reports_death(self):
        """场景2（CI 实证）：nc bind 成功后立刻退出 → EOF 路径必须置死亡
        标志、记录退出码，健康态描述明确指向 harness 问题。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_HAS_LINE)
        proc = FakePopen("die_after_read", exit_code=-9)
        tap = make_tap(adb, [proc])
        try:
            deadline = time.time() + 2.0
            while not tap.dead and time.time() < deadline:
                time.sleep(0.02)
            self.assertTrue(tap.dead, "tap EOF 后必须置死亡标志")
            self.assertEqual(tap.death_exit_code, -9)
            self.assertIn("EOF", tap.death_reason)
            summary = pc_client.describe_tap_health(tap)
            self.assertIn("tap 进程已退出", summary)
            self.assertIn("exit=", summary)
            self.assertIn("harness", summary)
            self.assertIn("不是 app 问题", summary)
        finally:
            release_tap(tap)

    def test_alive_tap_summary_points_to_app_side(self):
        """tap 活着但没收到 → 才说"app 发包但 tap 未收到"（指向 app 侧）。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_HAS_LINE)
        tap = make_tap(adb, [FakePopen("alive", stdout=FakeStdout(block_after=True))])
        try:
            summary = pc_client.describe_tap_health(tap)
            self.assertIn("app 发包但 tap 未收到", summary)
            self.assertNotIn("harness", summary)
        finally:
            release_tap(tap)

    def test_no_proc_summary_points_to_harness(self):
        """无 nc 进程（restart 失败后的状态）→ harness 问题定向。"""
        summary = pc_client.describe_tap_health(
            type("T", (), {"MODE": "adb", "media_port": PORT})())
        self.assertIn("tap 未运行", summary)
        self.assertIn("不是 app 问题", summary)

    def test_host_tap_summary(self):
        summary = pc_client.describe_tap_health(_HostStub())
        self.assertIn("host", summary)

    def test_close_does_not_report_unexpected_death(self):
        """close() 主动杀的不能算"意外死亡"（不置 dead、不报 TAP-DEAD）。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_HAS_LINE)
        tap = make_tap(adb, [FakePopen("alive", stdout=FakeStdout(block_after=True))])
        release_tap(tap)
        self.assertFalse(tap.dead)


# ---------------------------------------------------------------------------
# C 项：C05 失败 restart tap 再重试
# ---------------------------------------------------------------------------

class C05SelfHealTest(unittest.TestCase):
    """C 项：C05 未收到首包时 restart tap 自救（先诊断后重启）。"""

    def test_restart_then_packet_heals(self):
        events = []
        adb = MockAdb(events=events)
        tap = _FakeTap("dead_then_ok", events=events)
        results = pc_client.Results()
        pkt, healed = pc_client.c05_wait_first_packet(
            tap, adb, results, timeout=0.05, retries=2)
        self.assertTrue(healed)
        self.assertEqual(pkt, b"pkt")
        self.assertEqual(tap.restart_calls, 1)
        # 先抓现场再 restart（诊断必须在杀旧 tap 之前）
        self.assertIn("restart", events)
        self.assertLess(events.index("shell:ps -A | grep -E '(^| )nc( |$)|nc -u'"),
                        events.index("restart"))

    def test_no_heal_after_all_retries(self):
        adb = MockAdb()
        tap = _FakeTap("always_none")
        results = pc_client.Results()
        pkt, healed = pc_client.c05_wait_first_packet(
            tap, adb, results, timeout=0.05, retries=2)
        self.assertFalse(healed)
        self.assertIsNone(pkt)
        self.assertEqual(tap.restart_calls, 2)

    def test_host_tap_not_restarted(self):
        adb = MockAdb()
        tap = _FakeTap("always_none", mode="host")
        results = pc_client.Results()
        pkt, healed = pc_client.c05_wait_first_packet(
            tap, adb, results, timeout=0.05, retries=2)
        self.assertFalse(healed)
        self.assertEqual(tap.restart_calls, 0)

    def test_restart_failure_breaks_loop(self):
        adb = MockAdb()
        tap = _FakeTap("restart_raises")
        results = pc_client.Results()
        pkt, healed = pc_client.c05_wait_first_packet(
            tap, adb, results, timeout=0.05, retries=2)
        self.assertFalse(healed)
        self.assertIsNone(pkt)
        self.assertEqual(tap.restart_calls, 1)  # 第一次抛错即停，不浪费重试

    def test_zero_retries_no_restart(self):
        adb = MockAdb()
        tap = _FakeTap("always_none")
        results = pc_client.Results()
        pkt, healed = pc_client.c05_wait_first_packet(
            tap, adb, results, timeout=0.05, retries=0)
        self.assertFalse(healed)
        self.assertEqual(tap.restart_calls, 0)

    def test_restart_after_death_clears_flag(self):
        """restart 换全新监听后，死亡标志/退出码被清空（自愈后可继续断言）。"""
        adb = MockAdb(udp_provider=lambda cmd, n: UDP_HAS_LINE)
        procs = [FakePopen("die_after_read", exit_code=1),
                 FakePopen("alive", stdout=FakeStdout(block_after=True))]
        tap = make_tap(adb, procs)
        try:
            deadline = time.time() + 2.0
            while not tap.dead and time.time() < deadline:
                time.sleep(0.02)
            self.assertTrue(tap.dead)
            with mock_popen_sequence([procs[1]]):
                tap.restart()
            self.assertFalse(tap.dead)
            self.assertIsNone(tap.death_exit_code)
            self.assertIs(procs[1], tap.proc)  # 全新的未 connect 监听
        finally:
            release_tap(tap)


# ---------------------------------------------------------------------------
# D 项：/proc/net/udp 解析与 ps 噪声过滤
# ---------------------------------------------------------------------------

class UdpProcParsingTest(unittest.TestCase):

    def test_port_hex(self):
        self.assertEqual(pc_client.udp_port_hex(5004), "138C")
        self.assertEqual(pc_client.udp_port_hex(8125), "1FBD")

    def test_parse_listen_line(self):
        rows = pc_client.parse_udp_listen_lines(UDP_HAS_LINE, PORT)
        self.assertEqual(len(rows), 1)
        st, rx, line = rows[0]
        self.assertEqual(st, "07")
        self.assertEqual(rx, 0)
        self.assertIn(PORT_HEX, line)

    def test_parse_ignores_other_ports_and_header(self):
        self.assertEqual(pc_client.parse_udp_listen_lines(UDP_NO_LINE, PORT), [])

    def test_parse_case_insensitive_and_udp6(self):
        rows = pc_client.parse_udp_listen_lines(
            UDP_HAS_LINE + UDP6_LINE_LOWERCASE_RX, PORT)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[1][1], 0x0A00)  # 小写 hex + rx_queue 非零

    def test_parse_old_kernel_split_columns(self):
        rows = pc_client.parse_udp_listen_lines(UDP_OLD_KERNEL, PORT)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][1], 0x0A00)

    def test_parse_garbage_safe(self):
        self.assertEqual(pc_client.parse_udp_listen_lines("garbage\n\n", PORT), [])


class PsNoiseFilterTest(unittest.TestCase):
    """D 项：裸 grep nc 的噪声（incidentd/gnss-service/launcher3…）必须滤掉。"""

    def test_real_nc_survives_noise(self):
        rows = pc_client.filter_nc_ps_lines(PS_NOISE)
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0].endswith(" nc"))

    def test_grep_self_and_substring_noise_filtered(self):
        out = PS_NOISE + (
            "root 9999 1 100 100 0 0 R grep -E (^| )nc( |$)|nc -u\n"
            "root 8888 1 100 100 0 0 S nc -u -l -p 5004\n")
        rows = pc_client.filter_nc_ps_lines(out)
        self.assertEqual(len(rows), 2)  # 真 nc + 参数形态的 nc；grep 自身被滤掉

    def test_no_nc_returns_empty(self):
        self.assertEqual(pc_client.filter_nc_ps_lines(
            "root 1 0 1 1 0 0 S init\nsystem 2 1 1 1 0 0 S incidentd\n"), [])


class CaptureDiagnosticsTest(unittest.TestCase):
    """D 项：capture_c05_diagnostics 用精确 ps 匹配 + udp/udp6 双查。"""

    def test_ps_and_udp_reported(self):
        adb = MockAdb(ps_text=PS_NOISE,
                      udp_provider=lambda cmd, n: UDP_HAS_LINE)
        results = pc_client.Results()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            pc_client.capture_c05_diagnostics(adb, PORT, results)
        out = buf.getvalue()
        self.assertIn("ps: root         1234  1000    4321    1234 0                   0 S nc",
                      out)                      # 真 nc 进程行在
        self.assertNotIn("incidentd", out)      # 含 nc 子串的噪声不在
        self.assertNotIn("launcher3", out)
        self.assertIn("/proc/net/udp", out)     # udp6 也查了
        self.assertIn(PORT_HEX, out)
        self.assertIn("有监听行", out)


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------

class FmtExitCodeTest(unittest.TestCase):

    def test_fmt_exit_code(self):
        self.assertEqual(pc_client.fmt_exit_code(None), "未知")
        self.assertEqual(pc_client.fmt_exit_code(1), "1")
        self.assertIn("信号 15", pc_client.fmt_exit_code(-15))


if __name__ == "__main__":
    unittest.main(verbosity=2)
