"""驱动行为矩阵实验（补齐此前未做的四个小实验）。

背景：灌音协议已通过静态反汇编还原并实测 PASS（rms=8485 / 1000Hz）。
本脚本回答四个"还能怎么用它"的问题，全部以采集端音频为判据：

  E1  0x22200C 语义：不同阶段读字节，判断它是不是状态查询
  E2  双句柄门控：门控是全局还是每句柄（决定客户端要不要强制单实例）
  E3  采样率：驱动是否只搬运字节（灌 44.1k 是否会变调）
  E4  容量档位：0x222014 设小容量能否工作（延迟 vs 稳健）

必须在 SYSTEM / Session 0 下运行（waveIn 需要）。
"""

import ctypes
import json
import math
import struct
import sys
import time
from ctypes import wintypes

IOCTL_FEED = 0x222001
IOCTL_GET_KEY = 0x222004
IOCTL_HANDSHAKE = 0x222008
IOCTL_STATUS = 0x22200C
IOCTL_START = 0x222010
IOCTL_SET_CAP = 0x222014

DEVICE = r"\\.\WOMic"
RATE = 48000


def force_utf8_output():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
winmm = ctypes.WinDLL("winmm")

k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k32.CloseHandle.argtypes = [wintypes.HANDLE]

winmm.waveInOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT,
                             ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD]
winmm.waveInStart.argtypes = [wintypes.HANDLE]
winmm.waveInStop.argtypes = [wintypes.HANDLE]
winmm.waveInReset.argtypes = [wintypes.HANDLE]
winmm.waveInClose.argtypes = [wintypes.HANDLE]
winmm.waveInPrepareHeader.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint]
winmm.waveInUnprepareHeader.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint]
winmm.waveInAddBuffer.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint]


class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [("wFormatTag", wintypes.WORD), ("nChannels", wintypes.WORD),
                ("nSamplesPerSec", wintypes.DWORD), ("nAvgBytesPerSec", wintypes.DWORD),
                ("nBlockAlign", wintypes.WORD), ("wBitsPerSample", wintypes.WORD),
                ("cbSize", wintypes.WORD)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wintypes.DWORD),
                ("dwBytesRecorded", wintypes.DWORD), ("dwUser", ctypes.c_void_p),
                ("dwFlags", wintypes.DWORD), ("dwLoops", wintypes.DWORD),
                ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_void_p)]


def open_dev():
    h = k32.CreateFileW(DEVICE, 0xC0000000, 0x3, None, 3, 0, None)
    return None if h in (-1, None) else h


def ioctl(h, code, inbuf=None, outlen=0):
    ret = wintypes.DWORD(0)
    out = ctypes.create_string_buffer(outlen) if outlen else None
    ib = ctypes.create_string_buffer(inbuf) if inbuf else None
    ctypes.set_last_error(0)
    ok = k32.DeviceIoControl(h, code, ib, len(inbuf) if inbuf else 0,
                             out, outlen, ctypes.byref(ret), None)
    raw = bytes(out.raw[:ret.value]) if (out is not None and ret.value) else b""
    return bool(ok), ctypes.get_last_error(), ret.value, raw


def handshake(h):
    ok, gle, _, key_raw = ioctl(h, IOCTL_GET_KEY, None, 1)
    key = key_raw[0] if key_raw else 0
    ok2, gle2, _, _ = ioctl(h, IOCTL_HANDSHAKE, bytes([key ^ 0x3B]), 1)
    return {"key": key, "get_ok": ok, "hs_ok": ok2, "hs_gle": gle2}


def status(h):
    ok, gle, n, raw = ioctl(h, IOCTL_STATUS, None, 1)
    return {"ok": ok, "gle": gle, "bytesReturned": n, "value": (raw[0] if raw else None)}


def set_cap(h, cap):
    ok, gle, _, _ = ioctl(h, IOCTL_SET_CAP, struct.pack(">H", cap), 0)
    return {"ok": ok, "gle": gle, "cap": cap}


def start(h, on=True):
    ok, gle, _, _ = ioctl(h, IOCTL_START, b"\x01" if on else b"\x00", 0)
    return {"ok": ok, "gle": gle}


def gen_chunk(freq, rate, nsamples, amp=12000):
    buf = bytearray(nsamples * 2)
    step = 2 * math.pi * freq / rate
    ph = 0.0
    for i in range(nsamples):
        v = int(amp * math.sin(ph))
        ph += step
        struct.pack_into("<h", buf, i * 2, v)
        if ph > 2 * math.pi:
            ph -= 2 * math.pi
    return bytes(buf)


def feed(h, seconds, freq=1000, rate=RATE, chunk_ms=20):
    """持续灌音 seconds 秒（20ms 一包）。"""
    nsamples = int(rate * chunk_ms / 1000)
    chunk = gen_chunk(freq, rate, nsamples)
    n = max(1, int(seconds * 1000 / chunk_ms))
    for _ in range(n):
        ioctl(h, IOCTL_FEED, chunk, 0)
        time.sleep(chunk_ms / 1000.0)


