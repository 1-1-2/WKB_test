#!/usr/bin/env python3
"""WO Mic 驱动探针：枚举 waveIn 设备 -> IOCTL 0x222000 灌 1kHz 正弦 -> waveIn 录音 -> Goertzel 判频。

校验链：
  1) waveIn 设备列表里存在 "WO Mic Device"（驱动已加载并注册 KS 捕获引脚）
  2) \\\\.\\WOMic 可打开，DeviceIoControl(0x222000) 接受 1920B/20ms 的 16bit PCM
  3) 录音有能量（RMS 显著大于静音底噪）
  4) 频谱主峰落在 1kHz 附近（±15%，容忍 48k/44.1k 重采样）

用法：python probe.py [--seconds 3] [--rate 48000]
退出码：0 全过；1 有失败（stdout 末尾打印 JSON 摘要）
"""
import argparse, ctypes, json, math, os, struct, sys, threading, time, wave
from ctypes import wintypes

winmm = ctypes.WinDLL("winmm")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

WAVE_MAPPER = wintypes.UINT(-1)
CALLBACK_NULL = 0x00000000
WHDR_DONE = 0x00000001
IOCTL_FEED = 0x222000
CHUNK = 1920          # 48k/20ms/16bit/mono


class WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("wFormatTag", wintypes.WORD), ("nChannels", wintypes.WORD),
                ("nSamplesPerSec", wintypes.DWORD), ("nAvgBytesPerSec", wintypes.DWORD),
                ("nBlockAlign", wintypes.WORD), ("wBitsPerSample", wintypes.WORD),
                ("cbSize", wintypes.WORD)]


class WAVEINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32), ("dwFormats", wintypes.DWORD),
                ("wChannels", wintypes.WORD), ("wReserved1", wintypes.WORD)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wintypes.DWORD),
                ("dwBytesRecorded", wintypes.DWORD), ("dwUser", ctypes.c_size_t),
                ("dwFlags", wintypes.DWORD), ("dwLoops", wintypes.DWORD),
                ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_size_t)]


winmm.waveInGetNumDevs.restype = wintypes.UINT
winmm.waveInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(WAVEINCAPSW), wintypes.UINT]
winmm.waveInOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), wintypes.UINT,
                             ctypes.POINTER(WAVEFORMATEX), ctypes.c_size_t, ctypes.c_size_t, wintypes.DWORD]
winmm.waveInPrepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]
winmm.waveInAddBuffer.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]
winmm.waveInStart.argtypes = [ctypes.c_void_p]
winmm.waveInStop.argtypes = [ctypes.c_void_p]
winmm.waveInReset.argtypes = [ctypes.c_void_p]
winmm.waveInClose.argtypes = [ctypes.c_void_p]
winmm.waveInUnprepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]

k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k32.CloseHandle.argtypes = [wintypes.HANDLE]

results = {"checks": [], "devices": [], "detail": {}}


def check(name, ok, info=""):
    results["checks"].append({"name": name, "ok": bool(ok), "info": str(info)[:400]})
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {info}")
    return ok


# ---------------------------------------------------------------- 1. 枚举设备
def enum_devices():
    n = winmm.waveInGetNumDevs()
    out = []
    for i in range(n):
        caps = WAVEINCAPSW()
        rc = winmm.waveInGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps))
        if rc == 0:
            out.append({"index": i, "name": caps.szPname, "channels": caps.wChannels,
                        "formats": hex(caps.dwFormats)})
    results["devices"] = out
    print(f"waveIn 设备数: {n}")
    for d in out:
        print(f"  [{d['index']}] {d['name']!r} ch={d['channels']} formats={d['formats']}")
    return out


def pick_device(devs):
    for d in devs:
        if "wo mic" in d["name"].lower():
            return d
    return None


# ---------------------------------------------------------------- 2. 灌音
def feeder(rate, stop_evt, stats):
    h = k32.CreateFileW("\\\\.\\WOMic", 0xC0000000, 0x3, None, 3, 0x80, None)
    if h == wintypes.HANDLE(-1).value or h is None:
        stats["error"] = f"CreateFile 失败 gle={ctypes.get_last_error()}"
        stats["opened"] = False
        return
    stats["opened"] = True
    n_samples = CHUNK // 2
    phase = 0.0
    step = 2 * math.pi * 1000.0 / rate
    sent = 0
    err = 0
    t0 = time.time()
    while not stop_evt.is_set():
        buf = bytearray(CHUNK)
        for i in range(n_samples):
            v = int(12000 * math.sin(phase))
            phase += step
            if phase > 2 * math.pi:
                phase -= 2 * math.pi
            struct.pack_into("<h", buf, i * 2, v)
        # 每包一次性喂入；METHOD_BUFFERED，输入缓冲 1920B
        ok = k32.DeviceIoControl(h, IOCTL_FEED, bytes(buf), CHUNK, None, 0,
                                 ctypes.byref(wintypes.DWORD(0)), None)
        if not ok:
            err += 1
            if err > 50:
                stats["error"] = f"DeviceIoControl 连续失败 gle={ctypes.get_last_error()}"
                break
        sent += 1
        time.sleep(0.02)
    stats.update(sent=sent, errors=err, seconds=round(time.time() - t0, 2))
    k32.CloseHandle(h)


