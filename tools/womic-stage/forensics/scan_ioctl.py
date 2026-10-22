#!/usr/bin/env python3
"""\\\.\\WOMic IOCTL 全表面扫描。

原理：内核驱动对未实现的 IOCTL 返回 ERROR_INVALID_FUNCTION(1)。
对已实现但参数不符的，返回 ERROR_INVALID_PARAMETER(87) 等；
对完全接受的返回 TRUE。扫描后即可圈出驱动的真实 IOCTL 面。

扫描域：
  pass1: dev_type=0x22(FILE_DEVICE_UNKNOWN)，func=0x000..0xFFF，
         method∈{BUFFERED,IN_DIRECT,OUT_DIRECT}，access=ANY，输入 1920B
  pass2: 同域但**无输入缓冲**（捕获只读/被动 IOCTL）
  pass3: func=0x800，dev_type=0x00..0xFF，method=0，access∈{0,1,2}
输出：所有 gle != 1 的 (code, ok, gle, bytesReturned)，存 JSON。
"""
import ctypes
import json
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
k32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]

h = k32.CreateFileW("\\\\.\\WOMic", GENERIC_READ | GENERIC_WRITE, 0, None,
                    OPEN_EXISTING, 0, None)
if h in (None, INVALID_HANDLE_VALUE) or not h:
    print(json.dumps({"error": "open_failed", "gle": ctypes.get_last_error()}))
    sys.exit(1)

inbuf = ctypes.create_string_buffer(1920)
outbuf = ctypes.create_string_buffer(4096)
ret = ctypes.c_ulong(0)


def probe(code, use_input=True):
    ctypes.set_last_error(0)
    ok = k32.DeviceIoControl(h, code, ctypes.addressof(inbuf) if use_input else None,
                             1920 if use_input else 0,
                             ctypes.addressof(outbuf), 4096, ctypes.byref(ret), None)
    return {"ok": bool(ok), "gle": ctypes.get_last_error(), "ret": ret.value}


def ctl(dt, fn, method, access):
    return (dt << 16) | (access << 14) | (fn << 2) | method


hits = {}

# pass1+2：dev=0x22，func 全域，method 三种，access=ANY
for method in (0, 1, 2):
    for fn in range(0x1000):
        code = ctl(0x22, fn, method, 0)
        for use_input in (True, False):
            r = probe(code, use_input)
            if r["ok"] or r["gle"] != 1:
                key = f"{code:#x}{'/noIn' if not use_input else ''}"
                hits[key] = r

# pass3：func=0x800，扫 dev_type 与 access
for dt in range(0x100):
    for access in (0, 1, 2):
        code = ctl(dt, 0x800, 0, access)
        if f"{code:#x}" in hits:
            continue
        r = probe(code)
        if r["ok"] or r["gle"] != 1:
            hits[f"{code:#x}"] = r

k32.CloseHandle(h)
print(json.dumps({"total_probed": (0x1000 * 3 * 2 + 0x100 * 3),
                  "hits": hits}, indent=2))
