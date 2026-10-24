#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_driver_consistency.py -- MICLINK 内核驱动（T03）静态一致性自校验。

背景
----
当前开发环境是 Linux，装不了 WDK，因此无法真正编译驱动。这个脚本做的是
"在 Linux 上能做的全部静态检查"：把 T03 里几个"做错了必然 BSOD / 静默失效"
的强约束固化成可执行断言。任何一条回归都会在这里变红，而不是等到
Windows 上蓝屏才被发现。

用法
----
    python3 driver/verify/check_driver_consistency.py            # 人读报告
    python3 driver/verify/check_driver_consistency.py --quiet    # 只打汇总
    python3 driver/verify/check_driver_consistency.py --json     # 机器可读

退出码：0 = 全绿；1 = 有 FAIL。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

# --------------------------------------------------------------------------- #
# 路径
# --------------------------------------------------------------------------- #

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DRIVER_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, os.pardir))          # driver/
REPO_DIR = os.path.abspath(os.path.join(DRIVER_DIR, os.pardir))            # miclink-open/

SRC = os.path.join(DRIVER_DIR, "Source")
MAIN = os.path.join(SRC, "Main")
FILTERS = os.path.join(SRC, "Filters")
INC = os.path.join(SRC, "Inc")
UTIL = os.path.join(SRC, "Utilities")
SHARED_INC = os.path.join(REPO_DIR, "shared", "core", "include")
SHARED_SRC = os.path.join(REPO_DIR, "shared", "core", "src")

MSBUILD_NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"


# --------------------------------------------------------------------------- #
# 小工具
# --------------------------------------------------------------------------- #

class Report:
    def __init__(self) -> None:
        self.results: list[tuple[str, bool, str]] = []

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.results.append((name, bool(ok), detail))
        return bool(ok)

    @property
    def passed(self) -> int:
        return sum(1 for _, ok, _ in self.results if ok)

    @property
    def failed(self) -> int:
        return sum(1 for _, ok, _ in self.results if not ok)


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def exists(path: str) -> bool:
    return os.path.isfile(path)


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释，让 grep 类断言只针对真实代码。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def code_of(path: str) -> str:
    return strip_comments(read(path))


def any_line(text: str, pattern: str) -> bool:
    return re.search(pattern, text, flags=re.M) is not None


def line_of(text: str, pattern: str) -> int:
    """返回 pattern 首次命中的行号（1-based）；未命中返回 -1。"""
    m = re.search(pattern, text, flags=re.M)
    if not m:
        return -1
    return text.count("\n", 0, m.start()) + 1


NS_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


# --------------------------------------------------------------------------- #
# 检查项
# --------------------------------------------------------------------------- #

def check_files(report: Report) -> None:
    """1. 交付文件清单：新增的存在、删除的确实没了。"""
    must_exist = [
        os.path.join(INC, "miclink", "miclink_ctl.h"),
        os.path.join(MAIN, "miclinkctl.cpp"),
        os.path.join(MAIN, "MicLinkVirtualMic.inx"),
        os.path.join(MAIN, "MicLinkVirtualMic.rc"),
        os.path.join(DRIVER_DIR, "MicLinkVirtualMic.sln"),
        os.path.join(UTIL, "miclink_shared_ring.c"),
        os.path.join(UTIL, "miclink_shared_dsp.c"),
        os.path.join(SHARED_INC, "miclink", "miclink_ioctl.h"),
        os.path.join(SHARED_INC, "miclink", "miclink_ring.h"),
        os.path.join(SHARED_INC, "miclink", "miclink_dsp.h"),
    ]
    missing = [p for p in must_exist if not exists(p)]
    report.check("交付文件齐全（新增 7 个 + 共享 3 个）",
                 not missing, "缺失: %s" % missing)

    gone = [
        os.path.join(FILTERS, "speakertopo.cpp"),
        os.path.join(FILTERS, "speakertopo.h"),
        os.path.join(FILTERS, "speakertoptable.h"),
        os.path.join(FILTERS, "speakerwavtable.h"),
        os.path.join(MAIN, "SimpleAudioSample.inx"),
        os.path.join(MAIN, "SimpleAudioSample.rc"),
        os.path.join(DRIVER_DIR, "SimpleAudioSample.sln"),
        # 缺陷 D-04：正弦发生器已无任何调用点，文件一并删除
        os.path.join(UTIL, "ToneGenerator.cpp"),
        os.path.join(UTIL, "ToneGenerator.h"),
        # WOMIC -> MICLINK 重命名残留（CI run 39335899536：双 INF 盖章）
        os.path.join(MAIN, "WomicVirtualMic.inx"),
        os.path.join(MAIN, "WomicVirtualMic.rc"),
        os.path.join(MAIN, "womicctl.cpp"),
        os.path.join(INC, "womic", "womic_ctl.h"),
        os.path.join(UTIL, "womic_shared_ring.c"),
        os.path.join(UTIL, "womic_shared_dsp.c"),
        os.path.join(DRIVER_DIR, "WomicVirtualMic.sln"),
        os.path.join(SHARED_INC, "womic", "womic_ioctl.h"),
        os.path.join(SHARED_INC, "womic", "womic_ring.h"),
        os.path.join(REPO_DIR, "server", "womicserver.ini.example"),
        os.path.join(REPO_DIR, "android", "app", "src", "main", "java", "com", "womic", "open", "MainActivity.kt"),
    ]
    leftover = [p for p in gone if exists(p)]
    report.check("speaker 文件、旧 INF/RC/SLN、ToneGenerator 与 Womic 残留已删除",
                 not leftover, "残留: %s" % leftover)


