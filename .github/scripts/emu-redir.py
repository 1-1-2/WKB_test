#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""emu-redir —— Android 模拟器控制台 redir 助手（tailscale-dual TCP 通路兜底）。

背景：双机联调要求 app 看到的 TCP 对端是 10.0.2.2（slirp 网关），这样 app 的
UDP 媒体流才会发往 10.0.2.2 → host loopback（见 android/verify/integration/
README.md「直连拓扑」段）。首选方案是 host 直连模拟器 slirp 地址
10.0.2.15:8125；若该直连在 runner 上不通（QEMU slirp 对入站连接的隔离行为
与镜像/版本相关，历史上无定论），本脚本提供第二条路：

    redir add tcp:18125:8125
      → host 127.0.0.1:18125 的入站连接被注入模拟器 guest 的 10.0.2.15:8125；
      → 连接由 slirp 网关代为发起，guest 侧看到的对端仍是 10.0.2.2，
        app 的 UDP 目标不变，host loopback 回收的拓扑不变。

实现两条通道，按序尝试：
  1) `adb emu redir ...`（首选）：adb 控制通道免 console auth token；
  2) 裸 console（localhost:5554 + $HOME/.emulator_console_auth_token）兜底。

用法：
    python3 emu-redir.py --add tcp:18125:8125
    python3 emu-redir.py --del tcp:18125:8125
    python3 emu-redir.py --list
    python3 emu-redir.py --add tcp:18125:8125 --verify     # 增补后回读校验

退出码：0 = 成功（--verify 时含回读确认）；1 = 失败；2 = 用法错误。
"""
from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time

ADB = os.environ.get("ADB_BIN", "adb")
CONSOLE_HOST = "127.0.0.1"
CONSOLE_PORT = 5554          # 第一个模拟器实例的控制台端口
AUTH_TOKEN_PATH = os.path.expanduser("~/.emulator_console_auth_token")


def run_adb_emu(spec: str, timeout: float = 20.0) -> tuple[bool, str]:
    """走 `adb emu <spec>`；返回 (ok, 输出)。"""
    cmd = [ADB, "emu"] + spec.split()
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        return False, "找不到 %s（PATH 里没有 adb）" % ADB
    except subprocess.TimeoutExpired:
        return False, "adb emu 超时（%.0fs）：%s" % (timeout, spec)
    out = (proc.stdout + proc.stderr).decode("utf-8", "replace").strip()
    return proc.returncode == 0, out


def console_command(spec: str, timeout: float = 20.0) -> tuple[bool, str]:
    """裸控制台通道：连接 127.0.0.1:5554，必要时先 auth。"""
    try:
        token = open(AUTH_TOKEN_PATH, "r", encoding="utf-8").read().strip()
    except OSError as exc:
        return False, "读不到 console auth token（%s）：%s" % (AUTH_TOKEN_PATH, exc)
    if not token:
        return False, "console auth token 为空（%s）" % AUTH_TOKEN_PATH
    try:
        sock = socket.create_connection((CONSOLE_HOST, CONSOLE_PORT), timeout=5.0)
    except OSError as exc:
        return False, "连不上模拟器控制台 %s:%d：%s" % (CONSOLE_HOST, CONSOLE_PORT, exc)
    try:
        sock.settimeout(timeout)
        received = b""

        def drain(expect_more: float = 1.0) -> bytes:
            nonlocal received
            end = time.monotonic() + expect_more
            while time.monotonic() < end:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                received += chunk
                end = time.monotonic() + 0.3
            return received

        drain(1.0)
        sock.sendall(("auth %s\n" % token).encode("utf-8"))
        out = drain(2.0).decode("utf-8", "replace")
        if "OK" not in out:
            return False, "console auth 失败：%s" % out.strip()[:200]
        sock.sendall((spec + "\n").encode("utf-8"))
        out = drain(2.0).decode("utf-8", "replace")
        ok = "OK" in out
        return ok, out.strip()[:300]
    except OSError as exc:
        return False, "控制台会话错误：%s" % exc
    finally:
        try:
            sock.close()
        except OSError:
            pass


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Android 模拟器控制台 redir 助手")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--add", metavar="SPEC", help="新增映射，如 tcp:18125:8125")
    # 注意：--del 的 option 名合法，但 argparse 默认 dest 会推成 "del"（保留字），
    # 必须显式 dest="delete" 才能用属性访问。
    g.add_argument("--del", dest="delete", metavar="SPEC",
                   help="删除映射，如 tcp:18125:8125")
    g.add_argument("--list", action="store_true", help="列出现有映射")
    p.add_argument("--verify", action="store_true",
                   help="--add 后回读 redir list 确认映射存在")
    args = p.parse_args(argv)
    if (args.add or args.delete):
        spec = args.add or args.delete
        parts = spec.split(":")
        if len(parts) != 3 or parts[0] not in ("tcp", "udp") \
                or not parts[1].isdigit() or not parts[2].isdigit():
            p.error("映射格式须为 tcp:<hostPort>:<guestPort>（如 tcp:18125:8125）")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if args.list:
        ok, out = run_adb_emu("redir list")
        print("[emu-redir] adb emu redir list → ok=%s\n%s" % (ok, out), flush=True)
        return 0 if ok else 1

    action = "add" if args.add else "del"
    spec = args.add or args.delete
    cmd_spec = "redir %s %s" % (action, spec)

    ok, out = run_adb_emu(cmd_spec)
    print("[emu-redir] adb emu %s → ok=%s out=%s" % (cmd_spec, ok, out[:200]), flush=True)
    if not ok:
        print("[emu-redir] adb 通道失败，回退裸控制台：%s" % out[:200], flush=True)
        ok, out = console_command(cmd_spec)
        print("[emu-redir] console %s → ok=%s out=%s" % (cmd_spec, ok, out[:200]), flush=True)
        if not ok:
            print("[emu-redir] FAIL 两条通道都无法执行 %s：%s" % (cmd_spec, out), flush=True)
            return 1

    if args.add and args.verify:
        ok, out = run_adb_emu("redir list")
        host_port = spec.split(":")[1]
        present = host_port in out
        print("[emu-redir] 回读校验：hostPort %s %s（list 输出：%s）"
              % (host_port, "在映射中" if present else "不在映射中",
                 out.replace("\n", " | ")[:300]), flush=True)
        if not (ok and present):
            print("[emu-redir] FAIL 回读校验未确认映射生效", flush=True)
            return 1

    print("[emu-redir] OK %s" % cmd_spec, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
