#!/usr/bin/env python3
"""按反汇编还原的协议灌音：0x222014 设容量 → 0x222010 启动 → 0x222001 灌裸 PCM。

时间线：waveIn 全程录 8s；前 2.5s 不灌（基线），后 5.5s 每 20ms 灌 1920B 1kHz 正弦。
分窗算 RMS：若后段 RMS 显著抬升 → 协议成立。
"""
import ctypes
import json
import math
import struct
import sys
import threading
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
winmm = ctypes.WinDLL("winmm")
GENERIC_READ, GENERIC_WRITE, OPEN_EXISTING = 0x80000000, 0x40000000, 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000

k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
k32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


class WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("wFormatTag", wintypes.WORD), ("nChannels", wintypes.WORD),
                ("nSamplesPerSec", wintypes.DWORD), ("nAvgBytesPerSec", wintypes.DWORD),
                ("nBlockAlign", wintypes.WORD), ("wBitsPerSample", wintypes.WORD),
                ("cbSize", wintypes.WORD)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wintypes.DWORD),
                ("dwBytesRecorded", wintypes.DWORD), ("dwUser", ctypes.c_size_t),
                ("dwFlags", wintypes.DWORD), ("dwLoops", wintypes.DWORD),
                ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_size_t)]


RATE = 48000
CHUNK = 1920
CAP = 0x2d00          # 11520，反汇编里的容量上限
TOTAL = 8.0
FEED_AFTER = 2.5

g_raw = bytearray()


def record(seconds):
    fmt = WAVEFORMATEX(1, 1, RATE, RATE * 2, 2, 16, 0)
    hwi = ctypes.c_void_p()
    rc = winmm.waveInOpen(ctypes.byref(hwi), 0, ctypes.byref(fmt), 0, 0, 0)
    print(f"waveInOpen rc={rc}", flush=True)
    if rc != 0:
        return
    nbytes = int(RATE * 2 * seconds)
    buf = ctypes.create_string_buffer(nbytes)
    hdr = WAVEHDR()
    hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
    hdr.dwBufferLength = nbytes
    winmm.waveInPrepareHeader(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInAddBuffer(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInStart(hwi)
    deadline = time.time() + seconds + 8
    while time.time() < deadline and not (hdr.dwFlags & 1):
        time.sleep(0.05)
    winmm.waveInStop(hwi)
    winmm.waveInReset(hwi)
    got = hdr.dwBytesRecorded
    winmm.waveInUnprepareHeader(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInClose(hwi)
    print(f"recorded {got} bytes", flush=True)
    g_raw.extend(buf.raw[:got])


def ioctl(h, code, inbuf, insize, outsize=0, tag=""):
    out = ctypes.create_string_buffer(max(outsize, 1))
    ret = ctypes.c_ulong(0)
    ctypes.set_last_error(0)
    ok = k32.DeviceIoControl(h, code, ctypes.addressof(inbuf) if inbuf else None, insize,
                             ctypes.addressof(out) if outsize else None, outsize,
                             ctypes.byref(ret), None)
    gle = ctypes.get_last_error()
    print(f"ioctl {hex(code)} {tag}: ok={ok} gle={gle} ret={ret.value}", flush=True)
    return ok, gle, ret.value


def main():
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    h = k32.CreateFileW("\\\\.\\WOMic", GENERIC_READ | GENERIC_WRITE, 0, None,
                        OPEN_EXISTING, 0, None)
    if h in (None, INVALID_HANDLE_VALUE) or not h:
        print(f"OPEN FAILED gle={ctypes.get_last_error()}", flush=True)
        sys.exit(1)
    print("device opened", flush=True)

    # 0) 握手：读 key（0x222004，1 字节输出），回送 key^0x3b（0x222008）
    keybuf = ctypes.create_string_buffer(1)
    ret = ctypes.c_ulong(0)
    ok = k32.DeviceIoControl(h, 0x222004, None, 0,
                             ctypes.addressof(keybuf), 1, ctypes.byref(ret), None)
    key = keybuf.raw[0]
    print(f"ioctl 0x222004 get-key: ok={ok} gle={ctypes.get_last_error()} "
          f"ret={ret.value} key={key:#04x}", flush=True)
    resp = ctypes.create_string_buffer(bytes([key ^ 0x3B]), 1)
    out1 = ctypes.create_string_buffer(1)
    ret2 = ctypes.c_ulong(0)
    ok2 = k32.DeviceIoControl(h, 0x222008, ctypes.addressof(resp), 1,
                              ctypes.addressof(out1), 1, ctypes.byref(ret2), None)
    print(f"ioctl 0x222008 handshake: ok={ok2} gle={ctypes.get_last_error()} "
          f"ret={ret2.value}", flush=True)

    # 1) 设容量（2 字节大端）
    cap = ctypes.create_string_buffer(struct.pack(">H", CAP), 2)
    ioctl(h, 0x222014, cap, 2, 0, f"set-cap {CAP}")

    # 2) 启动（1 字节 = 1）
    on = ctypes.create_string_buffer(b"\x01", 1)
    ioctl(h, 0x222010, on, 1, 0, "start")

    # 3) 录音线程先跑，基线 2.5s 后再灌
    th = threading.Thread(target=record, args=(TOTAL,), daemon=True)
    th.start()
    time.sleep(FEED_AFTER)

    phase = 0.0
    step = 2 * math.pi * 1000.0 / RATE
    attempts = accepted = 0
    first_gle = None
    t0 = time.time()
    while time.time() - t0 < TOTAL - FEED_AFTER - 0.3:
        buf = ctypes.create_string_buffer(CHUNK)
        for i in range(CHUNK // 2):
            v = int(12000 * math.sin(phase))
            phase += step
            if phase > 2 * math.pi:
                phase -= 2 * math.pi
            struct.pack_into("<h", buf, i * 2, v)
        ctypes.set_last_error(0)
        ret = ctypes.c_ulong(0)
        dummy = ctypes.create_string_buffer(CHUNK)
        ok = k32.DeviceIoControl(h, 0x222001, ctypes.addressof(buf), CHUNK,
                                 ctypes.addressof(dummy), CHUNK, ctypes.byref(ret), None)
        attempts += 1
        if ok:
            accepted += 1
        elif first_gle is None:
            first_gle = ctypes.get_last_error()
        time.sleep(0.02)
    print(f"feed: attempts={attempts} accepted={accepted} first_gle={first_gle}", flush=True)

    # 4) 停止
    off = ctypes.create_string_buffer(b"\x00", 1)
    ioctl(h, 0x222010, off, 1, 0, "stop")
    k32.CloseHandle(h)
    th.join(timeout=TOTAL + 10)

    # 5) 分窗
    out = {"feed": {"attempts": attempts, "accepted": accepted, "first_gle": first_gle},
           "windows": []}
    if len(g_raw) >= int(TOTAL * RATE * 2 * 0.8):
        for name, a, b in (("baseline", 0, 2), ("feeding", 3, 7)):
            seg = g_raw[int(a * RATE * 2):int(b * RATE * 2)]
            n = len(seg) // 2
            s = struct.unpack(f"<{n}h", seg[:n * 2])
            rms = math.sqrt(sum(x * x for x in s) / n)
            peak = max(map(abs, s))
            out["windows"].append({"name": name, "rms": round(rms, 1), "peak": peak})
            print(f"win {name}: rms={rms:.1f} peak={peak}", flush=True)
    print("RING_FEED_DONE " + json.dumps(out), flush=True)


if __name__ == "__main__":
    main()