def check_no_speaker_refs(report: Report) -> None:
    """2. 删除的 speaker 文件在全树无残留引用（允许项已在白名单里）。"""
    allowed = {
        # KSAUDIO_SPEAKER_MONO 是 ksmedia 里的常量，与 speaker 端点无关
        "KSAUDIO_SPEAKER_MONO",
        "KSAUDIO_SPEAKER_STEREO",
        # eSpeakerDevice 是 eDeviceType 枚举的第 0 项，
        # minwavert.h 的 IsRenderDevice() 仍在对它做比较；改枚举值风险更大，故保留。
        "eSpeakerDevice",
    }
    offenders: list[str] = []
    for root, _dirs, files in os.walk(SRC):
        for fn in files:
            if not fn.endswith((".c", ".cpp", ".h", ".inx", ".rc")):
                continue
            path = os.path.join(root, fn)
            text = code_of(path)
            for m in re.finditer(r"[A-Za-z_]*[Ss]peaker[A-Za-z_]*", text):
                tok = m.group(0)
                if tok in allowed:
                    continue
                offenders.append("%s: %s" % (os.path.relpath(path, DRIVER_DIR), tok))
    report.check("无 speaker 端点残留代码引用",
                 not offenders, "; ".join(sorted(set(offenders))[:8]))


def check_miclink_ctl_header(report: Report) -> None:
    """3. miclink_ctl.h 的契约常量与结构体布局。"""
    path = os.path.join(INC, "miclink", "miclink_ctl.h")
    text = read(path)

    for macro, val in [
        ("MICLINK_CONTROL_SIGNATURE", "0x574F4D43UL"),
        ("MICLINK_POOLTAG", "'CmoW'"),
    ]:
        report.check("miclink_ctl.h 定义 %s" % macro,
                     any_line(text, r"#define\s+%s\s+%s" % (macro, val)))

    # 这两个宏的值里含反斜杠，用子串判断比正则更不容易出错
    report.check("miclink_ctl.h 定义 MICLINK_CONTROL_NT_NAME (\\Device\\MicLinkControl)",
                 ('#define MICLINK_CONTROL_NT_NAME' in text) and
                 ('L"' + chr(92) * 2 + 'Device' + chr(92) * 2 + 'MicLinkControl"' in text))
    report.check("miclink_ctl.h 定义 MICLINK_CONTROL_SYMLINK (\\DosDevices\\Global\\...)",
                 ('#define MICLINK_CONTROL_SYMLINK' in text) and
                 ('L"' + chr(92) * 2 + 'DosDevices' + chr(92) * 2 + 'Global'
                  + chr(92) * 2 + 'MicLinkControl"' in text))

    # Signature 必须是第一个字段（两段式判定直接解引用 offset 0）
    m = re.search(r"typedef\s+struct\s+_MICLINK_CONTROL_EXT\s*\{(.*?)\}\s*MICLINK_CONTROL_EXT",
                  text, flags=re.S)
    ok = False
    detail = "未找到 MICLINK_CONTROL_EXT"
    if m:
        body = strip_comments(m.group(1))
        fields = [f.strip() for f in body.split(";") if f.strip()]
        first = fields[0] if fields else ""
        ok = first.endswith("Signature")
        detail = "第一字段 = %r" % first
    report.check("MICLINK_CONTROL_EXT.Signature 是第一个字段", ok, detail)

    required = ["Self", "OpenHandleCount", "Injecting", "InjectFormat", "StreamFormat",
                "StreamOpen", "Ring", "FramesWritten", "FramesConsumed",
                "UnderrunCount", "OverrunCount", "DroppedFrames",
                "RingHighWatermark", "LastSample", "Primed"]
    if m:
        body = m.group(1)
        missing = [f for f in required
                   if not re.search(r"\b%s\b" % f, body)]
        report.check("MICLINK_CONTROL_EXT 字段齐全（%d 项）" % len(required),
                     not missing, "缺失: %s" % missing)

    report.check("miclink_ctl.h 直接 include 三方共享头（无副本）",
                 any_line(text, r'#include\s+"miclink/miclink_ioctl\.h"') and
                 any_line(text, r'#include\s+"miclink/miclink_ring\.h"') and
                 any_line(text, r'#include\s+"miclink/miclink_dsp\.h"'))

    report.check("声明 extern PDEVICE_OBJECT g_pMicLinkControlDevice",
                 any_line(text, r"extern\s+PDEVICE_OBJECT\s+g_pMicLinkControlDevice\s*;"))
    report.check("声明 MicLinkGetControlExt()",
                 any_line(text, r"MicLinkGetControlExt\s*\("))
    report.check("MicLinkGetControlExt 标注可在 DISPATCH_LEVEL 调用",
                 any_line(text, r"_IRQL_requires_max_\(DISPATCH_LEVEL\)"))

    # 全局只定义一次
    ctl_cpp = code_of(os.path.join(MAIN, "miclinkctl.cpp"))
    defs = re.findall(r"^\s*PDEVICE_OBJECT\s+g_pMicLinkControlDevice\s*=", ctl_cpp, flags=re.M)
    report.check("g_pMicLinkControlDevice 在 miclinkctl.cpp 中恰好定义一次",
                 len(defs) == 1, "定义次数 = %d" % len(defs))