def do_record(seconds):
    """waveIn 录 seconds 秒，返回采样的 int 列表。"""
    fmt = WAVEFORMATEX(1, 1, RATE, RATE * 2, 2, 16, 0)
    h = wintypes.HANDLE()
    rc = winmm.waveInOpen(ctypes.byref(h), 0, ctypes.byref(fmt), 0, 0, 0)
    if rc != 0:
        return None, f"waveInOpen rc={rc}"
    nb = RATE * 2 * int(seconds)
    buf = ctypes.create_string_buffer(nb)
    hdr = WAVEHDR()
    hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
    hdr.dwBufferLength = nb
    winmm.waveInPrepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInAddBuffer(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInStart(h)
    deadline = time.time() + seconds + 8
    while time.time() < deadline and not (hdr.dwFlags & 1):
        time.sleep(0.05)
    winmm.waveInStop(h)
    winmm.waveInReset(h)
    got = hdr.dwBytesRecorded
    winmm.waveInUnprepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInClose(h)
    if got < 2:
        return None, f"recorded {got} bytes"
    s = struct.unpack("<%dh" % (got // 2), buf.raw[:got])
    return list(s), None


def goertzel(samples, freq, rate=RATE):
    k = int(0.5 + len(samples) * freq / rate)
    w = 2 * math.pi * k / len(samples)
    cw, sw2, c = math.cos(w), math.sin(w), 2 * math.cos(w)
    s0 = s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / len(samples)


def analyze(samples, probe_freqs=(600, 1000, 1088, 1500)):
    if not samples:
        return {"rms": 0}
    rms = math.sqrt(sum(x * x for x in samples) / len(samples))
    mags = {f: goertzel(samples, f) for f in probe_freqs}
    peak = max(mags, key=mags.get)
    return {"rms": round(rms, 2),
            "peak_freq": peak if rms > 50 else None,
            "mags": {f: round(v, 1) for f, v in mags.items()}}


def baseline():
    samples, err = do_record(2)
    return analyze(samples) if samples else {"error": err}


def exp_status():
    """E1：0x22200C 在各阶段的值。"""
    out = {}
    out["baseline"] = baseline()
    h = open_dev()
    if not h:
        return {"error": "CreateFile failed"}
    out["after_open"] = status(h)
    hs = handshake(h)
    out["handshake"] = hs
    out["after_handshake"] = status(h)
    out["set_cap"] = set_cap(h, 0x2D00)
    out["after_cap"] = status(h)
    out["start"] = start(h, True)
    out["after_start"] = status(h)
    feed(h, 1.0)
    out["after_feed"] = status(h)
    out["start_off"] = start(h, False)
    out["after_stop"] = status(h)
    k32.CloseHandle(h)
    return out


def exp_two_handles():
    """E2：门控是全局还是每句柄。"""
    out = {}
    out["baseline"] = baseline()
    h1 = open_dev()
    out["h1_handshake"] = handshake(h1)
    set_cap(h1, 0x2D00)
    start(h1, True)
    feed(h1, 1.0)
    s, _ = do_record(2)
    out["h1_recording"] = analyze(s)

    # 第二个句柄：它的 open 会不会清掉 h1 的门控
    h2 = open_dev()
    out["h2_opened"] = bool(h2)
    if h2:
        feed(h1, 1.0)
        s2, _ = do_record(2)
        out["h1_after_h2_open"] = analyze(s2)
        out["h2_handshake"] = handshake(h2)
        set_cap(h2, 0x2D00)
        start(h2, True)
        feed(h2, 1.0)
        s3, _ = do_record(2)
        out["h2_recording"] = analyze(s3)
        k32.CloseHandle(h2)
    k32.CloseHandle(h1)
    return out


def exp_sample_rate():
    """E3：驱动是否只搬运字节（喂 44.1k 会变调吗）。"""
    out = {}
    out["baseline"] = baseline()
    h = open_dev()
    if not h:
        return {"error": "CreateFile failed"}
    handshake(h)
    set_cap(h, 0x2D00)

    # 48k 基准
    start(h, True)
    feed(h, 1.5, freq=1000, rate=48000)
    s, _ = do_record(2)
    out["rate_48000"] = analyze(s, (900, 1000, 1100))
    start(h, False)

    # 44.1k 数据：若驱动只搬运，按 48k 播放理论值 = 1000 * 48/44.1 = 1088.4 Hz
    start(h, True)
    feed(h, 1.5, freq=1000, rate=44100)
    s, _ = do_record(2)
    out["rate_44100_theory_1088hz"] = analyze(s, (900, 1000, 1088, 1100))
    start(h, False)
    k32.CloseHandle(h)
    out["theory"] = "if 1088 dominates -> driver moves bytes, no resampling"
    return out


def exp_capacity():
    """E4：容量能否调小（延迟档位）。"""
    out = {}
    out["baseline"] = baseline()
    for cap in (0x2D00, 0x1680, 0x0A00, 0x0400):
        h = open_dev()
        if not h:
            out[hex(cap)] = {"error": "CreateFile failed"}
            continue
        handshake(h)
        set_cap(h, cap)
        start(h, True)
        feed(h, 1.5)
        s, _ = do_record(2)
        r = analyze(s)
        r["cap_bytes"] = cap
        r["cap_ms"] = round(cap / 2 / 48.0, 1)  # 16bit mono 48k -> 2 bytes/ms
        out[hex(cap)] = r
        start(h, False)
        k32.CloseHandle(h)
    return out


def main():
    force_utf8_output()
    results = {}
    for name, fn in (("E1_status_0x22200C", exp_status),
                     ("E2_two_handles", exp_two_handles),
                     ("E3_sample_rate", exp_sample_rate),
                     ("E4_capacity", exp_capacity)):
        # 每个实验独立输出并刷就可以
        print(f"===== {name} =====")
        sys.stdout.flush()
        try:
            results[name] = fn()
        except Exception as e:  # noqa: BLE001
            results[name] = {"exception": f"{type(e).__name__}: {e}"}
        print(json.dumps(results[name], ensure_ascii=False, indent=1))
        sys.stdout.flush()
        time.sleep(1)
    print("\n===== MATRIX_JSON =====")
    print(json.dumps(results, ensure_ascii=False))


if __name__ == "__main__":
    main()
