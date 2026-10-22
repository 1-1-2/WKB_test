#!/usr/bin/env python3
"""WO Mic 独立 WASAPI 采集探针：**不启动 feeder、不打开 \\\\.\\WOMic**。

目的（外部评审方案第 1 条）：把"采集链路本身"与"IOCTL 灌音"完全解耦——
仅通过 WASAPI 找到 WO Mic 采集端点，用 GetMixFormat 返回的格式初始化共享模式
（微软明确说明该格式必被共享模式接受），启动采集并统计数据包/帧数/静音标志，
每一步都保存 HRESULT，失败时给出比 waveInOpen rc=1 具体得多的错误码。

判读矩阵（对应评审建议）：
  WASAPI 能持续取得非静音数据  -> 采集链路成立，问题在灌音协议
  初始化/启动失败              -> 按具体 HRESULT 定位（占用/格式不支持/端点创建失败）
  取得的全是静音数据           -> 采集链路成立，接下来解决音频注入
  能打开但没有数据包           -> 仅证明打开成功，还需有效音源验证

用法：python probe_wasapi.py [--seconds 5] [--out wasapi_out]
退出码：0=采集到非静音数据；1=未达该标准（stdout 末尾打印 JSON 摘要，每步含 HRESULT）
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import struct
import sys
import time
import wave
from ctypes import wintypes
from pathlib import Path

# ------------------------------------------------------------------ 常量
CLSID_MMDeviceEnumerator = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
IID_IMMDeviceEnumerator = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"
IID_IAudioClient = "{1CB9AD4C-DBFA-4C32-B178-C2F568A703B2}"
IID_IAudioCaptureClient = "{C8ADBD64-E71E-48A0-A4DE-185C395CD317}"

eCAPTURE = 1                # EDataFlow::eCapture
DEVICE_STATE_ACTIVE = 0x1
STGM_READ = 0
CLSCTX_ALL = 0x17
AUDCLNT_SHAREMODE_SHARED = 0
AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY = 0x1
AUDCLNT_BUFFERFLAGS_SILENT = 0x2
AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR = 0x4

PKEY_Device_FriendlyName = ("{A45C254E-DF1C-4EFD-8020-67D146A850E0}", 14)

HRESULT_NAMES = {
    0x88890001: "AUDCLNT_E_NOT_INITIALIZED",
    0x88890002: "AUDCLNT_E_ALREADY_INITIALIZED",
    0x88890003: "AUDCLNT_E_WRONG_ENDPOINT_TYPE",
    0x88890004: "AUDCLNT_E_DEVICE_INVALIDATED",
    0x88890005: "AUDCLNT_E_DEVICE_NOT_SUPPORTED",
    0x88890006: "AUDCLNT_E_NOTAUTHORIZED",
    0x88890007: "AUDCLNT_E_DEVICE_USAGE",
    0x88890008: "AUDCLNT_E_UNSUPPORTED_FORMAT",
    0x8889000A: "AUDCLNT_E_DEVICE_IN_USE",
    0x8889000B: "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED",
    0x8889000C: "AUDCLNT_E_INVALID_DEVICE_PERIOD",
    0x8889000E: "AUDCLNT_E_ENDPOINT_CREATE_FAILED",
    0x8889000F: "AUDCLNT_E_INVALID_STREAM_FLAG",
    0x88890019: "AUDCLNT_E_BUFFER_ERROR",
    0x8889001A: "AUDCLNT_E_SERVICE_NOT_RUNNING",
    0x80004005: "E_FAIL",
    0x80070005: "E_ACCESSDENIED",
    0x8007000E: "E_OUTOFMEMORY",
    0x80070057: "E_INVALIDARG",
}


def hr_name(hr: int) -> str:
    return HRESULT_NAMES.get(hr & 0xFFFFFFFF, "")


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_ulong), ("Data2", ctypes.c_ushort),
                ("Data3", ctypes.c_ushort), ("Data4", ctypes.c_ubyte * 8)]


def guid(s: str) -> GUID:
    s = s.strip("{}")
    a, b, c, d, e = s.split("-")
    g = GUID()
    g.Data1 = int(a, 16)
    g.Data2 = int(b, 16)
    g.Data3 = int(c, 16)
    g.Data4 = (ctypes.c_ubyte * 8)(*[int(d[i:i + 2], 16) for i in (0, 2)],
                                   *[int(e[i:i + 2], 16) for i in range(0, 12, 2)])
    return g


class PROPVARIANT(ctypes.Structure):
    _fields_ = [("vt", ctypes.c_ushort), ("r1", ctypes.c_ushort),
                ("r2", ctypes.c_ushort), ("r3", ctypes.c_ushort),
                ("u", ctypes.c_ubyte * 16)]


class PROPERTYKEY(ctypes.Structure):
    _fields_ = [("fmtid", GUID), ("pid", ctypes.c_ulong)]


class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [("wFormatTag", ctypes.c_ushort), ("nChannels", ctypes.c_ushort),
                ("nSamplesPerSec", ctypes.c_ulong), ("nAvgBytesPerSec", ctypes.c_ulong),
                ("nBlockAlign", ctypes.c_ushort), ("wBitsPerSample", ctypes.c_ushort),
                ("cbSize", ctypes.c_ushort)]


ole32 = ctypes.WinDLL("ole32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
ole32.CoCreateInstance.argtypes = [ctypes.POINTER(GUID), ctypes.c_void_p, ctypes.c_ulong,
                                   ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)]
k32.ProcessIdToSessionId.restype = wintypes.BOOL
k32.ProcessIdToSessionId.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
k32.GetCurrentProcessId.restype = wintypes.DWORD

VT_LPWSTR = 31


# ------------------------------------------------------------------ COM 基础设施
def vtable(obj, index: int, restype, *argtypes):
    """取 COM 对象 vtable 第 index 个方法并包成可调用（首参 self=接口指针）。

    注意：POINTER(c_void_p) 的下标操作要作用在**指针对象**上；
    `.contents` 是解引用（取第 0 个值），对它下标会 TypeError。
    """
    vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p)).contents.value
    if not vtbl:
        raise RuntimeError("接口指针为空，无法取 vtable")
    fn_addr = ctypes.cast(vtbl, ctypes.POINTER(ctypes.c_void_p))[index]
    return ctypes.CFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(fn_addr)


def enum_capture_endpoints(enum):
    """返回 (err, [(id, name, dev*)])；err=None 表示成功。"""
    coll = ctypes.c_void_p()
    hr = vtable(enum, 3, ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_void_p))(enum, eCAPTURE, DEVICE_STATE_ACTIVE,
                                                 ctypes.byref(coll))
    if hr != 0:
        return hr_name(hr) or hex(hr), []
    n = ctypes.c_ulong()
    vtable(coll, 3, ctypes.c_long, ctypes.POINTER(ctypes.c_ulong))(coll, ctypes.byref(n))
    out = []
    for i in range(n.value):
        dev = ctypes.c_void_p()
        hr = vtable(coll, 4, ctypes.c_long, ctypes.c_ulong,
                    ctypes.POINTER(ctypes.c_void_p))(coll, i, ctypes.byref(dev))
        if hr != 0 or not dev.value:
            continue
        did = ctypes.c_wchar_p()
        vtable(dev, 5, ctypes.c_long, ctypes.POINTER(ctypes.c_wchar_p))(dev, ctypes.byref(did))
        name = ""
        store = ctypes.c_void_p()
        vtable(dev, 4, ctypes.c_long, ctypes.c_ulong,
               ctypes.POINTER(ctypes.c_void_p))(dev, STGM_READ, ctypes.byref(store))
        if store.value:
            pv = PROPVARIANT()
            key = PROPERTYKEY()
            key.fmtid = guid(PKEY_Device_FriendlyName[0])
            key.pid = PKEY_Device_FriendlyName[1]
            hr2 = vtable(store, 5, ctypes.c_long, ctypes.POINTER(PROPERTYKEY),
                         ctypes.POINTER(PROPVARIANT))(store, ctypes.byref(key), ctypes.byref(pv))
            if hr2 == 0 and pv.vt == VT_LPWSTR:
                ptr = struct.unpack_from("<Q", bytes(pv.u))[0]
                if ptr:
                    name = ctypes.wstring_at(ptr)
            vtable(store, 2, ctypes.c_long)(store)  # Release
        out.append({"id": did.value or "", "name": name, "dev": dev})
    return None, out


# ------------------------------------------------------------------ 记录
results = {"steps": [], "env": {}, "endpoints": [], "detail": {}}
_pcm = bytearray()
block_align = [2]      # GetMixFormat 后更新，采集循环用


def step(name, hr=None, info="") -> bool:
    """记录一步及其 HRESULT；返回 True 仅当成功（hr=0 或 hr=None 仅为记录）。"""
    ok = (hr is None) or ((hr & 0xFFFFFFFF) == 0)
    e = {"name": name, "hr": None if hr is None else hex(hr & 0xFFFFFFFF),
         "hr_name": hr_name(hr) if hr is not None else "", "ok": ok}
    if info:
        e["info"] = str(info)[:600]
    results["steps"].append(e)
    extra = f" hr={e['hr']} {e['hr_name']}" if hr is not None else ""
    print(f"[{'OK  ' if ok else 'FAIL'}] {name}{extra} {info}")
    return ok


def current_session_id():
    sid = wintypes.DWORD(0)
    if k32.ProcessIdToSessionId(k32.GetCurrentProcessId(), ctypes.byref(sid)):
        return sid.value
    return None


def driver_versions():
    """复用 probe.py 的实现（同目录）；不可用则跳过。"""
    try:
        sys.path.insert(0, str(Path(__file__).parent))
        import probe  # noqa: PLC0415
        return probe.driver_versions()
    except Exception:  # noqa: BLE001
        return []


def fmt_summary(fx: WAVEFORMATEX, extra: bytes) -> dict:
    d = {"format_tag": fx.wFormatTag, "channels": fx.nChannels,
         "rate": fx.nSamplesPerSec, "bits": fx.wBitsPerSample,
         "block_align": fx.nBlockAlign, "avg_bytes_per_sec": fx.nAvgBytesPerSec,
         "cb_size": fx.cbSize}
    if fx.wFormatTag == 0xFFFE and len(extra) >= 28:
        sub = struct.unpack_from("<I", extra, 24)[0]   # SubFormat GUID Data1
        d["sub_format_guid1"] = hex(sub)
        d["is_float"] = (sub == 3)
        d["is_pcm"] = (sub == 1)
        if fx.cbSize >= 22:
            d["valid_bits"] = struct.unpack_from("<H", extra, 18)[0]
            d["channel_mask"] = hex(struct.unpack_from("<I", extra, 20)[0])
    elif fx.wFormatTag == 3:
        d["is_float"] = True
    elif fx.wFormatTag == 1:
        d["is_pcm"] = True
    return d


def write_wav(path: str, raw: bytes, fmt: dict) -> None:
    """把采集到的 PCM 写成 16bit WAV（float32 则先转换），便于直接试听。"""
    data = raw
    if fmt.get("bits") == 32 and fmt.get("is_float") and len(raw) >= 4:
        n = len(raw) // 4
        floats = struct.unpack(f"<{n}f", raw[:n * 4])
        data = struct.pack(f"<{n}h", *[max(-32768, min(32767, int(v * 32767))) for v in floats])
        fmt = {**fmt, "bits": 16, "is_float": False}
    elif fmt.get("bits") == 32 and len(raw) >= 4:
        n = len(raw) // 4
        ints = struct.unpack(f"<{n}i", raw[:n * 4])
        data = struct.pack(f"<{n}h", *[v >> 16 for v in ints])
        fmt = {**fmt, "bits": 16}
    with wave.open(path, "wb") as w:
        w.setnchannels(fmt.get("channels", 1))
        w.setsampwidth(max(1, fmt.get("bits", 16) // 8))
        w.setframerate(fmt.get("rate", 48000))
        w.writeframes(data)


# ------------------------------------------------------------------ 采集主循环
def capture_loop(client, capture, seconds: float) -> None:
    buf_frames = ctypes.c_uint32()
    vtable(client, 4, ctypes.c_long, ctypes.POINTER(ctypes.c_uint32))(
        client, ctypes.byref(buf_frames))                                    # GetBufferSize
    pmin, pmax = ctypes.c_longlong(), ctypes.c_longlong()
    vtable(client, 9, ctypes.c_long, ctypes.POINTER(ctypes.c_longlong),
           ctypes.POINTER(ctypes.c_longlong))(client, ctypes.byref(pmin), ctypes.byref(pmax))
    results["detail"]["buffer_frames"] = buf_frames.value
    results["detail"]["device_period_min_hns"] = pmin.value
    print(f"  buffer={buf_frames.value} frames, period_min={pmin.value} hns")

    hr = vtable(client, 10, ctypes.c_long)(client)                           # Start
    if not step("IAudioClient::Start", hr):
        results["detail"]["capture"] = {"error": "Start 失败，未采集"}
        return

    packets = total_frames = silent = disc = ts_err = 0
    first_getbuf_hr = None
    t0 = time.time()
    while time.time() - t0 < seconds:
        size = ctypes.c_uint32()
        hr = vtable(capture, 5, ctypes.c_long, ctypes.POINTER(ctypes.c_uint32))(
            capture, ctypes.byref(size))                                     # GetNextPacketSize
        if hr != 0:
            step("GetNextPacketSize", hr, f"t={time.time() - t0:.2f}s")
            break
        if size.value == 0:
            time.sleep(0.01)
            continue
        data = ctypes.POINTER(ctypes.c_ubyte)()
        frames, flags = ctypes.c_uint32(), ctypes.c_uint32()
        dpos, qpc = ctypes.c_uint64(), ctypes.c_uint64()
        hr = vtable(capture, 3, ctypes.c_long,
                    ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
                    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64))(
            capture, ctypes.byref(data), ctypes.byref(frames), ctypes.byref(flags),
            ctypes.byref(dpos), ctypes.byref(qpc))                           # GetBuffer
        if hr != 0:
            if first_getbuf_hr is None:
                first_getbuf_hr = hex(hr & 0xFFFFFFFF)
            step("IAudioCaptureClient::GetBuffer", hr, f"packet#{packets}")
            break
        packets += 1
        total_frames += frames.value
        silent_flag = bool(flags.value & AUDCLNT_BUFFERFLAGS_SILENT)
        if silent_flag:
            silent += 1
        if flags.value & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY:
            disc += 1
        if flags.value & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR:
            ts_err += 1
        if frames.value and data and not silent_flag:
            _pcm.extend(ctypes.string_at(data, frames.value * block_align[0]))
        vtable(capture, 4, ctypes.c_long, ctypes.c_uint32)(
            capture, frames.value)                                           # ReleaseBuffer

    hr = vtable(client, 11, ctypes.c_long)(client)                           # Stop
    step("IAudioClient::Stop", hr)

    results["detail"]["capture"] = {
        "seconds": round(time.time() - t0, 2),
        "packets": packets, "total_frames": total_frames,
        "silent_packets": silent, "discontinuities": disc,
        "timestamp_errors": ts_err,
        "first_getbuffer_hr": first_getbuf_hr,
        "captured_bytes": len(_pcm),
    }


# ------------------------------------------------------------------ 主流程
def main():
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--out", default="wasapi_out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    results["env"] = {
        "pid": os.getpid(), "session_id": current_session_id(),
        "user": os.environ.get("USERNAME", ""),
        "probe_version": "wasapi-1",
        "driver_versions": driver_versions(),
    }
    print(f"  env: {json.dumps(results['env'], ensure_ascii=False)}")

    step("CoInitializeEx", ole32.CoInitializeEx(None, 0))

    enum = ctypes.c_void_p()
    hr = ole32.CoCreateInstance(ctypes.byref(guid(CLSID_MMDeviceEnumerator)), None,
                                CLSCTX_ALL, ctypes.byref(guid(IID_IMMDeviceEnumerator)),
                                ctypes.byref(enum))
    if not step("CoCreateInstance(MMDeviceEnumerator)", hr):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    err, eps = enum_capture_endpoints(enum)
    if err:
        step("EnumAudioEndpoints(eCapture)", 1, f"错误: {err}")
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1
    results["endpoints"] = [{"id": e["id"], "name": e["name"]} for e in eps]
    print(f"  采集端点数: {len(eps)}")
    for e in eps:
        print(f"    {e['name']!r} id={e['id']}")

    target = next((e for e in eps if "wo mic" in e["name"].lower()), None)
    if not step("找到 WO Mic 采集端点", 0 if target else 1,
                target["id"] if target else "未找到"):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1
    results["detail"]["endpoint_id"] = target["id"]

    client = ctypes.c_void_p()
    iid = guid(IID_IAudioClient)
    # IMMDevice::Activate(REFIID, dwClsCtx, pParams, out) —— 注意共 4 个实参
    hr = vtable(target["dev"], 3, ctypes.c_long, ctypes.POINTER(GUID), ctypes.c_ulong,
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
        target["dev"], ctypes.byref(iid), CLSCTX_ALL, None, ctypes.byref(client))
    if not step("IMMDevice::Activate(IAudioClient)", hr):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    mix = ctypes.c_void_p()
    hr = vtable(client, 8, ctypes.c_long, ctypes.POINTER(ctypes.c_void_p))(
        client, ctypes.byref(mix))                                           # GetMixFormat
    if not step("IAudioClient::GetMixFormat", hr):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1
    fx = WAVEFORMATEX.from_address(mix.value)
    extra = ctypes.string_at(mix.value, 18 + max(fx.cbSize, 24))
    summary = fmt_summary(fx, extra)
    results["detail"]["mix_format"] = summary
    print(f"  mix format: {json.dumps(summary, ensure_ascii=False)}")
    block_align[0] = fx.nBlockAlign or 2

    # Initialize(shareMode, flags, 200ms buffer, periodicity=0, format, guid)
    hr = vtable(client, 3, ctypes.c_long, ctypes.c_int, ctypes.c_uint32,
                ctypes.c_longlong, ctypes.c_longlong, ctypes.c_void_p, ctypes.c_void_p)(
        client, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, None)
    if not step("IAudioClient::Initialize(shared, 200ms, mixformat)", hr):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    capture = ctypes.c_void_p()
    iidc = guid(IID_IAudioCaptureClient)
    hr = vtable(client, 14, ctypes.c_long, ctypes.POINTER(GUID),
                ctypes.POINTER(ctypes.c_void_p))(client, ctypes.byref(iidc),
                                                 ctypes.byref(capture))   # GetService
    if not step("IAudioClient::GetService(IAudioCaptureClient)", hr):
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 1

    capture_loop(client, capture, a.seconds)
    cap = results["detail"]["capture"]

    ok = False
    if cap.get("packets", 0) == 0:
        print("  判读：打开成功但无数据包 —— 仅证明打开成功，需有效音源验证")
    elif cap.get("silent_packets") == cap.get("packets"):
        print("  判读：采集链路成立但数据全静音 —— 接下来解决音频注入")
    else:
        ok = cap.get("total_frames", 0) > 0
        print("  判读：WASAPI 采集链路完全成立 —— 问题在灌音协议")

    if len(_pcm) > 0:
        wav_path = os.path.join(a.out, "capture_wasapi.wav")
        write_wav(wav_path, bytes(_pcm), summary)
        print(f"  已保存 {wav_path}")

    results["ok"] = ok
    print(json.dumps(results, ensure_ascii=False, indent=2))
    print(f"\nRESULT={'PASS' if ok else 'FAIL'}")
    print(f"===== 结论: {'PASS' if ok else 'FAIL'} =====")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
