#!/usr/bin/env python3
"""在 runner 上反汇编 womic.sys：DriverEntry 的 MajorFunction 表 + 关键分发例程。"""
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, r"D:\a\_temp")

import pefile  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64  # noqa: E402

PE = r"D:\a\_temp\install_pkg\womic.sys"
pe = pefile.PE(PE)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__

print(f"ImageBase={hex(base)} SizeOfImage={hex(pe.OPTIONAL_HEADER.SizeOfImage)}")
ep = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
print(f"EntryPoint={hex(ep)}")

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False


def disasm(va, count=400, maxbytes=0x800):
    rva = va - base
    foff = pe.get_offset_from_rva(rva)
    code = data[foff:foff + maxbytes]
    out = []
    for ins in md.disasm(code, va):
        out.append(f"  {ins.address:#x}  {ins.mnemonic} {ins.op_str}")
        if len(out) >= count:
            break
        if ins.mnemonic == "ret" and len(out) > 20:
            break
    return "\n".join(out)


print("\n===== DriverEntry（找 MajorFunction 填表）=====")
de = disasm(ep, count=400, maxbytes=0x1000) + "\n" + disasm(0x14000209c, count=400, maxbytes=0x1000)
print(de)

# 从 DriverEntry 反汇编里抽 MajorFunction 写入：mov [rax+0x70+...]
import re  # noqa: E402
mj = {}
for line in de.splitlines():
    m = re.search(r"\+ (0x[0-9a-f]+)\], (\w+)$", line)
    if m and "mov" in line:
        off = int(m.group(1), 16)
        if 0x70 <= off <= 0x70 + 27 * 8 and (off - 0x70) % 8 == 0:
            idx = (off - 0x70) // 8
            mj[idx] = m.group(2)
print("\n===== MajorFunction 表 =====")
NAMES = {0: "CREATE", 2: "CLOSE", 3: "READ", 4: "WRITE", 14: "DEVICE_CONTROL",
         18: "CLEANUP", 19: "QUERY_TEARDOWN" if False else "PnP"}
for idx in sorted(mj):
    print(f"  [{idx:2d}] {NAMES.get(idx, '?'):16s} {mj[idx]}")
print("\n===== 关键例程反汇编 =====")
for idx in sorted(mj):
    if idx in (0, 2, 3, 4, 14, 18):
        va = int(mj[idx], 16)
        print(f"\n--- IRP_MJ_{NAMES.get(idx, idx)} handler @ {hex(va)} ---")
        print(disasm(va, count=150, maxbytes=0x600))
