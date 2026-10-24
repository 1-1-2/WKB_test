#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""womic-record-probe —— Windows 虚拟麦克风录音取证探针（tailscale-dual，job B）。

角色：双机联调里"音频真的从 app 流到了 Windows 虚拟麦克风"的独立取证。
灌音由**真 womic.exe** 负责（它把收到的 Opus 流解码后经 \\\\.\\WOMic 灌进
womic.sys 环形缓冲）；本探针**只录不喂**——与 client/tools/e2e_probe.py 的
区别就在这：e2e_probe 自带 1kHz feeder 验证驱动本身，而本场景的灌音源是
womic.exe，若探针也喂音，录音里就是 1kHz+440Hz 的混合音，无法证明音频
来自 app。因此判据是：录到的能量集中在 **440Hz**（app 调试音频率，
MicCaptureService.kt TONE_FREQUENCY_HZ=440.0）附近。

为什么必须由 SYSTEM 计划任务跑（会话隔离，已实证，见 windows-e2e.yml f 步）：
waveInOpen 在交互会话（runneradmin）下 rc=1（MMSYSERR_ERROR），SYSTEM/
Session 0 下从未复现；与提权与否无关。workflow 用 schtasks 部署本探针。

用法：
    python womic-record-probe.py [--seconds 30] [--rate 48000] [--out DIR]
        [--expect-hz 440] [--hz-tolerance 0.15] [--min-rms 500] [--min-snr 3]

输出契约（与 e2e_probe.py 一致，供 CI 机器读取）：
    <out>/capture.wav      录音原件（48kHz/16bit/mono）
    <out>/summary.json     环境 + 分析 + 逐项结论
    stdout 末尾            JSON 摘要 + "RESULT=PASS/FAIL" 行
    退出码                 0 = 全过；1 = 有失败；2 = 环境错误