def check_header_impl_parity(report: Report) -> None:
    """4. miclink_ctl.h 里声明的每个函数都要在 miclinkctl.cpp 里有定义（无悬空 extern）。"""
    hdr = code_of(os.path.join(INC, "miclink", "miclink_ctl.h"))
    impl = code_of(os.path.join(MAIN, "miclinkctl.cpp"))

    declared = set()
    # 形如 `MicLinkXxx (` 的函数声明（排除调用）
    for m in re.finditer(r"^\s*(?:_IRQL_[A-Za-z_]*\([^)]*\)\s*)*(?:DRIVER_DISPATCH\s+)?"
                         r"(MicLink[A-Za-z0-9_]*)\s*\(", hdr, flags=re.M):
        declared.add(m.group(1))
    # `DRIVER_DISPATCH MicLinkDispatchCreate;` 这类无形参声明
    for m in re.finditer(r"^\s*DRIVER_DISPATCH\s+(MicLink[A-Za-z0-9_]*)\s*;", hdr, flags=re.M):
        declared.add(m.group(1))

    missing = []
    for name in sorted(declared):
        if not re.search(r"^\s*(?:static\s+)?[\w:*&\s]*?\b%s\s*\(" % re.escape(name),
                         impl, flags=re.M):
            missing.append(name)
    report.check("miclink_ctl.h 声明的函数在 miclinkctl.cpp 全部有定义（%d 个）" % len(declared),
                 not missing, "缺失: %s" % missing)

    # 反向：cpp 里定义的全局函数必须在头文件声明过（否则别的 TU 用不到，属死代码）
    extra = []
    # 注意：本工程里 `static` 常常单独成行（`static\nNTSTATUS\nMicLinkXxx(`），
    # 因此不能只看行首；改为回看匹配起点前的最后 12 个字符里有没有 static。
    for m in re.finditer(r"(?:static\s+)?(?:NTSTATUS|VOID|BOOLEAN|PMICLINK_CONTROL_EXT)\s+"
                         r"(MicLink[A-Za-z0-9_]*)\s*\(", impl):
        # 匹配本身可能就以 static 开头（正则里的 (?:static\s+)? 是可选的）
        if m.group(0).lstrip().startswith("static"):
            continue
        prefix = impl[max(0, m.start() - 12):m.start()]
        if "static" in prefix:
            continue
        if m.group(1) not in declared:
            extra.append(m.group(1))
    report.check("miclinkctl.cpp 无未声明的全局函数",
                 not extra, "未在头文件声明: %s" % extra)


def check_dispatch_order(report: Report) -> None:
    """5. dispatch 覆盖必须在 PcInitializeAdapterDriver 之后（顺序不可颠倒）。"""
    text = read(os.path.join(MAIN, "adapter.cpp"))
    stripped = strip_comments(text)

    i_pc = stripped.find("PcInitializeAdapterDriver")
    i_hook = stripped.find("MicLinkInitializeControlDevice")
    i_pnp = stripped.find("MajorFunction[IRP_MJ_PNP] = PnpHandler")
    i_unload = stripped.find("DriverObject->DriverUnload = DriverUnload")

    report.check("存在 PcInitializeAdapterDriver 调用", i_pc >= 0)
    report.check("存在 MicLinkInitializeControlDevice 调用", i_hook >= 0)
    report.check("dispatch 覆盖在 PcInitializeAdapterDriver 之后",
                 i_pc >= 0 and i_hook > i_pc,
                 "Pc@%d, MicLink@%d" % (i_pc, i_hook))
    report.check("dispatch 覆盖也在官方 IRP_MJ_PNP 覆盖附近（同一 INIT 段内）",
                 i_pnp >= 0 and abs(i_hook - i_pnp) < 2000,
                 "PNP@%d, MicLink@%d" % (i_pnp, i_hook))
    report.check("DriverUnload hook 仍在最后（在覆盖之后）",
                 i_unload >= 0 and i_unload > i_hook,
                 "Unload@%d" % i_unload)


def check_pnp_shortcircuit(report: Report) -> None:
    """6. 坑 1（N-3）：PnpHandler 入口必须先短路控制设备。"""
    text = read(os.path.join(MAIN, "adapter.cpp"))
    stripped = strip_comments(text)

    m = re.search(r"NTSTATUS\s*\n?\s*PnpHandler\s*\(", stripped)
    if not m:
        report.check("找到 PnpHandler", False)
        return
    report.check("找到 PnpHandler", True)

    body = stripped[m.start():]
    # 只看 PnpHandler 的函数体（到下一个顶层 "}" 之前的首个 switch 为止足够）
    i_short = body.find("MicLinkIsControlDevice")
    i_cast = body.find("static_cast<PortClassDeviceContext*>")
    i_complete = body.find("IoCompleteRequest")

    report.check("PnpHandler 里有 MicLinkIsControlDevice 短路", i_short >= 0)
    report.check("短路发生在 PortClassDeviceContext 强转之前（否则必 BSOD）",
                 i_short >= 0 and i_cast >= 0 and i_short < i_cast,
                 "short@%d, cast@%d" % (i_short, i_cast))
    report.check("短路分支直接完成 IRP（不调用 PcDispatchIrp）",
                 i_complete >= 0 and i_complete < i_cast,
                 "complete@%d" % i_complete)
    report.check("短路分支位于 Cleanup()/Release() 调用之前",
                 body.find("Cleanup()") > i_short)


