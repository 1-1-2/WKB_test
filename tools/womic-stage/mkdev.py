#!/usr/bin/env python3
"""纯 ctypes 的 SetupAPI 封装：枚举 / 创建设备节点 / 安装驱动。

为什么不用 devcon 之外的方案
--------------------------
* PowerShell 版（`*-PnpDevice`、`pnputil`）在这条 PTY 通道上会被打断，且后台无输出；
* `devcon.exe` 官方包里混装 i386/x64/ARM64，取错就 `Exec format error`；
* 直接用 SetupAPI 既没有控制台依赖，也不受包内架构混乱影响。

对外接口：
    list_devices(keyword) -> [{"instance_id","desc","status"}]
    install_inf(inf_path, hwid) -> (ok, message)
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
cfgmgr = ctypes.WinDLL("cfgmgr32", use_last_error=True)

DIGCF_PRESENT = 0x02
DIGCF_ALLCLASSES = 0x04
SPDRP_DEVICEDESC = 0x00
SPDRP_HARDWAREID = 0x01
SPDRP_FRIENDLYNAME = 0x0C
DIF_REGISTERDEVICE = 0x19
INSTALLFLAG_FORCE = 0x01
DN_HAS_PROBLEM = 0x00000400
CM_DRP_STATUS = 0x0C


class SP_DEVINFO_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("ClassGuid", ctypes.c_byte * 16),
                ("DevInst", wt.DWORD),
                ("Reserved", ctypes.c_void_p)]


class SP_DEVINSTALL_PARAMS_W(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("Flags", wt.DWORD),
                ("FlagsEx", wt.DWORD),
                ("hwndParent", wt.HWND),
                ("InstallMsgHandler", ctypes.c_void_p),
                ("InstallMsgHandlerContext", ctypes.c_void_p),
                ("FileQueue", ctypes.c_void_p),
                ("ClassInstallReserved", ctypes.c_void_p),
                ("Reserved", ctypes.c_size_t),
                ("DriverPath", ctypes.c_wchar * 260)]


setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.c_void_p, wt.LPCWSTR, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInfo.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.DWORD,
    ctypes.POINTER(wt.DWORD), ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
setupapi.SetupDiGetINFClassW.argtypes = [
    wt.LPCWSTR, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD, ctypes.c_void_p]
setupapi.SetupDiCreateDeviceInfoList.argtypes = [ctypes.c_void_p, wt.HWND]
setupapi.SetupDiCreateDeviceInfoList.restype = wt.HANDLE
setupapi.SetupDiCreateDeviceInfoW.argtypes = [
    wt.HANDLE, wt.LPCWSTR, ctypes.c_void_p, wt.LPCWSTR, wt.HWND, wt.DWORD,
    ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiSetDeviceRegistryPropertyW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.DWORD, ctypes.c_void_p, wt.DWORD]
setupapi.SetupDiCallClassInstaller.argtypes = [
    wt.DWORD, wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiSetSelectedDevice.argtypes = [wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiOpenDevRegKey.restype = wt.HKEY
setupapi.SetupDiOpenDevRegKey.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD]

update_driver = ctypes.WinDLL("newdev", use_last_error=True)
update_driver.UpdateDriverForPlugAndPlayDevicesW.argtypes = [
    wt.HWND, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.POINTER(wt.BOOL)]
update_driver.UpdateDriverForPlugAndPlayDevicesW.restype = wt.BOOL


def _guid_bytes(g: ctypes.c_byte * 16) -> str:
    b = bytes(bytearray(x & 0xFF for x in g))
    return "{%08X-%04X-%04X-%s-%s}" % (
        int.from_bytes(b[0:4], "little"), int.from_bytes(b[4:6], "little"),
        int.from_bytes(b[6:8], "little"), b[8:10].hex().upper(), b[10:16].hex().upper())


def _reg_prop(hdev: wt.HANDLE, did: SP_DEVINFO_DATA, prop: int) -> str:
    buf = ctypes.create_unicode_buffer(1024)
    need = wt.DWORD(0)
    if setupapi.SetupDiGetDeviceRegistryPropertyW(
            hdev, ctypes.byref(did), prop, None, buf, ctypes.sizeof(buf), ctypes.byref(need)):
        return buf.value
    return ""


def list_devices(keyword: str = "WOMic") -> list[dict]:
    """枚举所有已呈现设备，过滤关键字（匹配 DeviceDesc / FriendlyName / InstanceId）。"""
    hdev = setupapi.SetupDiGetClassDevsW(None, None, None, DIGCF_PRESENT | DIGCF_ALLCLASSES)
    if hdev == wt.HANDLE(-1).value or not hdev:
        return []
    out: list[dict] = []
    did = SP_DEVINFO_DATA()
    did.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
    i = 0
    while setupapi.SetupDiEnumDeviceInfo(hdev, i, ctypes.byref(did)):
        i += 1
        desc = _reg_prop(hdev, did, SPDRP_DEVICEDESC)
        friendly = _reg_prop(hdev, did, SPDRP_FRIENDLYNAME)
        hwid = _reg_prop(hdev, did, SPDRP_HARDWAREID)
        inst = ""
        # 实例 ID 通过 CM_Get_Device_IDW 取
        buf = ctypes.create_unicode_buffer(512)
        if cfgmgr.CM_Get_Device_IDW(did.DevInst, buf, 512, 0) == 0:
            inst = buf.value
        blob = " ".join((desc, friendly, hwid, inst)).lower()
        if keyword.lower() in blob:
            status = wt.ULONG(0)
            prob = wt.ULONG(0)
            cfgmgr.CM_Get_DevNode_Status(ctypes.byref(status), ctypes.byref(prob), did.DevInst, 0)
            out.append({"instance_id": inst, "desc": desc, "friendly": friendly,
                        "hwid": hwid, "status": "ok" if (status.value & DN_HAS_PROBLEM) == 0 else f"problem({prob.value})"})
    setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return out


def install_inf(inf_path: str, hwid: str) -> tuple[bool, str]:
    """按 INF 安装设备（等价 devcon install 的核心步骤）。

    步骤：取 INF 的 ClassGUID → 建设备信息集 → 造节点 → 写 HardwareID
          → DIF_REGISTERDEVICE 注册 → UpdateDriverForPlugAndPlayDevices 装驱动。
    """
    inf_path = os.path.abspath(inf_path)
    if not os.path.exists(inf_path):
        return False, f"INF 不存在: {inf_path}"

    guid = ctypes.c_byte * 16
    class_guid = guid()
    class_name = ctypes.create_unicode_buffer(64)
    if not setupapi.SetupDiGetINFClassW(inf_path, ctypes.byref(class_guid), class_name, 64, None):
        return False, f"SetupDiGetINFClass 失败 err={ctypes.get_last_error()}"

    hdev = setupapi.SetupDiCreateDeviceInfoList(ctypes.byref(class_guid), None)
    if not hdev:
        return False, f"SetupDiCreateDeviceInfoList 失败 err={ctypes.get_last_error()}"

    did = SP_DEVINFO_DATA()
    did.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
    if not setupapi.SetupDiCreateDeviceInfoW(hdev, class_name.value, ctypes.byref(class_guid),
                                             None, None, 0x00000001, ctypes.byref(did)):
        return False, f"SetupDiCreateDeviceInfo 失败 err={ctypes.get_last_error()}"

    # 写 HardwareID（多字符串，双 NUL 结尾）
    hw = (hwid + "\0\0").encode("utf-16-le")
    if not setupapi.SetupDiSetDeviceRegistryPropertyW(
            hdev, ctypes.byref(did), SPDRP_HARDWAREID, hw, len(hw)):
        return False, f"写 HardwareID 失败 err={ctypes.get_last_error()}"

    if not setupapi.SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hdev, ctypes.byref(did)):
        return False, f"DIF_REGISTERDEVICE 失败 err={ctypes.get_last_error()}"

    log = []
    log.append(f"Device node created: {hwid} (class={class_name.value})")
    reboot = wt.BOOL(0)
    ok = update_driver.UpdateDriverForPlugAndPlayDevicesW(
        None, hwid, inf_path, INSTALLFLAG_FORCE, ctypes.byref(reboot))
    if not ok:
        err = ctypes.get_last_error()
        # 常见：ERROR_NO_SUCH_DEVINST(0x80070490) 表示节点已存在，交给 UpdateDriver 处理
        return False, f"UpdateDriverForPlugAndPlayDevices 失败 err={err} " \
                      f"(0x{err & 0xFFFFFFFF:08X})；部分情况下设备已注册，可查 list_devices"
    log.append("Drivers installed successfully.")
    return True, "\n".join(log)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "list":
        kw = sys.argv[2] if len(sys.argv) > 2 else "WOMic"
        for d in list_devices(kw):
            print(d)
        sys.exit(0)
    inf = sys.argv[1] if len(sys.argv) > 1 else r"D:\a\_temp\extract\driver\womic.inf"
    hwid = sys.argv[2] if len(sys.argv) > 2 else "*WOMic"
    ok, msg = install_inf(inf, hwid)
    print(("OK: " if ok else "FAIL: ") + msg)
    time.sleep(1)
    for d in list_devices("WOMic"):
        print("  设备:", d)
    sys.exit(0 if ok else 1)
