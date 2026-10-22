#!/usr/bin/env python3
"""验证 PCM 灌入通道：WriteFile vs DeviceIoControl，时间窗归因。

时间线（waveIn 全程录 9s）：
  窗1 (0-3s):   WriteFile 1920B/20ms 正弦
  窗2 (3-6s):   WriteFile 1924B（前置 4B 序号头的猜测变体）
  窗3 (6-9s):   DeviceIoControl 0x222008 1920B（对照，预期无效）
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
k32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong,
                          ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
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
SEC = 3.0


def make_chunk(phase, n_samples, hdr4=None):
    buf = bytearray(CHUNK if hdr4 is None else CHUNK + 4)
    off = 4 if hdr4 is not None else 0
    for i in range(n_samples):
        v = int(12000 * math.sin(phase))
        phase += 2 * math.pi * 1000.0 / RATE
        if phase > 2 * math.pi:
            phase -= 2 * math.pi
        struct.pack_into("<h", buf, off + i * 2, v)
    if hdr4 is not None:
        struct.pack_into("<I", buf, 0, hdr4)
    return bytes(buf), phase


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
    print("opened \\\\.\\WOMic", flush=True)

    t0 = time.time() + 0.3
    stats = []

    def phase_write(duration, hdr4_seq=False, tag=""):
        attempts = accepted = first_gle = 0
        phase = 0.0
        seq = 0
        while time.time() < t0 + duration:
            chunk, phase = make_chunk(phase, CHUNK // 2,
                                      hdr4=(seq if hdr4_seq else None))
            written = ctypes.c_ulong(0)
            ctypes.set_last_error(0)
            ok = k32.WriteFile(h, chunk, len(chunk), ctypes.byref(written), None)
            attempts += 1
            if ok:
                accepted += 1
            elif first_gle == 0:
                first_gle = ctypes.get_last_error()
            seq += 1
            time.sleep(0.018)
        stats.append({"phase": tag, "attempts": attempts, "accepted": accepted,
                      "first_gle": first_gle})
        print(f"{tag}: attempts={attempts} accepted={accepted} first_gle={first_gle}",
              flush=True)

    th = threading.Thread(target=record, args=(9.0,), daemon=True)

    # 录音线程先起，再按时段灌
    th.start()
    time.sleep(t0 - time.time())

    phase_write(t0 + SEC, hdr4_seq=False, tag="writefile-raw1920")
    phase_write(t0 + 2 * SEC, hdr4_seq=True, tag="writefile-hdr4-1924")
    # 对照：DeviceIoControl
    attempts = accepted = first_gle = 0
    phase = 0.0
    while time.time() < t0 + 3 * SEC:
        chunk, phase = make_chunk(phase, CHUNK // 2)
        ctypes.set_last_error(0)
        ok = k32.DeviceIoControl(h, 0x222008, chunk, len(chunk), None, 0,
                                 ctypes.byref(ctypes.c_ulong(0)), None)
        attempts += 1
        if ok:
            accepted += 1
        elif first_gle == 0:
            first_gle = ctypes.get_last_error()
        time.sleep(0.018)
    stats.append({"phase": "ioctl-0x222008", "attempts": attempts,
                  "accepted": accepted, "first_gle": first_gle})
    print(f"ioctl-0x222008: attempts={attempts} accepted={accepted} "
          f"first_gle={first_gle}", flush=True)

    k32.CloseHandle(h)
    time.sleep(9.5)   # 等录音线程收尾（record 自己打日志）

    # 按时间窗归因 RMS（各窗 3s，与灌音阶段对齐）
    out = {"stats": stats, "windows": []}
    if len(g_raw) >= 9 * RATE * 2:
        names = ["writefile-raw1920", "writefile-hdr4-1924", "ioctl-0x222008"]
        for i, name in enumerate(names):
            a = i * 3 * RATE * 2
            b = (i + 1) * 3 * RATE * 2
            seg = g_raw[a:b]
            n = len(seg) // 2
            samples = struct.unpack(f"<{n}h", seg[:n * 2])
            rms = math.sqrt(sum(x * x for x in samples) / n)
            peak = max(map(abs, samples))
            nz = sum(1 for x in samples if x != 0)
            out["windows"].append({"phase": name, "rms": round(rms, 1),
                                   "peak": peak, "nonzero": nz})
            print(f"win {name}: rms={rms:.1f} peak={peak} nonzero={nz}", flush=True)
    print("FEED_WRITE_DONE " + json.dumps(out), flush=True)


if __name__ == "__main__":
    main()