# ---------------------------------------------------------------- 3. 录音
def record(dev_index, rate, seconds):
    fmt = WAVEFORMATEX(1, 1, rate, rate * 2, 2, 16, 0)
    hwi = ctypes.c_void_p()
    rc = winmm.waveInOpen(ctypes.byref(hwi), dev_index, ctypes.byref(fmt), 0, 0, CALLBACK_NULL)
    if rc != 0:
        return None, f"waveInOpen rc={rc}"
    nbytes = rate * 2 * seconds
    buf = ctypes.create_string_buffer(nbytes)
    hdr = WAVEHDR()
    hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
    hdr.dwBufferLength = nbytes
    winmm.waveInPrepareHeader(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInAddBuffer(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInStart(hwi)
    deadline = time.time() + seconds + 5
    while time.time() < deadline and not (hdr.dwFlags & WHDR_DONE):
        time.sleep(0.05)
    winmm.waveInStop(hwi)
    winmm.waveInReset(hwi)
    got = hdr.dwBytesRecorded
    winmm.waveInUnprepareHeader(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInClose(hwi)
    return buf.raw[:got], None


# ---------------------------------------------------------------- 4. 分析
def goertzel(samples, rate, freq):
    n = len(samples)
    k = int(0.5 + n * freq / rate)
    w = 2 * math.pi * k / n
    cw, sw = math.cos(w), math.sin(w)
    coeff = 2 * cw
    s0 = s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / n


def analyze(raw, rate):
    n = len(raw) // 2
    if n < rate // 2:
        return {"error": f"录音过短 {n} samples"}
    samples = list(struct.unpack(f"<{n}h", raw[:n * 2]))
    rms = math.sqrt(sum(x * x for x in samples) / n)
    peak = max(abs(x) for x in samples)
    seg = samples[: min(n, 16384)]
    mags = {f: goertzel(seg, rate, f) for f in range(200, 3001, 10)}
    best_f = max(mags, key=mags.get)
    best_m = mags[best_f]
    others = sorted((m for f, m in mags.items() if abs(f - best_f) > 60), reverse=True)
    floor = others[len(others) // 2] if others else 0.0
    snr = best_m / floor if floor else float("inf")
    return {"samples": n, "rms": round(rms, 1), "peak": peak,
            "peak_freq": best_f, "peak_mag": round(best_m, 1),
            "noise_floor": round(floor, 1), "snr": round(snr, 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=3)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--out", default="probe_out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    devs = enum_devices()
    dev = pick_device(devs)
    if not check("设备列表中存在 WO Mic 设备", dev is not None, dev or "未找到"):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1
    rate = a.rate
    fmt_bit = 0x2000 if rate == 48000 else 0x200
    if not (int(dev["formats"], 16) & fmt_bit):
        print(f"  设备未声明 {rate}Hz 单声道 16bit（formats={dev['formats']}），改用 44100")
        rate = 44100

    stop = threading.Event()
    stats = {}
    th = threading.Thread(target=feeder, args=(rate, stop, stats), daemon=True)
    th.start()
    time.sleep(1.0)
    if not check("\\\\.\\WOMic 可打开", stats.get("opened"), stats.get("error", "")):
        stop.set(); th.join(timeout=5)
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    raw, err = record(dev["index"], rate, a.seconds)
    stop.set(); th.join(timeout=10)
    results["detail"]["feeder"] = stats
    results["detail"]["rate"] = rate
    if not check("waveIn 录音成功", raw is not None and len(raw) > 0, err or f"{len(raw or b'')} bytes"):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    wav_path = os.path.join(a.out, "capture.wav")
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(raw)
    print(f"  已保存 {wav_path}")

    an = analyze(raw, rate)
    results["detail"]["analysis"] = an
    if "error" in an:
        check("频谱分析", False, an["error"])
    else:
        check("录音有能量（RMS>200）", an["rms"] > 200, f"rms={an['rms']} peak={an['peak']}")
        check("主峰在 1kHz ±15%", 850 <= an["peak_freq"] <= 1150,
              f"peak={an['peak_freq']}Hz snr={an['snr']}")
        check("主峰显著高于底噪（snr>5）", an["snr"] > 5, f"snr={an['snr']}")
        check("DeviceIoControl 无连续失败", stats.get("errors", 0) < 10, f"errors={stats.get('errors')}")

    ok = all(c["ok"] for c in results["checks"])
    print(json.dumps(results, ensure_ascii=False, indent=2))
    print(f"\n===== 结论: {'PASS' if ok else 'FAIL'} =====")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
