#!/usr/bin/env python3
"""轮流用候选 IOCTL 灌 1kHz 正弦，同时 waveIn 录音，按时间窗归因哪个编号真的在灌。

时间线（每编号 1.5s，顺序灌）：
  t0.0-1.5  0x222004   t1.5-3.0  0x222008   t3.0-4.5  0x22200c
  t4.5-6.0  0x222010   t6.0-7.5  0x222014   t7.5-9.0  0x222000（对照，已知无效）
waveIn 全程录 10s（48000/16bit/mono）。逐窗算 RMS：信号出现 = 对应编号有效。
同时记录每个编号的 attempts/accepted/gle。
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
CHUNK = 1920          # 20ms mono 16bit
CODES = [0x222004, 0x222008, 0x22200c, 0x222010, 0x222014, 0x222000]
SEC_PER_CODE = 1.5
TOTAL_SEC = 10.5


def feeder(results, t0):
    h = k32.CreateFileW("\\\\.\\WOMic", GENERIC_READ | GENERIC_WRITE, 0, None,
                        OPEN_EXISTING, 0, None)
    if h in (None, INVALID_HANDLE_VALUE) or not h:
        results["open_error"] = ctypes.get_last_error()
        return
    phase = 0.0
    step = 2 * math.pi * 1000.0 / RATE
    for ci, code in enumerate(CODES):
        attempts = accepted = first_gle = 0
        start = t0 + ci * SEC_PER_CODE
        while time.time() < start + SEC_PER_CODE - 0.05:
            buf = ctypes.create_string_buffer(CHUNK)
            for i in range(CHUNK // 2):
                v = int(12000 * math.sin(phase))
                phase += step
                if phase > 2 * math.pi:
                    phase -= 2 * math.pi
                struct.pack_into("<h", buf, i * 2, v)
            ctypes.set_last_error(0)
            ok = k32.DeviceIoControl(h, code, ctypes.addressof(buf), CHUNK,
                                     None, 0, ctypes.byref(ctypes.c_ulong(0)), None)
            attempts += 1
            if ok:
                accepted += 1
            elif first_gle == 0:
                first_gle = ctypes.get_last_error()
            time.sleep(0.018)
        results["feed"][hex(code)] = {"attempts": attempts, "accepted": accepted,
                                      "first_gle": first_gle}
        print(f"feed {hex(code)}: attempts={attempts} accepted={accepted} "
              f"first_gle={first_gle}", flush=True)
    k32.CloseHandle(h)


def record(seconds):
    winmm.waveInGetNumDevs.restype = wintypes.UINT
    n = winmm.waveInGetNumDevs()
    print(f"waveIn devs: {n}", flush=True)
    fmt = WAVEFORMATEX(1, 1, RATE, RATE * 2, 2, 16, 0)
    hwi = ctypes.c_void_p()
    rc = winmm.waveInOpen(ctypes.byref(hwi), 0, ctypes.byref(fmt), 0, 0, 0)
    print(f"waveInOpen rc={rc}", flush=True)
    if rc != 0:
        return None
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
    return buf.raw[:got]


def main():
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    results = {"feed": {}}
    t0 = time.time() + 0.3
    th = threading.Thread(target=feeder, args=(results, t0), daemon=True)
    th.start()
    raw = record(TOTAL_SEC)
    th.join(timeout=15)

    out = {"feed": results["feed"], "windows": []}
    if raw:
        sec_bytes = RATE * 2
        for ci, code in enumerate(CODES):
            a = int((ci * SEC_PER_CODE) * sec_bytes) // 2 * 2
            b = int(((ci + 1) * SEC_PER_CODE) * sec_bytes) // 2 * 2
            seg = raw[a:min(b, len(raw))]
            if len(seg) < 2:
                continue
            n = len(seg) // 2
            samples = struct.unpack(f"<{n}h", seg[:n * 2])
            rms = math.sqrt(sum(x * x for x in samples) / n)
            peak = max(map(abs, samples))
            nonzero = sum(1 for x in samples if x != 0)
            out["windows"].append({"code": hex(code), "rms": round(rms, 1),
                                   "peak": peak, "nonzero": nonzero})
            print(f"win {hex(code)}: rms={rms:.1f} peak={peak} nonzero={nonzero}",
                  flush=True)
        with open("feed_scan_out.json", "w") as f:
            json.dump(out, f, indent=2)
    print("FEED_SCAN_DONE " + json.dumps(out), flush=True)


if __name__ == "__main__":
    main()
