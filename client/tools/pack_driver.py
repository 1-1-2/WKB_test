#!/usr/bin/env python3
"""驱动包打包器 —— 生成内置驱动资源与图标。

产物（全部为确定性生成物，随本脚本刷新）：
  src/drvpkg_data.h   驱动包的 C 字节数组（deflate 原始流），供 drvpkg.c 与单测使用
  res/drvpkg.bin      同一份字节的二进制副本，供 windres 编进 .rsrc（RT_RCDATA）
  res/app.ico         程序图标（16/32/48 BMP + 256 PNG，全部代码绘制）

内置的是 **三套 2022 版（2.1.0.0）官方驱动**（NTx86 / NTamd64 / NTARM64），
硬件 ID 均为 `Root\\WOMic`，cat 为微软 WHCP 签名。2017 版（womic_3/4/5/6，
硬件 ID `*WOMic`）一律不打包 —— 其握手长度检查与 2022 版不兼容且带内核态落盘。

⚠ 公开源码库不内置第三方已签名驱动二进制（法律风险，架构文档 R8）：
res/drvpkg.bin 与 src/drvpkg_data.h 均已在 .gitignore 中。干净检出时
build.sh 会先尝试真实打包，找不到官方驱动目录就用 `--empty` 生成合法空包
（magic 正确、ver=1、count=0，16 字节）兜底，构建不中断；那种 exe 靠
`--driver-pack <dir>` 外置驱动工作。

用法：
  python3 tools/pack_driver.py                       # 自动寻找驱动目录并生成三个产物
  python3 tools/pack_driver.py --driver-dir <dir>    # 指定驱动目录
  python3 tools/pack_driver.py --empty               # 空包兜底（不含第三方二进制）
  python3 tools/pack_driver.py --check               # 只校验压缩/解压往返一致

数据来源优先级：
  1. --driver-dir 参数
  2. 环境变量 WOMIC_DRIVER_DIR
  3. 内置候选路径列表（womic-recovery 的抽取目录等）
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.dirname(HERE)

OUT_HEADER = os.path.join(CLIENT, "src", "drvpkg_data.h")
OUT_BIN = os.path.join(CLIENT, "res", "drvpkg.bin")
OUT_ICO = os.path.join(CLIENT, "res", "app.ico")

BLOB_MAGIC = b"MICLDRV1"
BLOB_VERSION = 1

# arch id 与架构文档 1.2.2 一致：AMD64→womic_1.*, ARM64→womic_2.*, INTEL→womic.*
# dir 名同时用于 %ProgramData%\MicLink\driver\<dir>\
ARCHS = (
    # (id, dir, 前缀, 说明)
    (1, "x86", "womic", "NTx86 / Intel 32-bit"),
    (2, "amd64", "womic_1", "NTamd64 / x64"),
    (3, "arm64", "womic_2", "NTARM64"),
)
KINDS = ("inf", "sys", "cat")

CANDIDATE_DIRS = (
    "/workspace/womic-recovery/driver-pkg/extract2/driver",
    "/workspace/womic-recon/all/driver",
    os.path.join(CLIENT, "driver"),
    os.path.join(CLIENT, "..", "driver"),
)


# --------------------------------------------------------------------- 工具
def u32le(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def find_driver_dir(explicit: str | None) -> str:
    """定位驱动三件套所在目录。"""
    for d in (explicit, os.environ.get("WOMIC_DRIVER_DIR")):
        if d and os.path.isdir(d):
            return os.path.abspath(d)
    for d in CANDIDATE_DIRS:
        d = os.path.abspath(d)
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "womic_1.inf")):
            return d
    raise SystemExit(
        "找不到驱动目录。请用 --driver-dir 指定包含 womic*.{inf,sys,cat} 的目录，"
        "或设置环境变量 WOMIC_DRIVER_DIR。"
    )


def read_pack(base: str, prefix: str) -> dict:
    """读取一套三件套的原始字节；缺失即报错（宁可构建失败，也不产残缺包）。"""
    out = {}
    for k in KINDS:
        p = os.path.join(base, "%s.%s" % (prefix, k))
        if not os.path.isfile(p):
            raise SystemExit("缺少驱动文件：%s" % p)
        with open(p, "rb") as f:
            out[k] = f.read()
    return out


def build_blob(base: str) -> tuple[bytes, list]:
    """组装二进制包。

    布局（全小端）：
      0   : magic 8B = "MICLDRV1"
      8   : version u32
      12  : entry_count u32
      16  : entry[count]，每项 ENTRY_BYTES=48B
              +0  arch id u32
              +4  inf_raw u32   +8  inf_comp u32   +12 inf_ofs u32
              +16 sys_raw u32   +20 sys_comp u32   +24 sys_ofs u32
              +28 cat_raw u32   +32 cat_comp u32   +36 cat_ofs u32
              +40 reserved u32  +44 reserved u32
      紧随其后为各 deflate 原始流（wbits=-15，无 zlib 头）
    """
    ENTRY_BYTES = 48
    entries, payload = [], b""
    header_len = 16 + ENTRY_BYTES * len(ARCHS)

    packs = {}
    for arch_id, _d, prefix, _desc in ARCHS:
        raw = read_pack(base, prefix)
        comp = {}
        for k in KINDS:
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp[k] = c.compress(raw[k]) + c.flush()
            # 往返自检：deflate 必须能被标准 zlib 还原，否则后面的 C 端 inflate 无从验证
            assert zlib.decompress(comp[k], -15) == raw[k], "deflate 往返失败 %s %s" % (prefix, k)
        packs[arch_id] = (raw, comp, prefix)

    for arch_id, _d, _p, _desc in ARCHS:
        raw, comp, _prefix = packs[arch_id]
        ofs = {}
        for k in KINDS:
            ofs[k] = header_len + len(payload)
            payload += comp[k]
        entries.append((arch_id, raw, comp, ofs))

    blob = BLOB_MAGIC + u32le(BLOB_VERSION) + u32le(len(entries))
    for arch_id, raw, comp, ofs in entries:
        e = u32le(arch_id)
        for k in KINDS:
            e += u32le(len(raw[k])) + u32le(len(comp[k])) + u32le(ofs[k])
        e += u32le(0) + u32le(0)
        assert len(e) == ENTRY_BYTES, len(e)
        blob += e
    blob += payload
    return blob, entries


def build_empty_blob() -> tuple[bytes, list]:
    """组装合法空包：magic + ver=1 + count=0，共 16 字节，无 entry、无 payload。

    用于公开源码库的干净检出 / CI：不内置任何第三方驱动二进制，但包头结构
    完整（C 端 entry_get / drvpkg_available 能正确识别并给出可操作提示，
    parse_and_verify 也能通过）。"""
    return BLOB_MAGIC + u32le(BLOB_VERSION) + u32le(0), []


def parse_and_verify(blob: bytes) -> list:
    """按 C 端同样的解析方式读回，并解压校验（防止生成器与解析器理解不一致）。"""
    assert blob[:8] == BLOB_MAGIC, "magic 不符"
    ver, count = struct.unpack_from("<II", blob, 8)
    assert ver == BLOB_VERSION
    out = []
    for i in range(count):
        base = 16 + i * 48
        arch_id = struct.unpack_from("<I", blob, base)[0]
        item = {"arch": arch_id}
        for j, k in enumerate(KINDS):
            raw_len, comp_len, ofs = struct.unpack_from("<III", blob, base + 4 + j * 12)
            data = zlib.decompress(blob[ofs:ofs + comp_len], -15)
            assert len(data) == raw_len, "%s 解压长度不符" % k
            item[k] = data
        out.append(item)
    return out


# --------------------------------------------------------------------- 产物
def _fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def write_header(blob: bytes, entries: list, empty: bool = False) -> None:
    lines = []
    lines.append("/* src/drvpkg_data.h —— 自动生成，请勿手工修改")
    lines.append(" *")
    if empty:
        lines.append(" * 由 tools/pack_driver.py --empty 生成：空包（16 B，count=0）。")
        lines.append(" * 公开源码库不内置第三方已签名驱动二进制（womic.sys 版权归 Wolicheng")
        lines.append(" * 所有，架构文档 R8）。需要内置驱动时，自备官方驱动目录后重跑：")
        lines.append(" *   python3 tools/pack_driver.py --driver-dir <dir>")
        lines.append(" * 未内置驱动的 exe 通过 --driver-pack <dir> 外置驱动工作。")
    else:
        lines.append(" * 由 tools/pack_driver.py 生成：三套 WO Mic 官方驱动（2022 / 2.1.0.0）")
        lines.append(" * 经 raw deflate（wbits=-15）压缩后内嵌。")
        lines.append(" *")
        lines.append(" * 驱动归属：womic.sys / womic.inf / womic.cat 版权归 Wolicheng（WO Mic）所有，")
        lines.append(" * 版本 2.1.0.0，来源 https://www.wolicheng.com/womic/ 。本项目只是原样再分发，")
        lines.append(" * 未修改、未重新签名。可用 womic.exe --driver-pack <dir> 指定自带驱动包替换。")
    lines.append(" *")
    lines.append(" * 二进制布局见 tools/pack_driver.py::build_blob()。")
    lines.append(" */")
    lines.append("#ifndef MICLINK_DRVPKG_DATA_H")
    lines.append("#define MICLINK_DRVPKG_DATA_H")
    lines.append("")
    lines.append("#define DRVPKG_BLOB_MAGIC \"MICLDRV1\"")
    lines.append("#define DRVPKG_BLOB_LEN %d" % len(blob))
    lines.append("")
    # 解压自检表：C 端单测（tools/test_drvpkg.c）用它校验 inflate 的字节正确性
    lines.append("/* 解压自检表：{arch, kind, 原始长度, FNV-1a-64}，供 tools/test_drvpkg.c 校验 */")
    lines.append("typedef struct { int arch; const char *kind; unsigned long raw_len;"
                 " unsigned long long fnv; } DrvPkgExpect;")
    if entries:
        lines.append("static const DrvPkgExpect drvpkg_expect[] = {")
        for arch_id, _d, _prefix, _desc in ARCHS:
            for k in KINDS:
                data = [it for it in entries if it[0] == arch_id][0][1][k]
                lines.append("    {%d, \"%s\", %dUL, 0x%016XULL}," %
                             (arch_id, k, len(data), _fnv1a64(data)))
        lines.append("};")
        lines.append("#define DRVPKG_EXPECT_N %d" % (len(ARCHS) * len(KINDS)))
    else:
        # C 不允许零长数组；放一个占位项但令 N=0，单测据此识别空包并 SKIP。
        lines.append("/* 空包：没有任何驱动条目。占位一项只是为了避免 C 的零长数组，")
        lines.append(" * DRVPKG_EXPECT_N=0，单测会报告“无内置驱动包”并非零退出。 */")
        lines.append("static const DrvPkgExpect drvpkg_expect[] = { {0, \"\", 0UL, 0ULL} };")
        lines.append("#define DRVPKG_EXPECT_N 0")
    lines.append("")
    lines.append("/* 字节数组只在 MICLINK_DRVPKG_EMBED_ARRAY 打开时才编译进来。")
    lines.append(" * 正式构建走 .rsrc（res/drvpkg.bin，由 windres 编入），不要再放一份到")
    lines.append(" * .rodata —— 两份就是 +100 KB，直接把体积预算撑爆。")
    lines.append(" * 只有 Linux 侧的解压单测（tools/test_drvpkg.c）没有资源可读，才打开它。 */")
    lines.append("#ifdef MICLINK_DRVPKG_EMBED_ARRAY")
    lines.append("static const unsigned char drvpkg_blob[DRVPKG_BLOB_LEN] = {")
    per_line = 20
    for i in range(0, len(blob), per_line):
        chunk = blob[i:i + per_line]
        lines.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("#endif /* MICLINK_DRVPKG_EMBED_ARRAY */")
    lines.append("")
    for arch_id, _d, _prefix, desc in ARCHS:
        lines.append("/* arch %d: %s */" % (arch_id, desc))
    lines.append("#endif /* MICLINK_DRVPKG_DATA_H */")
    os.makedirs(os.path.dirname(OUT_HEADER), exist_ok=True)
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_bin(blob: bytes) -> None:
    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(blob)


# --------------------------------------------------------------------- 图标
def _clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return lo if v < lo else (hi if v > hi else v)


def _rr_sdf(x: float, y: float, x0: float, y0: float, x1: float, y1: float, r: float) -> float:
    """圆角矩形的有向距离（内部为负）。"""
    px = max(x0 + r - x, 0.0, x - (x1 - r))
    py = max(y0 + r - y, 0.0, y - (y1 - r))
    return (px * px + py * py) ** 0.5 - r


def render_icon(size: int) -> list:
    """绘制一个麦克风图标，返回 size 行 RGBA（每行 size 个 4 元组，自上而下）。"""
    s = float(size)
    cx, cy = s / 2.0, s / 2.0
    pad = s * 0.04
    r = s * 0.22
    # 底板：从上到下 #3FB6FF → #0067C4
    top = (0x3F, 0xB6, 0xFF)
    bot = (0x00, 0x67, 0xC4)
    # 麦克风：胶囊 + 支架弧 + 立柱 + 底座
    cap_w = s * 0.20
    cap_x0, cap_x1 = cx - cap_w / 2.0, cx + cap_w / 2.0
    cap_y0, cap_y1 = s * 0.20, s * 0.56
    arc_cy = s * 0.565
    arc_r = s * 0.20
    arc_t = max(1.4, s * 0.055)
    stem_x0, stem_x1 = cx - arc_t / 2.0, cx + arc_t / 2.0
    stem_y0, stem_y1 = arc_cy + arc_r * 0.62, s * 0.795
    base_y0, base_y1 = s * 0.795, s * 0.855
    base_x0, base_x1 = cx - s * 0.155, cx + s * 0.155

    ss = 3  # 3x3 超采样，小尺寸也不糊
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            ar = ag = ab = aa = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    x = px + (sx + 0.5) / ss
                    y = py + (sy + 0.5) / ss
                    # 底板
                    d = _rr_sdf(x, y, pad, pad, s - pad, s - pad, r)
                    if d <= -0.5:
                        cov = 1.0
                    elif d >= 0.5:
                        cov = 0.0
                    else:
                        cov = 0.5 - d
                    t = (y - pad) / max(1.0, s - 2 * pad)
                    br = top[0] + (bot[0] - top[0]) * t
                    bg = top[1] + (bot[1] - top[1]) * t
                    bb = top[2] + (bot[2] - top[2]) * t
                    # 麦克风（白色），同样按覆盖度混合
                    mic = 0.0
                    dm = _rr_sdf(x, y, cap_x0, cap_y0, cap_x1, cap_y1, cap_w / 2.0)
                    mic = max(mic, _clamp(0.5 - dm))
                    dx, dy = x - cx, y - arc_cy
                    dist = (dx * dx + dy * dy) ** 0.5
                    if dy > 0.0 and abs(dist - arc_r) <= arc_t / 2.0 + 0.5:
                        mic = max(mic, _clamp(arc_t / 2.0 + 0.5 - abs(dist - arc_r)))
                    if stem_x0 <= x <= stem_x1 and stem_y0 <= y <= stem_y1:
                        mic = max(mic, 1.0)
                    if base_x0 <= x <= base_x1 and base_y0 <= y <= base_y1:
                        mic = max(mic, 1.0)
                    mic = _clamp(mic)
                    cr = br + (255 - br) * mic
                    cg = bg + (255 - bg) * mic
                    cb = bb + (255 - bb) * mic
                    ar += cr * cov
                    ag += cg * cov
                    ab += cb * cov
                    aa += cov * 255.0
            n = float(ss * ss)
            row += bytes((int(ar / n) & 0xFF, int(ag / n) & 0xFF, int(ab / n) & 0xFF, int(aa / n) & 0xFF))
        rows.append(bytes(row))
    return rows


def _png(size: int, rows: list) -> bytes:
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _bmp_dib(size: int, rows: list) -> bytes:
    """32bpp BGRA，自下而上，附 1bpp AND 掩码。"""
    body = bytearray()
    for r in reversed(rows):  # 自下而上
        for i in range(size):
            rr, gg, bb, aa = r[i * 4:i * 4 + 4]
            body += bytes((bb, gg, rr, aa))
    mask_stride = ((size + 31) // 32) * 4
    body += bytes(mask_stride * size)
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(body), 0, 0, 0, 0)
    return hdr + bytes(body)


def write_ico() -> None:
    sizes = (16, 32, 48)
    images = []
    for s in sizes:
        rows = render_icon(s)
        images.append((s, _bmp_dib(s, rows)))
    png_size = 256
    images.append((png_size, _png(png_size, render_icon(png_size))))

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    ofs = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0,
                           0, 0, 1, 32, len(data), ofs)
        ofs += len(data)
    for _s, data in images:
        out += data
    os.makedirs(os.path.dirname(OUT_ICO), exist_ok=True)
    with open(OUT_ICO, "wb") as f:
        f.write(bytes(out))


# --------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description="打包内置驱动资源与图标")
    ap.add_argument("--driver-dir", default=None, help="包含 womic*.{inf,sys,cat} 的目录")
    ap.add_argument("--empty", action="store_true",
                    help="生成空驱动包（不含第三方二进制），用于干净检出/CI 兜底")
    ap.add_argument("--check", action="store_true", help="只做压缩/解压往返校验")
    a = ap.parse_args()

    if a.empty:
        blob, entries = build_empty_blob()
        print("模式        : 空包（不内置第三方驱动二进制）")
    else:
        base = find_driver_dir(a.driver_dir)
        blob, entries = build_blob(base)
        print("驱动目录 : %s" % base)
    parsed = parse_and_verify(blob)

    if not a.empty:
        total_raw = sum(len(v) for it in parsed for k, v in it.items() if k in KINDS)
        for (arch_id, d, prefix, desc), it in zip(ARCHS, parsed):
            print("  arch %d (%s, %s): inf=%d sys=%d cat=%d" %
                  (arch_id, d, desc, len(it["inf"]), len(it["sys"]), len(it["cat"])))
        print("原始合计 : %d B" % total_raw)
        print("压缩后   : %d B  (%.1f%%)" % (len(blob), 100.0 * len(blob) / max(1, total_raw)))
    else:
        ver, count = struct.unpack_from("<II", blob, 8)
        print("包大小   : %d B（magic=%s ver=%d count=%d）" %
              (len(blob), blob[:8].decode(), ver, count))

    if a.check:
        print("往返校验 : OK")
        return 0

    write_header(blob, entries, empty=a.empty)
    write_bin(blob)
    write_ico()
    print("已生成   : %s" % os.path.relpath(OUT_HEADER, CLIENT))
    print("          %s" % os.path.relpath(OUT_BIN, CLIENT))
    print("          %s (%d B)" % (os.path.relpath(OUT_ICO, CLIENT), os.path.getsize(OUT_ICO)))
    if a.empty:
        print("提示     : 空包构建的 exe 请用 --driver-pack <dir> 指定自带驱动目录")
    return 0


if __name__ == "__main__":
    sys.exit(main())
