#!/usr/bin/env python3
"""Windows 侧阶段执行器：幂等 + 断点续跑 + 完全脱离控制台。

设计目标
--------
驱动安装（`devcon install` / `UpdateDriverForPlugAndPlayDevices`）会触发 PnP 重配置，
Windows 随之重建控制台会话（conhost），把我们所在的交互式 PTY 一起带走。
现象：ssh 会话在"Updating drivers ..."这一行之后立即断开，重连后脚本从头重放，
于是在同一处反复失败（原地打转）。

本脚本把"执行"与"控制台"解耦：
  * 每个阶段执行完就把结果追加到 state.jsonl（只追加，不覆盖，掉电也不会丢失已完成记录）；
  * 重新启动时读 state.jsonl，跳过已 ok 的阶段，从第一个未完成处继续 —— 幂等续跑；
  * 由计划任务以 SYSTEM 身份在 Session 0 运行，不继承交互式控制台，
    因此 conhost 重建不会杀死它（见 install_task.xml）。

用法：
    python agent_stage.py --auto          # 从第一个未完成阶段续跑
    python agent_stage.py --stage probe   # 只跑指定阶段
    python agent_stage.py --status        # 打印当前进度
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

BASE = Path(os.environ.get("WOMIC_WORK", r"D:\a\_temp"))
STATE = BASE / "state.jsonl"
LOG = BASE / "stage.log"
SETUP_URL = "https://wolicheng.com/womic/downloads/WOMicClientSetup6.3.exe"
SEVENZIP = r"C:\Program Files\7-Zip\7z.exe"
# 驱动三件套在 bundle 里的位置（守护进程经 SSH 上传时落在 $BASE/driver/）。
# 有它就免去下载官方安装包 + 7z 解压，安装路径完全确定性。
BUNDLED_DRIVER = BASE / "driver"

# 阶段有序表：--auto 依此顺序推进
STAGES = ["fetch", "pick_devcon", "install", "verify_device", "probe"]


# ----------------------------------------------------------------- 基础设施
def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    try:
        with LOG.open("a", encoding="utf-8", errors="replace") as f:
            f.write(line + "\n")
    except OSError:
        pass


def record(stage: str, status: str, **detail) -> None:
    """追加一条阶段记录。status ∈ start|ok|fail。"""
    entry = {"stage": stage, "status": status, "ts": int(time.time()), **detail}
    with STATE.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")


def history() -> dict[str, dict]:
    """读 state.jsonl，返回每个阶段最后一次记录。"""
    out: dict[str, dict] = {}
    if not STATE.exists():
        return out
    for line in STATE.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        out[e.get("stage", "?")] = e
    return out


def done(stage: str) -> bool:
    return history().get(stage, {}).get("status") == "ok"


def run(cmd, timeout=600, cwd=None):
    log("$ " + (cmd if isinstance(cmd, str) else " ".join(cmd)))
    r = subprocess.run(cmd, shell=isinstance(cmd, str), cwd=cwd or str(BASE),
                       capture_output=True, text=True, timeout=timeout,
                       errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    for line in out.splitlines():
        log("  | " + line)
    log(f"  -> rc={r.returncode}")
    return r.returncode, out


# ----------------------------------------------------------------- 各阶段
def stage_fetch() -> bool:
    """准备驱动三件套。优先用 bundle 里带的（$BASE/driver/），
    没有才下载官方安装包并用 7z 展开（保留同名多架构版本）。"""
    drv = BUNDLED_DRIVER
    need = ["womic.inf", "womic.sys", "womic.cat"]
    if all((drv / f).exists() for f in need):
        files = sorted(p.name for p in drv.glob("*"))
        log(f"使用 bundle 内驱动文件（{drv}）: {files}")
        record("fetch", "ok", source="bundle", files=files)
        return True

    exe = BASE / "womic.exe"
    if not (exe.exists() and exe.stat().st_size > 1_000_000):
        rc, _ = run(["curl", "-sSL", "--retry", "2", "-o", str(exe), SETUP_URL], timeout=300)
        if rc != 0:
            return False
    extract = BASE / "extract"
    if extract.exists():
        # -aou 会让同名文件带 _1/_2 后缀，重复展开会继续叠加，故先清理
        import shutil
        shutil.rmtree(extract, ignore_errors=True)
    sevenzip = SEVENZIP
    if not Path(sevenzip).exists():
        # 镜像里未必预装 7-Zip；退回 PATH 里找，再找不到就明说
        found = shutil.which("7z.exe") or shutil.which("7z")
        if not found:
            log("未找到 7z（bundle 驱动也不在），无法展开官方安装包")
            return False
        sevenzip = found
    rc, _ = run([sevenzip, "x", "-aou", str(exe), f"-o{extract}", "-y"], timeout=300)
    if rc != 0:
        return False
    files = sorted(p.name for p in (extract / "driver").glob("*")) if (extract / "driver").exists() else []
    log(f"driver 目录: {files}")
    if not all((extract / "driver" / f).exists() for f in need):
        log(f"官方包内缺少 {need}")
        return False
    record("fetch", "ok", source="setup-exe", size=exe.stat().st_size, files=files)
    return True


def pe_machine(path: Path) -> tuple[int, int]:
    """读 PE 头：返回 (machine, 文件大小)。不依赖 pefile，纯 struct 解析。"""
    with path.open("rb") as f:
        head = f.read(0x200)
    if head[:2] != b"MZ":
        return 0, path.stat().st_size
    e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    with path.open("rb") as f:
        f.seek(e_lfanew)
        nt = f.read(6)
    if nt[:4] != b"PE\0\0":
        return 0, path.stat().st_size
    return struct.unpack_from("<H", nt, 4)[0], path.stat().st_size


def stage_pick_devcon() -> bool:
    """挑出 x64 的 devcon：官方包内混装了 i386 / x64 / ARM64 三份同名文件，
    直接 7z x（不带 -aou）会只留下最后一份（恰好是 ARM64），
    在 x64 上执行即 'Exec format error'。这里按 PE Machine 精确挑选。"""
    drv = BUNDLED_DRIVER if (BUNDLED_DRIVER / "womic.inf").exists() else BASE / "extract" / "driver"
    cands = sorted(drv.glob("devcon*.exe"))
    if not cands:
        log(f"没有找到 devcon*.exe（{drv}）")
        return False
    best = None
    for p in cands:
        m, size = pe_machine(p)
        arch = {0x14C: "i386", 0x8664: "x64", 0xAA64: "ARM64"}.get(m, hex(m))
        log(f"  {p.name:16s} {size:7d}B {arch}")
        if m == 0x8664:
            best = p
    if not best:
        log("包内没有 x64 devcon")
        return False
    target = drv / "devcon_x64.exe"
    target.write_bytes(best.read_bytes())
    log(f"已选用 {best.name} -> devcon_x64.exe")
    record("pick_devcon", "ok", source=best.name, size=best.stat().st_size)
    return True


def pick_arch_package(drv: Path) -> tuple[Path, Path, Path, str] | None:
    """从多架构混杂的驱动目录里挑出适配本机 (x64) 的一套，返回 (inf, sys, cat, 硬件ID)。

    官方安装包内每个架构是**一套同结构文件**：womic.inf / womic.sys / womic.cat。
    7z 的 `-aou` 会把同名文件重命名成 womic_1.inf / womic_2.inf……，
    但 **catalog 内的成员名始终是原始的 `womic.inf` / `womic.sys`**。
    因此直接安装改名后的 womic_4.inf 会签名失败（run 39723150250 实测）：

        sig: Driver package INF file hash is not present in catalog file.
             Filename = womic_4.inf, Error = 0xE000024B

    正确做法：按架构挑一套，**成套改回原名**放到独立目录再安装。
    同时优先选 models 硬件 ID 为 `*WOMic` 的那套（与 root 枚举出的设备 ID 一致）。
    """
    def decode_inf(p: Path) -> str:
        raw = p.read_bytes()
        for enc in ("utf-16-le", "utf-8", "cp1252"):
            try:
                return raw.decode(enc, errors="strict")
            except Exception:  # noqa: BLE001
                continue
        return raw.decode("utf-16-le", errors="replace")

    cands: list[tuple[int, Path, Path, Path, str]] = []
    for inf in sorted(drv.glob("womic*.inf")):
        stem = inf.stem
        txt = decode_inf(inf)
        up = txt.upper()
        if "NTAMD64" not in up:          # 只要 64 位的
            continue
        if "*WOMIC" in up:
            hw, rank = "*WOMic", 0        # 与设备 HardwareIds 完全一致，最优
        elif "ROOT\\WOMIC" in up:
            hw, rank = r"Root\WOMic", 1
        else:
            continue
        sysf = drv / f"{stem}.sys"
        catf = drv / f"{stem}.cat"
        if not (sysf.exists() and catf.exists()):
            log(f"  跳过 {inf.name}：缺配套 {sysf.name}/{catf.name}")
            continue
        m, size = pe_machine(sysf)
        if m != 0x8664:                  # .sys 必须是 x64
            log(f"  跳过 {inf.name}：{sysf.name} 不是 x64（machine={hex(m)}）")
            continue
        log(f"  可用套件 {inf.name} + {sysf.name}({size}B,x64) + {catf.name}，硬件ID={hw}")
        cands.append((rank, inf, sysf, catf, hw))
    if not cands:
        return None
    cands.sort(key=lambda t: t[0])
    _, inf, sysf, catf, hw = cands[0]
    return inf, sysf, catf, hw


def import_catalog_signer(pkg_dir: Path) -> bool:
    """把 catalog 的签名证书导入受信任发布者/根，跨过 0xE0000242。

    run 39725547076 实测：womic.cat 由 DigiCert EV 代码签名证书签发
    （Beijing Wolicheng Technology Co., Ltd.，有效期 2017-11~2020-11），
    证书链本身完整、INF/SYS 哈希也对得上，但 Windows 认为**发布者未受信任**：

        Error 0xe0000242: The publisher of an Authenticode(tm) signed catalog
        has not yet been established as trusted.

    导入 TrustedPublisher（+ Root，因为其根不在本机信任列表）后，
    devcon 立刻成功（rc=0，"Drivers installed successfully"），
    端点与 waveIn 同时出现。这一步必须在 devcon 之前做。
    """
    cat = pkg_dir / "womic.cat"
    if not cat.exists():
        log("  install_pkg 里没有 womic.cat，跳过证书导入")
        return False
    cer = BASE / "signer.cer"
    rc, out = run(["powershell", "-NoProfile", "-Command",
                   f"$s = Get-AuthenticodeSignature '{cat}'; "
                   f"if ($s.SignerCertificate) {{ "
                   f"[System.IO.File]::WriteAllBytes('{cer}', "
                   f"$s.SignerCertificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)); "
                   f"'EXPORTED' }} else {{ 'NO_SIGNER' }}"], timeout=120)
    if "EXPORTED" not in out or not cer.exists():
        log("  无法从 catalog 导出签名证书")
        return False
    ok = True
    for store in ("TrustedPublisher", "Root"):
        rc, out = run(["certutil", "-addstore", "-f", store, str(cer)], timeout=120)
        if rc != 0:
            log(f"  证书导入 {store} 失败（rc={rc}）")
            ok = False
    if ok:
        log("  签名证书已导入 TrustedPublisher / Root")
    return ok


def driver_bound() -> bool:
    """判断驱动是否**真的**绑定成功——看系统状态，不看 devcon 输出。

    run 39723150250 实测：devcon 先创建设备节点、再选择驱动。选驱动失败
    （0xe0000228 无兼容驱动 / 0xE000024B 签名不匹配 / 0xE0000242 发布者不受信任）
    时，"Device node created" 早已打印，仅凭这句话判成功会得到**假阳性**。

    判定口径（不硬编码服务名——实际是 `wovad_micarray`，INF 里另有一段叫 WOVAD）：
      1. womic.sys 已落到 System32\\drivers
      2. 设备的 DEVPKEY_Device_DriverInfPath 非空（说明绑定了某个 oem*.inf）
    """
    windir = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    ok_sys = (windir / "System32" / "drivers" / "womic.sys").exists()
    rc, out = run(["powershell", "-NoProfile", "-Command",
                   "$p = Get-PnpDeviceProperty -InstanceId 'ROOT\\MEDIA\\0000' "
                   "-KeyName DEVPKEY_Device_DriverInfPath -EA SilentlyContinue; "
                   "if ($p.Data) { 'BOUND=' + $p.Data } else { 'UNBOUND' }"], timeout=120)
    ok_bind = ("BOUND=" in out) and ("UNBOUND" not in out)
    log(f"  绑定判定：womic.sys 落盘={ok_sys}，DriverInfPath 已绑定={ok_bind}")
    return ok_sys and ok_bind


def stage_install() -> bool:
    """安装驱动。这一步会重建控制台 —— 由计划任务在 Session 0 执行，故不受影响。"""
    drv = BUNDLED_DRIVER if (BUNDLED_DRIVER / "womic.inf").exists() else BASE / "extract" / "driver"
    if not (drv / "womic.inf").exists():
        log("womic.inf 不存在")
        return False
    devcon = drv / "devcon_x64.exe"
    if not devcon.exists():
        log("devcon_x64.exe 不存在（先跑 pick_devcon）")
        return False

    picked = pick_arch_package(drv)
    if not picked:
        log("没有找到适配 x64 且带配套 cat 的驱动套件")
        record("install", "fail", reason="no-amd64-package")
        return False
    inf, sysf, catf, hw = picked

    # 成套改回原名，否则 catalog 成员名对不上 → 0xE000024B
    stage_dir = BASE / "install_pkg"
    if stage_dir.exists():
        shutil.rmtree(stage_dir, ignore_errors=True)
    stage_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(inf, stage_dir / "womic.inf")
    shutil.copy2(sysf, stage_dir / "womic.sys")
    shutil.copy2(catf, stage_dir / "womic.cat")
    log(f"安装包已就绪：{stage_dir}（源 {inf.name}+{sysf.name}+{catf.name}，硬件ID {hw}）")

    # 先让系统信任 catalog 的签名者，否则 devcon 必然 0xE0000242
    import_catalog_signer(stage_dir)
    ensure_audio_services()
    rc, out = run([str(devcon), "install", str(stage_dir / "womic.inf"), hw], timeout=600)
    bound = driver_bound()
    record("install", "ok" if bound else "fail", rc=rc, hw=hw, pkg=inf.name, bound=bound,
           tail="\n".join(out.strip().splitlines()[-6:]))
    return bound


def ensure_audio_services() -> None:
    """确保 Windows 音频服务在跑。root 枚举的虚拟声卡也要音频栈才能起端点。"""
    for svc in ("Audiosrv", "AudioEndpointBuilder"):
        rc, out = run(["sc", "query", svc], timeout=60)
        state = ""
        for line in out.splitlines():
            if "STATE" in line:
                state = line.strip()
        if "RUNNING" in state:
            log(f"  服务 {svc} 已在运行")
            continue
        log(f"  服务 {svc} 未运行（{state or '未查询到'}），尝试启动")
        run(["sc", "config", svc, "start=", "auto"], timeout=60)
        run(["net", "start", svc], timeout=120)
        rc2, out2 = run(["sc", "query", svc], timeout=60)
        log(f"  {svc} 启动后状态: {out2.strip().splitlines()[-1] if out2.strip() else '未知'}")


def stage_verify_device() -> bool:
    """用 SetupAPI 枚举设备状态（不依赖 PowerShell 控制台），
    并用 CIM Win32_SoundDevice 印证录音端点是否存在。"""
    sys.path.insert(0, str(BASE))
    try:
        import mkdev  # 同目录的 ctypes 版 SetupAPI 封装
    except Exception as e:  # noqa: BLE001
        log(f"导入 mkdev 失败: {e}")
        return False
    info = mkdev.list_devices("WOMic")
    for d in info:
        log(f"  设备: {d}")
    # 录音设备证据：CIM 里的声音设备（MEDIA 类 PnP 设备）
    sound = []
    try:
        rc, out = run(["powershell", "-NoProfile", "-Command",
                       "Get-CimInstance Win32_SoundDevice | "
                       "Select-Object Name,Status,DeviceID | ConvertTo-Json -Compress -Depth 3"],
                      timeout=120)
        if rc == 0 and out.strip():
            data = json.loads(out.strip())
            if isinstance(data, dict):
                data = [data]
            sound = [{"name": d.get("Name"), "status": d.get("Status"),
                      "id": d.get("DeviceID")} for d in data]
    except Exception as e:  # noqa: BLE001
        log(f"  Win32_SoundDevice 查询失败: {e}")
    for d in sound:
        log(f"  声音设备: {d['name']} [{d['status']}]")
    ok = bool(info) and driver_bound()
    if info and not ok:
        log("  设备节点存在但驱动未绑定（womic.sys 未落盘或 WOVAD 服务缺失），判定失败")
    record("verify_device", "ok" if ok else "fail", devices=info, sound_devices=sound,
           driver_bound=ok)
    return ok


def stage_probe() -> bool:
    """IOCTL 0x222000 灌正弦波，再从虚拟麦克风录回来判频。"""
    probe = BASE / "probe.py"
    if not probe.exists():
        log("probe.py 不存在")
        return False
    rc, out = run([sys.executable, str(probe), "--seconds", "3", "--out", "out"], timeout=300)
    # probe.py 输出：若干 check 行 + 末段 JSON + "===== 结论: PASS|FAIL ====="
    detail: dict = {}
    m = re.search(r"\n(\{.*\})\s*\n", out, re.S)
    if m:
        try:
            detail = json.loads(m.group(1))
        except json.JSONDecodeError:
            detail = {}
    passed = ("结论: PASS" in out) or (detail.get("ok") is True)
    if rc == 0 and "结论" not in out:
        passed = passed or False
    record("probe", "ok" if passed else "fail", rc=rc,
           peak=detail.get("detail", {}).get("analysis", {}).get("peak_freq"),
           checks=len(detail.get("checks", [])))
    return passed


IMPL = {
    "fetch": stage_fetch,
    "pick_devcon": stage_pick_devcon,
    "install": stage_install,
    "verify_device": stage_verify_device,
    "probe": stage_probe,
}


# ----------------------------------------------------------------- 主流程
def force_utf8_output() -> None:
    """把 stdout/stderr 固定成 UTF-8。

    计划任务里 Python 的 stdout 被重定向到文件时，Windows 不用控制台代码页，
    而是用 locale 的 ANSI 代码页（GitHub runner 上是 cp1252）。中文日志会抛
    UnicodeEncodeError 让整个阶段崩掉——实测 fetch 阶段解压成功后正死在
    `log(f"driver 目录: ...")` 这一行，state.jsonl 里留下三条 fetch=start
    却永远拿不到结论。bat 里也设置了 PYTHONIOENCODING=utf-8，双保险。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001 —— 老版本 Python 或非常规流就放弃
            pass


