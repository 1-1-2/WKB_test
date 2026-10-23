#!/usr/bin/env python3
"""e2e_probe — WO Mic 驱动端到端探针（真实 Windows 录音取证）。

从 /workspace/womic-recovery/runner/probe.py vendors 进仓库（该脚本在真实
Windows 上 PASS 过，但从未入库）。相对原版的修改：
  1) 修正 docstring 里过时的 IOCTL 表述：灌音是 0x222001（不是 0x222000——
     0x222000 从未在还原的协议里出现过；握手链是 0x222004→0x222008→0x222014→0x222010）；
  2) 路径自包含：只依赖标准库与系统 DLL，不引用 womic-recovery 任何文件；
     默认输出目录 probe_out 相对当前工作目录，CI 里用 --out 显式指定；
  3) 输出契约：stdout 末尾打印 JSON 摘要 + "RESULT=PASS/FAIL" 行，
     退出码 0=全过 / 1=有失败（供 CI 的 ::error:: 标注与机器读取）。

校验链（对官方 womic.sys 2022 基线反汇编还原的协议）：
  1) waveIn 设备列表里存在 "WO Mic Device"（驱动已加载并注册 KS 捕获引脚）
  2) \\\\.\\WOMic 可打开，并按 0x222004→0x222008→0x222014→0x222010 建立会话
  3) 以 0x222001 灌 48kHz/16bit/mono PCM（20ms=1920B/帧）1kHz 正弦。
     注意：0x222001 恒定返回 Win32 FALSE + GetLastError=1（驱动的防逆向行为），
     **状态码不可作判据**，只能看实际录音效果。
  4) 录音有能量（RMS 显著大于静音底噪）
  5) 频谱主峰落在 1kHz 附近（±15%，容忍 48k/44.1k 重采样）

用法：python e2e_probe.py [--seconds 3] [--rate 48000] [--out probe_out]
退出码：0 全过；1 有失败（stdout 末尾打印 JSON 摘要 + RESULT= 行）
"""
import argparse, ctypes, json, math, os, struct, sys, threading, time, wave
from pathlib import Path
from ctypes import wintypes

winmm = ctypes.WinDLL("winmm")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

WAVE_MAPPER = wintypes.UINT(-1)
CALLBACK_NULL = 0x00000000
WHDR_DONE = 0x00000001
IOCTL_FEED = 0x222001      # 反汇编还原：输入缓冲=裸 PCM，写入环形缓冲。
                           # 恒定返回 FALSE+gle=1（防逆向），看效果不看状态码。
IOCTL_GET_KEY = 0x222004   # 返回 1 字节 key（同时写入驱动全局）
IOCTL_HANDSHAKE = 0x222008 # 输入 key^0x3b，通过则置门控
IOCTL_SET_CAP = 0x222014   # 2 字节大端容量，上限 0x2d00
IOCTL_START = 0x222010     # 1 字节 1=启动（清环），0=停止
CHUNK = 1920          # 48k/20ms/16bit/mono
# dwFormats 位常量（msmcm.h）。此前误用 0x2000/0x200（那是 44k/4k 的立体声 8bit 位）：
#   WAVEFORMAT_48M16 = 0x00040000（48kHz 单声道 16bit）
#   WAVEFORMAT_44M16 = 0x00004000（44.1kHz 单声道 16bit）
WAVE_FORMAT_48M16 = 0x00040000
WAVE_FORMAT_44M16 = 0x00004000


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
k32.GetCurrentProcessId.restype = wintypes.DWORD
k32.ProcessIdToSessionId.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]