def check_dispatch_forwarding(report: Report) -> None:
    """7. 非控制设备的 IRP 必须 PcDispatchIrp 原样回投（否则端点枚举不出来）。"""
    impl = code_of(os.path.join(MAIN, "miclinkctl.cpp"))

    n_forward = len(re.findall(r"PcDispatchIrp\s*\(\s*DeviceObject\s*,\s*Irp\s*\)", impl))
    report.check("4 个 dispatch 均向 PortCls 回投（PcDispatchIrp）",
                 n_forward >= 4, "回投处数 = %d" % n_forward)

    n_guard = len(re.findall(r"if\s*\(!MicLinkIsControlDevice\(", impl))
    report.check("每个 dispatch 都有 MicLinkIsControlDevice 守卫",
                 n_guard >= 4, "守卫数 = %d" % n_guard)

    # 两段式判定：先指针快拒，再 magic 终裁
    fn = code_of(os.path.join(MAIN, "miclinkctl.cpp"))
    m = re.search(r"MicLinkIsControlDevice\s*\((?:[^;]*?)\)\s*\{(.*?)\n\}", fn, flags=re.S)
    ok = False
    detail = "未找到函数体"
    if m:
        b = m.group(1)
        i_ptr = b.find("!= g_pMicLinkControlDevice")
        i_sig = b.find("Signature != MICLINK_CONTROL_SIGNATURE")
        ok = i_ptr >= 0 and i_sig >= 0 and i_ptr < i_sig
        detail = "指针快拒@%d, magic 终裁@%d" % (i_ptr, i_sig)
    report.check("两段式判定：指针快拒先于 magic 终裁", ok, detail)


def check_datarange_intersection(report: Report) -> None:
    """8. 坑 2（N-1）：DataRangeIntersection 必须遍历格式表，不能只返回 [0]。"""
    text = code_of(os.path.join(MAIN, "minwavert.cpp"))

    report.check("不再硬编码 `*resultantFormat = *MicArrayPinSupportedDeviceFormats`",
                 not any_line(text, r"\*resultantFormat\s*=\s*\*MicArrayPinSupportedDeviceFormats\s*;"))
    report.check("改为遍历格式表（SIZEOF_ARRAY + for）",
                 any_line(text, r"SIZEOF_ARRAY\(MicArrayPinSupportedDeviceFormats\)") and
                 re.search(r"for\s*\(\s*formatIndex\s*=\s*0", text) is not None)
    report.check("无匹配时返回 STATUS_NO_MATCH（而不是强行返回 [0]）",
                 any_line(text, r"return\s+STATUS_NO_MATCH\s*;"))
    report.check("存在 MicLinkFormatMatchesDataRange 判据函数",
                 any_line(text, r"MicLinkFormatMatchesDataRange\s*\(") and
                 any_line(text, r"^\s*MicLinkFormatMatchesDataRange\s*\("))
    # 判据要覆盖采样率上下界
    m = re.search(r"MicLinkFormatMatchesDataRange\s*\([^)]*\)\s*\{(.*?)\n\}", text, flags=re.S)
    ok = False
    if m:
        b = m.group(1)
        ok = ("MaximumSampleFrequency" in b) and ("MinimumSampleFrequency" in b) and \
             ("MaximumChannels" in b) and ("MaximumBitsPerSample" in b)
    report.check("判据覆盖通道数/位深/采样率上下界", ok)


def check_format_table(report: Report) -> None:
    """9. 格式表：16-bit mono 44.1k + 48k，两个都要能被选中。"""
    text = code_of(os.path.join(FILTERS, "micarraywavtable.h"))

    report.check("MICARRAY_DEVICE_MAX_CHANNELS == 1",
                 any_line(text, r"#define\s+MICARRAY_DEVICE_MAX_CHANNELS\s+1\b"))
    report.check("MICARRAY_RAW_CHANNELS == 1",
                 any_line(text, r"#define\s+MICARRAY_RAW_CHANNELS\s+1\b"))
    report.check("位深宏为 16（且旧的 32 位宏已不存在）",
                 any_line(text, r"#define\s+MICARRAY_BITS_PER_SAMPLE_PCM\s+16\b") and
                 not any_line(text, r"MICARRAY_32_BITS_PER_SAMPLE_PCM"))

    rates = re.findall(r"nSamplesPerSec.*?\n\s*(\d+),", text)
    # 直接从表体里数 48000 / 44100 条目
    n48 = len(re.findall(r"^\s*48000,", text, flags=re.M))
    n441 = len(re.findall(r"^\s*44100,", text, flags=re.M))
    report.check("格式表含 48 kHz 与 44.1 kHz 两项",
                 n48 >= 1 and n441 >= 1, "48000 x%d, 44100 x%d" % (n48, n441))

    n_bits16 = len(re.findall(r"MICARRAY_BITS_PER_SAMPLE_PCM", text))
    report.check("两项都使用 16-bit 位深宏", n_bits16 >= 4, "引用次数 = %d" % n_bits16)
    report.check("MONO 通道掩码（不再是 STEREO）",
                 "KSAUDIO_SPEAKER_MONO" in text and "KSAUDIO_SPEAKER_STEREO" not in text)

    # [0] 必须是 48 kHz（设计约定：与 RAW 默认格式、系统默认混音率一致）
    m = re.search(r"MicArrayPinSupportedDeviceFormats\[\]\s*=\s*\{(.*)", text, flags=re.S)
    ok = False
    if m:
        first_rate = re.search(r"\n\s*(48000|44100),", m.group(1))
        ok = bool(first_rate and first_rate.group(1) == "48000")
    report.check("48 kHz 位于格式表 [0]", ok)

    # DataRange 要覆盖两个采样率
    n_ranges = len(re.findall(r"MICARRAY_RAW_SAMPLE_RATE|MICARRAY_ALT_SAMPLE_RATE", text))
    report.check("DataRanges 覆盖两个采样率", n_ranges >= 4, "range 引用 = %d" % n_ranges)
    report.check("KSDATARANGE_ATTRIBUTES 标在属性列表之前的那一条上",
                 text.rfind("KSDATARANGE_ATTRIBUTES") >
                 text.rfind("MICARRAY_RAW_SAMPLE_RATE,"))


