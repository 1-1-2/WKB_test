#!/usr/bin/env python3
"""在 WOMicClient.exe 里找 'Wolicheng' 注册表串附近的 UTF-16 字符串，推断配置值名。"""
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
data = open(r"D:\a\_temp\extract\WOMicClient.exe", "rb").read()

# 找所有 Wolicheng（UTF-16LE 和 ASCII）
hits = []
for enc, pat in (("u16", "Wolicheng".encode("utf-16-le")),
                 ("a", b"Wolicheng")):
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        hits.append((enc, i))
        start = i + 1

print(f"hits: {len(hits)}")
for enc, off in hits:
    lo = max(0, off - 800)
    hi = min(len(data), off + 800)
    blob = data[lo:hi]
    print(f"\n===== {enc} @ {hex(off)} =====")
    if enc == "u16":
        # 提取周围 UTF-16 可打印串
        strs = re.findall(rb"(?:[\x20-\x7e]\x00){3,}", blob)
    else:
        strs = re.findall(rb"[\x20-\x7e]{4,}", blob)
    for s in strs:
        try:
            t = s.decode("utf-16-le") if enc == "u16" else s.decode()
        except Exception:  # noqa: BLE001
            continue
        print(f"  {t!r}")