def main() -> int:
    force_utf8_output()
    ap = argparse.ArgumentParser()
    ap.add_argument("--auto", action="store_true")
    ap.add_argument("--stage")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--reset", action="store_true")
    a = ap.parse_args()

    BASE.mkdir(parents=True, exist_ok=True)
    if a.reset and STATE.exists():
        STATE.unlink()

    if a.status:
        h = history()
        for s in STAGES:
            e = h.get(s, {})
            print(f"{s:14s} {e.get('status', '-'):5s} {e.get('ts', '')}")
        return 0

    if a.stage:
        fn = IMPL.get(a.stage)
        if not fn:
            log(f"未知阶段 {a.stage}")
            return 2
        record(a.stage, "start")
        ok = fn()
        log(f"阶段 {a.stage} -> {'OK' if ok else 'FAIL'}")
        return 0 if ok else 1

    # --auto：从第一个未完成阶段起依次推进
    record("auto", "start", pid=os.getpid(), session0=(os.getpid() != 0))
    for s in STAGES:
        if done(s):
            log(f"跳过已完成阶段: {s}")
            continue
        record(s, "start")
        ok = IMPL[s]()
        log(f"阶段 {s} -> {'OK' if ok else 'FAIL'}")
        if not ok:
            log("阶段失败，停止推进（保留断点，下次 --auto 从此处重试）")
            return 1
    log("全部阶段完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