def check_no_sine(report: Report) -> None:
    """10. 驱动不再产生正弦；捕获数据来自 MICLINK ring。"""
    for path in (os.path.join(MAIN, "minwavertstream.cpp"),
                 os.path.join(MAIN, "minwavertstream.h"),
                 os.path.join(MAIN, "minwavert.cpp"),
                 os.path.join(MAIN, "adapter.cpp")):
        code = code_of(path)
        report.check("%s 不再引用正弦发生器" % os.path.basename(path),
                     "GenerateSine" not in code and "m_ToneGenerator" not in code)

    # 缺陷 D-04：注册表开关 DisableToneGenerator 随正弦发生器一并移除
    adapter_code = code_of(os.path.join(MAIN, "adapter.cpp"))
    report.check("adapter.cpp 不再读取 DisableToneGenerator 注册表项",
                 "DisableToneGenerator" not in adapter_code)
    report.check("adapter.cpp 不再定义 g_DisableToneGenerator",
                 "g_DisableToneGenerator" not in adapter_code)

    hdr = code_of(os.path.join(MAIN, "minwavertstream.h"))
    report.check("minwavertstream.h 不再 include ToneGenerator.h",
                 not any_line(hdr, r'#include\s+"ToneGenerator\.h"'))
    report.check("minwavertstream.h 持有 miclink_declck 去咔哒状态",
                 any_line(hdr, r"miclink_declck\s+m_Declck\s*;"))
    report.check("minwavertstream.h include miclink/miclink_ctl.h",
                 any_line(hdr, r'#include\s+"miclink/miclink_ctl\.h"'))


def check_writebytes(report: Report) -> None:
    """11. 坑 3（N-2）：WriteBytes 的 DPC 安全性与数据来源。"""
    text = read(os.path.join(MAIN, "minwavertstream.cpp"))
    m = re.search(r"CMiniportWaveRTStream::WriteBytes\s*\((?:[^)]*)\)"
                  r"\s*(?:/\*.*?\*/\s*)?\{", text, flags=re.S)
    report.check("找到 WriteBytes 定义", m is not None)
    if not m:
        return

    # 取函数体（按大括号配对）
    start = text.index("{", m.end() - 1)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = text[start:end]
    code = strip_comments(body)

    report.check("WriteBytes 从 MICLINK ring 读取（miclink_ring_read_fill_silence）",
                 "miclink_ring_read_fill_silence" in code)
    report.check("WriteBytes 应用去咔哒（miclink_apply_declck）",
                 "miclink_apply_declck" in code)
    report.check("WriteBytes 取 ring 前先调用 MicLinkGetControlExt",
                 code.find("MicLinkGetControlExt") < code.find("miclink_ring_read_fill_silence"))

    forbidden = {
        "ExAllocatePool": r"ExAllocatePool",
        "ExFreePool": r"ExFreePool",
        "KeWaitForSingleObject": r"KeWaitForSingleObject",
        "KeAcquireSpinLock": r"KeAcquireSpinLock",
        "KeAcquireInStackQueuedSpinLock": r"KeAcquireInStackQueuedSpinLock",
        "ExAcquireResourceExclusiveLite": r"ExAcquireResource",
        "ExAcquireFastMutex": r"ExAcquireFastMutex",
        "KeDelayExecutionThread": r"KeDelayExecutionThread",
        "PAGED_CODE": r"PAGED_CODE",
        "ZwCreateFile/ZwWriteFile": r"\bZw\w+File\b",
        "floating point (float/double)": r"\b(double|float)\s+\w+\s*=",
        "memcpy/RtlCopyMemory on paged path": r"RtlCopyMemory",
    }
    bad = [name for name, pat in forbidden.items() if re.search(pat, code)]
    report.check("WriteBytes 无阻塞/分配/取锁/分页/浮点调用", not bad, "命中: %s" % bad)

    report.check("WriteBytes 注释标明 DISPATCH_LEVEL + 自旋锁约束",
                 "DISPATCH_LEVEL" in body and "SpinLock" in body)

    # 零填充 + 去咔哒渐变在欠载时生效
    report.check("欠载时补静音并通知去咔哒（miclink_declck_notify_loss）",
                 "miclink_declck_notify_loss" in code and
                 "miclink_fill_silence" in code)


def check_ioctl_coverage(report: Report) -> None:
    """12. 九个 IOCTL 全部实现。"""
    impl = code_of(os.path.join(MAIN, "miclinkctl.cpp"))
    shared = read(os.path.join(SHARED_INC, "miclink", "miclink_ioctl.h"))

    # 排除 include guard (MICLINK_IOCTL_H) 与 device type (MICLINK_IOCTL_DEVICE_TYPE)，
    # 剩下的就是真正的 9 个控制码。
    codes = [c for c in re.findall(r"#define\s+(MICLINK_IOCTL_[A-Z_]+)\s", shared)
             if c not in ("MICLINK_IOCTL_H", "MICLINK_IOCTL_DEVICE_TYPE")]
    report.check("共享头定义了 9 个 IOCTL", len(codes) == 9, "实际 %d: %s" % (len(codes), codes))

    missing = [c for c in codes if ("case %s:" % c) not in impl]
    report.check("9 个 IOCTL 全部在 switch 中有分支", not missing, "缺失: %s" % missing)

    # METHOD_BUFFERED：WRITE_PCM 走 SystemBuffer，绝不挂起
    write_branch = impl[impl.find("case MICLINK_IOCTL_WRITE_PCM:"):]
    write_branch = write_branch[:write_branch.find("\n    case ")] \
        if "\n    case " in write_branch else write_branch
    report.check("WRITE_PCM 使用 SystemBuffer（METHOD_BUFFERED）",
                 "AssociatedIrp.SystemBuffer" in impl)
    report.check("WRITE_PCM 缓冲满返回 STATUS_BUFFER_TOO_SMALL（不挂起 IRP）",
                 "STATUS_BUFFER_TOO_SMALL" in write_branch)
    report.check("WRITE_PCM 绝不 IoMarkIrpPending / 不等待空间",
                 "IoMarkIrpPending" not in impl and
                 "KeWaitForSingleObject" not in impl)
    report.check("WRITE_PCM 校验 frameBytes 与当前格式一致（禁止重采样）",
                 "frameBytes != frameBytes" in write_branch or
                 re.search(r"header\.frameBytes\s*!=\s*frameBytes", write_branch) is not None)
    report.check("内核侧无重采样实现",
                 not re.search(r"\b(resample|Resample|RESAMPLE|SRC|src_convert)\b", impl))