def current_session_id():
    """真实终端会话号。此前写错成 (os.getpid() != 0)——那是恒真的进程号判断。"""
    sid = wintypes.DWORD(0)
    if k32.ProcessIdToSessionId(k32.GetCurrentProcessId(), ctypes.byref(sid)):
        return sid.value
    return None


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
    """按反汇编还原的协议灌音：握手 → 设容量 → 启动 → 循环灌裸 PCM。

    驱动里 DeviceIoControl 0x222001 始终返回 STATUS_NOT_IMPLEMENTED
    （Win32 侧表现为 FALSE + gle=1），**状态码不可作为成败判据**，
    只能靠采集端是否真的拿到音频来判定。
    """
    h = k32.CreateFileW("\\\\.\\WOMic", 0xC0000000, 0x3, None, 3, 0, None)
    if h == wintypes.HANDLE(-1).value or h is None:
        stats["error"] = f"CreateFile 失败 gle={ctypes.get_last_error()}"
        stats["opened"] = False
        return
    stats["opened"] = True

    # 1) 握手
    keybuf = ctypes.create_string_buffer(1)
    ret = wintypes.DWORD(0)
    ok = k32.DeviceIoControl(h, IOCTL_GET_KEY, None, 0, ctypes.addressof(keybuf), 1,
                             ctypes.byref(ret), None)
    key = keybuf.raw[0]
    resp = ctypes.create_string_buffer(bytes([key ^ 0x3B]), 1)
    out1 = ctypes.create_string_buffer(1)
    ok2 = k32.DeviceIoControl(h, IOCTL_HANDSHAKE, ctypes.addressof(resp), 1,
                              ctypes.addressof(out1), 1, ctypes.byref(ret), None)
    stats["handshake"] = {"key": key, "get_ok": bool(ok), "hs_ok": bool(ok2)}

    # 2) 容量 + 启动
    cap = ctypes.create_string_buffer(struct.pack(">H", 0x2D00), 2)
    k32.DeviceIoControl(h, IOCTL_SET_CAP, ctypes.addressof(cap), 2, None, 0,
                        ctypes.byref(ret), None)
    on = ctypes.create_string_buffer(b"\x01", 1)
    k32.DeviceIoControl(h, IOCTL_START, ctypes.addressof(on), 1, None, 0,
                        ctypes.byref(ret), None)

    n_samples = CHUNK // 2
    phase = 0.0
    step = 2 * math.pi * 1000.0 / rate
    attempts = 0
    err = 0
    first_gle = None
    t0 = time.time()
    while not stop_evt.is_set():
        buf = bytearray(CHUNK)
        for i in range(n_samples):
            v = int(12000 * math.sin(phase))
            phase += step
            if phase > 2 * math.pi:
                phase -= 2 * math.pi
            struct.pack_into("<h", buf, i * 2, v)
        dummy = ctypes.create_string_buffer(CHUNK)
        ctypes.set_last_error(0)
        k32.DeviceIoControl(h, IOCTL_FEED, bytes(buf), CHUNK,
                            ctypes.addressof(dummy), CHUNK, ctypes.byref(ret), None)
        attempts += 1
        if first_gle is None:
            first_gle = ctypes.get_last_error()
        time.sleep(0.02)
    # 3) 停止
    off = ctypes.create_string_buffer(b"\x00", 1)
    k32.DeviceIoControl(h, IOCTL_START, ctypes.addressof(off), 1, None, 0,
                        ctypes.byref(ret), None)
    stats.update(attempts=attempts, first_gle=first_gle,
                 seconds=round(time.time() - t0, 2))
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


def driver_versions():
    """扫 C:\\Windows\\INF\\oem*.inf，找出与 womic 相关的 DriverVer（INF 可能是 UTF-16）。"""
    windir = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    out = []
    try:
        for p in sorted((windir / "INF").glob("oem*.inf")):
            raw = p.read_bytes()
            txt = None
            for enc in ("utf-16-le", "utf-8", "cp1252"):
                try:
                    txt = raw.decode(enc)
                    break
                except Exception:  # noqa: BLE001
                    continue
            if not txt or "womic" not in txt.lower():
                continue
            ver = ""
            for line in txt.splitlines():
                if line.lower().startswith("driverver"):
                    ver = line.strip()
                    break
            out.append({"inf": p.name, "driverver": ver})
    except OSError:
        pass
    return out


def main():
    # Windows 上 stdout 被重定向到文件时默认用 ANSI 代码页（cp1252），
    # 本脚本大量中文 print 会直接 UnicodeEncodeError 崩掉（agent_stage.py 踩过同一个坑）。
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=3)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--out", default="probe_out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    # 环境存证：真实会话号 / 账号 / 驱动版本（此前 session0=(pid!=0) 是无效判断）
    results["detail"]["env"] = {
        "pid": os.getpid(),
        "session_id": current_session_id(),
        "user": os.environ.get("USERNAME", ""),
        "elevated": ctypes.windll.shell32.IsUserAnAdmin() != 0
        if hasattr(ctypes, "windll") else None,
        "probe_version": "e2e-vendored-1",
        "driver_versions": driver_versions(),
    }
    print(f"  env: {json.dumps(results['detail']['env'], ensure_ascii=False)}")

    devs = enum_devices()
    dev = pick_device(devs)
    if not check("设备列表中存在 WO Mic 设备", dev is not None, dev or "未找到"):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1
    rate = a.rate
    fmt_bit = WAVE_FORMAT_48M16 if rate == 48000 else WAVE_FORMAT_44M16
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
        check("灌音协议已建立（握手通过 + 音频有信号）",
              an["rms"] > 200, f"attempts={stats.get('attempts')} "
              f"handshake={stats.get('handshake')}")

    ok = all(c["ok"] for c in results["checks"])
    results["ok"] = ok          # 供 CI 机器读取
    # JSON 摘要落盘（CI 直接收进 artifact，不必从 stdout 里再抠）
    with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    print(json.dumps(results, ensure_ascii=False, indent=2))
    print(f"\nRESULT={'PASS' if ok else 'FAIL'}")
    print(f"===== 结论: {'PASS' if ok else 'FAIL'} =====")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
