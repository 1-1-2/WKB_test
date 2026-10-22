#!/usr/bin/env python3
"""分析 Procmon CSV：WOMicClient.exe 的操作直方图 / TCP / 设备操作。"""
import csv
import io
import sys
from collections import Counter

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

hist = Counter()
tcp_rows = []
dev_rows = []
ioctl_rows = []
udp_rows = []

with open(r"D:\a\_temp\trace.csv", "r", encoding="utf-8-sig", errors="replace") as f:
    for row in csv.reader(f):
        if len(row) < 7:
            continue
        t, proc, pid, op, path, res, detail = row[0], row[1], row[2], row[3], row[4], row[5], row[6]
        if proc != "WOMicClient.exe":
            continue
        hist[op] += 1
        if op == "TCP":
            tcp_rows.append((t, op, path, res, detail))
        if op == "UDP":
            udp_rows.append((t, op, path, res, detail))
        if "WOMic" in path and not path.lower().endswith(".exe") and "extract" not in path:
            dev_rows.append((t, proc, op, path, res, detail))
        if op == "DeviceIoControl":
            ioctl_rows.append((t, op, path, res, detail))

print("=== 操作直方图 (WOMicClient.exe) ===")
for op, n in hist.most_common(25):
    print(f"{n:6d}  {op}")

print(f"\n=== TCP 事件 {len(tcp_rows)} 条（前后各 12 条）===")
for r in tcp_rows[:12] + (["..."] if len(tcp_rows) > 24 else []) + tcp_rows[-12:]:
    print(r)

print(f"\n=== UDP 事件 {len(udp_rows)} 条 ===")
for r in udp_rows[:12]:
    print(r)

print(f"\n=== WOMic 设备路径事件 {len(dev_rows)} 条 ===")
for r in dev_rows[:30]:
    print(r)

print(f"\n=== DeviceIoControl {len(ioctl_rows)} 条 ===")
for r in ioctl_rows[:30]:
    print(r)