def check_no_shared_copy(report: Report) -> None:
    """13. 禁止把共享头复制一份到驱动目录。"""
    offenders = []
    for root, _dirs, files in os.walk(SRC):
        for fn in files:
            if fn in ("miclink_ioctl.h", "miclink_ring.h", "miclink_dsp.h", "miclink_atomic.h"):
                rel = os.path.relpath(os.path.join(root, fn), DRIVER_DIR)
                if "miclink_ctl.h" not in fn:
                    offenders.append(rel)
    report.check("驱动目录内无共享头的副本", not offenders, "副本: %s" % offenders)

    hdr = read(os.path.join(INC, "miclink", "miclink_ctl.h"))
    report.check("miclink_ctl.h 引用的是 miclink/ 前缀路径（指向 shared/core/include）",
                 all(('"miclink/%s"' % h) in hdr
                     for h in ("miclink_ioctl.h", "miclink_ring.h", "miclink_dsp.h")))


def check_inf(report: Report) -> None:
    """14. INF：编码、产品名、只注册 capture、服务名。"""
    path = os.path.join(MAIN, "MicLinkVirtualMic.inx")
    raw = open(path, "rb").read()
    report.check("INF 保持 UTF-16LE + BOM（与基线一致，否则 stampinf 会失败）",
                 raw[:2] == b"\xff\xfe")
    text = raw.decode("utf-16")

    report.check('INF 设备名为 "MicLink Virtual Microphone"',
                 'MICLINK_VM.DeviceDesc="MicLink Virtual Microphone"' in text)
    report.check("INF 硬件 ID 为 ROOT\\MicLinkVirtualMic",
                 "ROOT\\MicLinkVirtualMic" in text)
    report.check("INF 服务二进制为 miclinkvirtualmic.sys",
                 "miclinkvirtualmic.sys" in text)
    report.check("INF 注册 KSCATEGORY_CAPTURE",
                 "AddInterface=%KSCATEGORY_CAPTURE%" in text)
    report.check("INF 不再注册 KSCATEGORY_RENDER 接口（纯麦克风）",
                 "AddInterface=%KSCATEGORY_RENDER%" not in text)
    report.check("INF 不含 speaker 接口段",
                 "KSNAME_WaveSpeaker" not in text and "SIMPLEAUDIOSAMPLE" not in text)
    report.check("INF Manufacturer 段已改为 MICLINK",
                 "%MfgName%=MICLINK,NT$ARCH$.10.0...22000" in text)

    # 缺陷 D-09：DriverVer 不能沿用 SYSVAD 示例的 2016 年日期
    m_ver = re.search(r"DriverVer\s*=\s*(\d{2})/(\d{2})/(\d{4})\s*,\s*([\d.]+)", text)
    report.check("INF DriverVer 可解析", m_ver is not None)
    if m_ver is not None:
        year = int(m_ver.group(3))
        report.check("INF DriverVer 用真实构建日期（不是 SYSVAD 示例的 02/22/2016）",
                     year >= 2026, "DriverVer = %s" % m_ver.group(0))