"""
import argparse
import ctypes
import json
import math
import os
import struct
import sys
import time
import wave
from ctypes import wintypes

try:
    winmm = ctypes.WinDLL("winmm")
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
except (AttributeError, OSError) as exc:
    # 非 Windows 平台没有 WinDLL/winmm（如在 Linux 上误跑本探针）：
    # 按输出契约给"环境错误"退出码 2 + 明确原因，不留裸 traceback。
    sys.stderr.write(
        "[record-probe] 环境错误：本探针只能在 Windows 上运行"
        "（加载 winmm/kernel32 失败：%s）\n" % exc)
    raise SystemExit(2)

CALLBACK_NULL = 0x00000000
WHDR_DONE = 0x00000001


class WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("wFormatTag", wintypes.WORD), ("nChannels", wintypes.WORD),
                ("nSamplesPerSec", wintypes.DWORD), ("nAvgBytesPerSec", wintypes.DWORD),
                ("nBlockAlign", wintypes.WORD), ("wBitsPerSample", wintypes.WORD),
                ("cbSize", wintypes.WORD)]


class WAVEINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD),
                ("vDriverVersion", wintypes.UINT), ("szPname", wintypes.WCHAR * 32),
                ("dwFormats", wintypes.DWORD), ("wChannels", wintypes.WORD),
                ("wReserved1", wintypes.WORD)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wintypes.DWORD),
                ("dwBytesRecorded", wintypes.DWORD), ("dwUser", ctypes.c_size_t),
                ("dwFlags", wintypes.DWORD), ("dwLoops", wintypes.DWORD),
                ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_size_t)]


winmm.waveInGetNumDevs.restype = wintypes.UINT
winmm.waveInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(WAVEINCAPSW), wintypes.UINT]
winmm.waveInOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), wintypes.UINT,
                             ctypes.POINTER(WAVEFORMATEX), ctypes.c_size_t,
                             ctypes.c_size_t, wintypes.DWORD]
winmm.waveInPrepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]
winmm.waveInAddBuffer.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]
winmm.waveInStart.argtypes = [ctypes.c_void_p]
winmm.waveInStop.argtypes = [ctypes.c_void_p]
winmm.waveInReset.argtypes = [ctypes.c_void_p]
winmm.waveInClose.argtypes = [ctypes.c_void_p]
winmm.waveInUnprepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(WAVEHDR), wintypes.UINT]

k32.ProcessIdToSessionId.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]

results = {"checks": [], "devices": [], "detail": {}}


def check(name, ok, info=""):
    results["checks"].append({"name": name, "ok": bool(ok), "info": str(info)[:400]})
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {info}", flush=True)
    return ok


def current_session_id():
    """真实终端会话号（ProcessIdToSessionId，不是 pid 判断）。"""
    sid = wintypes.DWORD(0)
    if k32.ProcessIdToSessionId(k32.GetCurrentProcessId(), ctypes.byref(sid)):
        return sid.value
    return None


def current_user():
    """从进程令牌取账号（DOMAIN\\user），不读环境变量。"""
    try:
        buf = ctypes.create_unicode_buffer(256)
        size = wintypes.DWORD(256)
        if ctypes.windll.advapi32.GetUserNameW(buf, ctypes.byref(size)):
            return buf.value
    except Exception:  # noqa: BLE001
        pass
    return os.environ.get("USERNAME", "")


def enum_devices():
    n = winmm.waveInGetNumDevs()
    out = []
    for i in range(n):
        caps = WAVEINCAPSW()
        if winmm.waveInGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            out.append({"index": i, "name": caps.szPname, "channels": caps.wChannels,
                        "formats": hex(caps.dwFormats)})
    results["devices"] = out
    print(f"waveIn 设备数: {n}", flush=True)
    for d in out:
        print(f"  [{d['index']}] {d['name']!r} ch={d['channels']} formats={d['formats']}",
              flush=True)
    return out


def pick_device(devs):
    for d in devs:
        if "wo mic" in d["name"].lower():
            return d
    return None


def record(dev_index, rate, seconds):
    """录 seconds 秒 48k/16bit/mono 到一个大缓冲（e2e_probe 同款做法）。"""
    fmt = WAVEFORMATEX(1, 1, rate, rate * 2, 2, 16, 0)
    hwi = ctypes.c_void_p()
    rc = winmm.waveInOpen(ctypes.byref(hwi), dev_index, ctypes.byref(fmt), 0, 0,
                          CALLBACK_NULL)
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
    deadline = time.time() + seconds + 8
    while time.time() < deadline and not (hdr.dwFlags & WHDR_DONE):
        time.sleep(0.05)
    winmm.waveInStop(hwi)
    winmm.waveInReset(hwi)
    got = hdr.dwBytesRecorded
    winmm.waveInUnprepareHeader(hwi, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveInClose(hwi)
    return buf.raw[:got], None


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


def analyze(raw, rate, expect_hz):
    n = len(raw) // 2
    if n < rate // 2:
        return {"error": f"录音过短 {n} samples"}
    samples = list(struct.unpack(f"<{n}h", raw[:n * 2]))
    rms = math.sqrt(sum(x * x for x in samples) / n)
    peak = max(abs(x) for x in samples)
    # 逐秒 RMS：日志里能直接看出 app 音频到达的时间窗（取证用）
    per_second = []
    for s in range(0, n, rate):
        seg = samples[s:s + rate]
        if seg:
            per_second.append(round(math.sqrt(sum(x * x for x in seg) / len(seg)), 1))
    # Goertzel 扫描窗口：取**能量最高的一秒**。
    # 为什么不能固定扫开头：联调时序里 womic.exe 约第 5 秒才开始推流，
    # 440Hz 调试音落在第 5-15 秒——扫开头 0.34s 只会看到静音，
    # peak_mag=0、peak_freq 乱指、判据假失败（实测复现过）。
    # 逐秒 RMS 已在上面算出，直接用它定位最响的一秒，健壮且无需额外遍历。
    best_sec = max(range(len(per_second)), key=lambda i: per_second[i]) if per_second else 0
    start = min(best_sec * rate, max(0, n - 16384))
    seg = samples[start:start + 16384]
    mags = {f: goertzel(seg, rate, f) for f in range(200, 3001, 10)}
    best_f = max(mags, key=mags.get)
    best_m = mags[best_f]
    others = sorted((m for f, m in mags.items() if abs(f - best_f) > 60), reverse=True)
    floor = others[len(others) // 2] if others else 0.0
    snr = best_m / floor if floor else float("inf")
    return {"samples": n, "rms": round(rms, 1), "peak": peak,
            "peak_freq": best_f, "peak_mag": round(best_m, 1),
            "noise_floor": round(floor, 1), "snr": round(snr, 1),
            "per_second_rms": per_second}


def finish(res, out_dir, ok):
    """统一收尾：JSON 落盘 + RESULT 行（所有退出路径共用，勿只打 JSON）。"""
    res["ok"] = bool(ok)
    try:
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(res, f, ensure_ascii=False, indent=2)
    except OSError:
        pass
    print(json.dumps(res, ensure_ascii=False, indent=2), flush=True)
    print(f"\nRESULT={'PASS' if ok else 'FAIL'}", flush=True)
    print(f"===== 结论: {'PASS' if ok else 'FAIL'} =====", flush=True)
    return 0 if ok else 1


def main():
    # stdout 重定向到文件时默认 cp1252，中文 print 会 UnicodeEncodeError 崩
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser(description="WO Mic 虚拟麦克风录音取证探针（只录不喂）")
    ap.add_argument("--seconds", type=int, default=30, help="录音秒数")
    ap.add_argument("--rate", type=int, default=48000, help="采样率")
    ap.add_argument("--out", default="probe_out", help="输出目录")
    ap.add_argument("--expect-hz", type=float, default=440.0,
                    help="期望主频（app 调试音 440Hz）")
    ap.add_argument("--hz-tolerance", type=float, default=0.15,
                    help="主频容差（±15%%，容忍 Opus/重采样偏移）")
    ap.add_argument("--min-rms", type=float, default=500.0, help="RMS 下限")
    ap.add_argument("--min-snr", type=float, default=3.0, help="主峰/底噪下限")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    # 环境存证：会话号 / 账号（证明跑在 Session 0，符合 waveIn 隔离约束）
    results["detail"]["env"] = {
        "pid": os.getpid(),
        "session_id": current_session_id(),
        "user": current_user(),
        "probe_version": "womic-dual-record-1",
    }
    print(f"  env: {json.dumps(results['detail']['env'], ensure_ascii=False)}", flush=True)

    devs = enum_devices()
    dev = pick_device(devs)
    if not check("设备列表中存在 WO Mic 设备", dev is not None, dev or "未找到"):
        return finish(results, a.out, False)

    raw, err = record(dev["index"], a.rate, a.seconds)
    if not check("waveIn 录音成功", raw is not None and len(raw) > 0,
                 err or f"{len(raw or b'')} bytes"):
        return finish(results, a.out, False)

    wav_path = os.path.join(a.out, "capture.wav")
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(a.rate)
        w.writeframes(raw)
    print(f"  已保存 {wav_path}（{len(raw)} 字节）", flush=True)

    an = analyze(raw, a.rate, a.expect_hz)
    results["detail"]["analysis"] = an
    if "error" in an:
        check("频谱分析", False, an["error"])
        return finish(results, a.out, False)
    lo, hi = a.expect_hz * (1 - a.hz_tolerance), a.expect_hz * (1 + a.hz_tolerance)
    check("录音有能量（非静音帧）", an["rms"] > a.min_rms,
          f"rms={an['rms']} peak={an['peak']} 阈值>{a.min_rms}")
    check(f"主峰在 {a.expect_hz:.0f}Hz ±{a.hz_tolerance:.0%}（app 调试音频率）",
          lo <= an["peak_freq"] <= hi,
          f"peak={an['peak_freq']}Hz snr={an['snr']}")
    check(f"主峰显著高于底噪（snr>{a.min_snr}）", an["snr"] > a.min_snr,
          f"snr={an['snr']}")
    print("  逐秒 RMS：%s" % an["per_second_rms"], flush=True)

    ok = all(c["ok"] for c in results["checks"])
    return finish(results, a.out, ok)


if __name__ == "__main__":
    sys.exit(main())