def check_vcxproj(report: Report) -> None:
    """15. 四个 vcxproj：新增文件收录、include 路径、wdmsec.lib。"""
    def items(proj: str, tag: str) -> list[str]:
        tree = ET.parse(proj)
        return [e.get("Include") for e in tree.iter(MSBUILD_NS + tag) if e.get("Include")]

    main = os.path.join(MAIN, "Main.vcxproj")
    util = os.path.join(UTIL, "Utilities.vcxproj")

    report.check("Main.vcxproj 收录 miclinkctl.cpp",
                 "miclinkctl.cpp" in items(main, "ClCompile"))
    report.check("Main.vcxproj 收录 MicLinkVirtualMic.rc",
                 "MicLinkVirtualMic.rc" in items(main, "ResourceCompile"))
    report.check("Main.vcxproj 不再引用 SimpleAudioSample.rc",
                 "SimpleAudioSample.rc" not in items(main, "ResourceCompile"))

    ut_items = items(util, "ClCompile")
    report.check("Utilities.vcxproj 收录共享 core 的两个内核外壳",
                 "miclink_shared_ring.c" in ut_items and "miclink_shared_dsp.c" in ut_items)
    report.check("Utilities.vcxproj 不再编译 speakertopo.cpp",
                 "speakertopo.cpp" not in items(os.path.join(FILTERS, "Filters.vcxproj"),
                                                "ClCompile"))
    # 缺陷 D-04：正弦发生器已删除，不得再出现在 Utilities 的编译项里
    report.check("Utilities.vcxproj 不再编译 tonegenerator.cpp",
                 "tonegenerator.cpp" not in ut_items)

    main_text = read(main)
    report.check("Main.vcxproj 链接 wdmsec.lib（IoCreateDeviceSecure）",
                 "wdmsec.lib" in main_text)
    report.check("Main.vcxproj TargetName 改为 MicLinkVirtualMic",
                 "<TargetName>MicLinkVirtualMic</TargetName>" in main_text and
                 "SimpleAudioSample" not in main_text)

    for proj, label in [(main, "Main"),
                        (os.path.join(FILTERS, "Filters.vcxproj"), "Filters"),
                        (os.path.join(INC, "Inc.vcxproj"), "Inc"),
                        (util, "Utilities")]:
        t = read(proj)
        report.check("%s.vcxproj include 路径含 shared\\core\\include" % label,
                     r"..\..\..\shared\core\include" in t)
        report.check("%s.vcxproj include 路径含 $(SolutionDir)Source\\Inc" % label,
                     r"$(SolutionDir)Source\Inc" in t)

    # vcxproj 必须是合法 XML
    for proj in (main, os.path.join(FILTERS, "Filters.vcxproj"),
                 os.path.join(INC, "Inc.vcxproj"), util,
                 os.path.join(MAIN, "Main.vcxproj.Filters"),
                 os.path.join(UTIL, "Utilities.vcxproj")):
        try:
            ET.parse(proj)
            ok = True
        except ET.ParseError as exc:
            ok = False
        report.check("%s 是合法 XML" % os.path.basename(proj), ok)


def check_misc_constraints(report: Report) -> None:
    """16. 其它硬性约束。"""
    impl = read(os.path.join(MAIN, "miclinkctl.cpp"))
    code = strip_comments(impl)

    report.check("创建设备后清 DO_DEVICE_INITIALIZING",
                 "DO_DEVICE_INITIALIZING" in code)
    report.check("符号链接使用 \\DosDevices\\Global\\（跨 session 可见）",
                 r"\DosDevices\Global\MicLinkControl" in impl)
    report.check("ring 存储用 NonPagedPool（POOL_FLAG_NON_PAGED / ExAllocatePool2）",
                 "POOL_FLAG_NON_PAGED" in code and "ExAllocatePool2" in code)
    m = re.search(r"MicLinkDeleteControlDevice\s*\(\s*void\s*\)\s*(?:/\*.*?\*/\s*)?\{",
                  code, flags=re.S)
    if m:
        depth, end = 0, len(code)
        for i in range(code.index("{", m.end() - 1), len(code)):
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        body = code[m.start():end]
        order_ok = (body.find("IoDeleteSymbolicLink") <
                    body.find("OpenHandleCount") <
                    body.find("ExFreePoolWithTag") <
                    body.find("IoDeleteDevice"))
    else:
        body, order_ok = "", False
    report.check("卸载顺序：删符号链接 -> 等句柄归零 -> 释放存储 -> IoDeleteDevice",
                 order_ok)
    report.check("DriverUnload 中销毁控制设备（且在 PortCls unload 之后）",
                 (lambda t: t.find("gPCDriverUnloadRoutine(DriverObject)") <
                  t.find("MicLinkDeleteControlDevice()") > 0)(
                      strip_comments(read(os.path.join(MAIN, "adapter.cpp")))))

    hdr = read(os.path.join(INC, "miclink", "miclink_ctl.h"))
    cap = re.search(r"#define\s+MICLINK_RING_CAPACITY_FRAMES\s+\(1u\s*<<\s*(\d+)\)", hdr)
    ok = False
    detail = "未找到容量宏"
    if cap:
        ok = True
        detail = "2^%s = %d 帧" % (cap.group(1), 1 << int(cap.group(1)))
    report.check("环形缓冲帧容量是 2 的幂", ok, detail)
    report.check("MICLINK_FRAME_BYTES == 2（16-bit mono）",
                 any_line(hdr, r"#define\s+MICLINK_FRAME_BYTES\s+2u"))

    mw = code_of(os.path.join(FILTERS, "micarraywavtable.h"))
    report.check("MICARRAY_FRAME_BYTES(2) 与 MICLINK_FRAME_BYTES 一致",
                 any_line(mw, r"#define\s+MICARRAY_FRAME_BYTES\s+2\b"))

    # 重复定义检查：同一个宏在 miclink_ctl.h 里只定义一次
    dupes = []
    for m in re.finditer(r"#define\s+(MICLINK_[A-Z0-9_]+)\b", hdr):
        name = m.group(1)
        if len(re.findall(r"#define\s+%s\b" % name, hdr)) > 1:
            dupes.append(name)
    report.check("miclink_ctl.h 中无重复宏定义", not dupes, "重复: %s" % sorted(set(dupes)))

    # 内核侧绝不调用浮点 DSP（x64 内核 / DPC 下没有 FP 上下文）
    all_code = ""
    for root, _dirs, files in os.walk(SRC):
        for fn in files:
            if fn.endswith((".c", ".cpp")):
                all_code += code_of(os.path.join(root, fn))
    report.check("内核侧不调用浮点 DSP（miclink_apply_gain / miclink_measure_level）",
                 "miclink_apply_gain" not in all_code and
                 "miclink_measure_level" not in all_code)

    # CLASS GUID：头文件 extern 声明 + cpp 里恰好定义一次
    hdr2 = code_of(os.path.join(INC, "miclink", "miclink_ctl.h"))
    cpp2 = code_of(os.path.join(MAIN, "miclinkctl.cpp"))
    report.check("MICLINK_CONTROL_CLASS_GUID 在头文件 extern 声明",
                 "extern const GUID MICLINK_CONTROL_CLASS_GUID" in hdr2)
    report.check("MICLINK_CONTROL_CLASS_GUID 在 miclinkctl.cpp 中恰好定义一次",
                 len(re.findall(r"^\s*const\s+GUID\s+MICLINK_CONTROL_CLASS_GUID\s*=",
                                cpp2, flags=re.M)) == 1)

    # 流格式上报：Init 里上报、析构里回收
    st = code_of(os.path.join(MAIN, "minwavertstream.cpp"))
    report.check("捕获流 Init 向控制设备上报格式（MicLinkNotifyStreamOpened）",
                 "MicLinkNotifyStreamOpened" in st)
    report.check("捕获流析构回收格式（MicLinkNotifyStreamClosed）",
                 "MicLinkNotifyStreamClosed" in st)

    # minipairs：渲染端点数为 0，捕获端点只剩 mic
    mp = code_of(os.path.join(FILTERS, "minipairs.h"))
    report.check("minipairs.h 里 g_cRenderEndpoints == 0",
                 any_line(mp, r"#define\s+g_cRenderEndpoints\s+0\b"))
    report.check("minipairs.h 只保留 MicArray1 捕获端点",
                 "MicArray1Miniports" in mp and "SpeakerMiniports" not in mp)
    report.check("minipairs.h 不再 include speaker 头",
                 not any_line(mp, r'#include\s+"speaker\w*\.h"'))

    # 端点友好名 / mono
    topt = code_of(os.path.join(FILTERS, "micarray1toptable.h"))
    report.check("topology 端点 category 改为 KSNODETYPE_MICROPHONE（单声道）",
                 "KSNODETYPE_MICROPHONE" in topt and
                 "KSNODETYPE_MICROPHONE_ARRAY" not in topt)
    topo = code_of(os.path.join(FILTERS, "micarraytopo.cpp"))
    report.check("mic array geometry 元素数为 1",
                 re.search(r"cElements\s*=\s*1\s*;", topo) is not None)
    report.check("jack description 使用 mono 通道掩码",
                 "KSAUDIO_SPEAKER_MONO" in topo)

    # sln
    sln = read(os.path.join(DRIVER_DIR, "MicLinkVirtualMic.sln"))
    report.check("SLN 仍正确引用 5 个子工程",
                 sln.count(r"Source\Main\Main.vcxproj") == 1 and
                 sln.count("Source\\Filters\\Filters.vcxproj") == 1 and
                 sln.count("Source\\Utilities\\Utilities.vcxproj") == 1 and
                 sln.count("Source\\Inc\\Inc.vcxproj") == 1 and
                 sln.count("Package\\package.VcxProj") == 1)


def check_shared_kernel_shims(report: Report) -> None:
    """17. 共享 C 的内核外壳：先 wdm.h 再 #ifndef _KERNEL_MODE，且路径存在。"""
    for name, target in [("miclink_shared_ring.c", "ring_buffer.c"),
                         ("miclink_shared_dsp.c", "dsp.c")]:
        p = os.path.join(UTIL, name)
        t = read(p)
        i_wdm = t.find("#include <wdm.h>")
        i_if = t.find("#ifndef _KERNEL_MODE")
        i_inc = t.find('#include "..\\..\\..\\shared\\core\\src\\%s"' % target)
        report.check("%s 先包含 wdm.h 再兜底 _KERNEL_MODE" % name,
                     i_wdm >= 0 and i_if > i_wdm)
        report.check("%s 直接 include shared/core/src/%s（无副本）" % (name, target),
                     i_inc >= 0)
        report.check("%s 指向的共享源文件存在" % target,
                     exists(os.path.join(SHARED_SRC, target)))


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #

def run() -> Report:
    r = Report()
    check_files(r)
    check_no_speaker_refs(r)
    check_miclink_ctl_header(r)
    check_header_impl_parity(r)
    check_dispatch_order(r)
    check_pnp_shortcircuit(r)
    check_dispatch_forwarding(r)
    check_datarange_intersection(r)
    check_format_table(r)
    check_no_sine(r)
    check_writebytes(r)
    check_ioctl_coverage(r)
    check_no_shared_copy(r)
    check_inf(r)
    check_vcxproj(r)
    check_misc_constraints(r)
    check_shared_kernel_shims(r)
    return r


def main() -> int:
    ap = argparse.ArgumentParser(description="MICLINK 驱动静态一致性自校验")
    ap.add_argument("--quiet", action="store_true", help="只打印汇总")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    args = ap.parse_args()

    r = run()

    if args.json:
        print(json.dumps(
            [{"name": n, "status": "PASS" if ok else "FAIL", "detail": d}
             for n, ok, d in r.results],
            ensure_ascii=False, indent=2))
        return 0 if r.failed == 0 else 1

    if not args.quiet:
        print("=" * 78)
        print(" MICLINK 内核驱动（T03）静态一致性自校验")
        print(" driver = %s" % DRIVER_DIR)
        print("=" * 78)
        for name, ok, detail in r.results:
            mark = "PASS" if ok else "FAIL"
            line = "[%s] %s" % (mark, name)
            if detail and not ok:
                line += "\n         -> %s" % detail
            elif detail and ok and len(detail) < 60:
                line += "  (%s)" % detail
            print(line)

    print("-" * 78)
    print(" TOTAL: %d   PASS: %d   FAIL: %d" % (len(r.results), r.passed, r.failed))
    print("=" * 78)
    if r.failed:
        print(" RESULT: FAIL")
        return 1
    print(" RESULT: ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
