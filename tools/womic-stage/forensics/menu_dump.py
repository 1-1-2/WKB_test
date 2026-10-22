#!/usr/bin/env python3
"""转储 WO Mic 客户端主窗口菜单（win32 GetMenu），并可按文本路径触发菜单项。

用法：
  python menu_dump.py                     # 转储菜单树
  python menu_dump.py "Connection>Connect..."   # 触发菜单项（WM_COMMAND）
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.GetMenu.argtypes = [wt.HWND]
u32.GetMenu.restype = wt.HMENU
u32.GetMenuItemCount.argtypes = [wt.HMENU]
u32.GetMenuItemCount.restype = ctypes.c_int
u32.GetSubMenu.argtypes = [wt.HMENU, ctypes.c_int]
u32.GetSubMenu.restype = wt.HMENU
u32.GetMenuItemID.argtypes = [wt.HMENU, ctypes.c_int]
u32.GetMenuItemID.restype = ctypes.c_uint
u32.GetMenuStringW.argtypes = [wt.HMENU, ctypes.c_uint, wt.LPWSTR, ctypes.c_int, ctypes.c_uint]
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.IsWindowVisible.restype = wt.BOOL
u32.GetDlgCtrlID.argtypes = [wt.HWND]
u32.GetDlgCtrlID.restype = ctypes.c_int
u32.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
u32.SendDlgItemMessageW.argtypes = [wt.HWND, ctypes.c_int, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SetDlgItemTextW.argtypes = [wt.HWND, ctypes.c_int, wt.LPCWSTR]
u32.SetDlgItemTextW.restype = wt.BOOL
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.SendInput.restype = wt.UINT
import time as _t2

MF_BYPOSITION = 0x400
MF_BYCOMMAND = 0
WM_COMMAND = 0x111


def menu_text(hmenu, pos):
    n = u32.GetMenuStringW(hmenu, pos, None, 0, MF_BYPOSITION)
    buf = ctypes.create_unicode_buffer(n + 2)
    u32.GetMenuStringW(hmenu, pos, buf, n + 1, MF_BYPOSITION)
    return buf.value


def find_client_windows():
    """找出本进程组所有 WO Mic 顶层窗口。"""
    out = []
    k32 = ctypes.WinDLL("kernel32")
    target_pid = None
    for p in sys.ps1 if False else []:
        pass
    return out


def enum_client_windows():
    """枚举 WO Mic 客户端进程的全部顶层窗口。"""
    k32 = ctypes.WinDLL("kernel32")
    u32.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    results = []
    _ = k32
    hwnd_main = u32.FindWindowW("WO Mic Window Class", None)
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd_main, ctypes.byref(pid))
    target = pid.value

    @WNDENUMPROC
    def cb(h, l):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == target:
            buf = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, buf, 256)
            cls = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(h, cls, 256)
            vis = u32.IsWindowVisible(h)
            results.append((h, cls.value, buf.value, bool(vis)))
        return True

    u32.EnumWindows(cb, 0)
    for h, cls, title, vis in results:
        print(f"hwnd={h} class={cls!r} title={title!r} visible={vis}")


def find_connect_dialog():
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    found = []

    @WNDENUMPROC
    def cb(h, l):
        cls = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, cls, 64)
        t = ctypes.create_unicode_buffer(64)
        u32.GetWindowTextW(h, t, 64)
        if cls.value == "#32770" and t.value == "Connect" and u32.IsWindowVisible(h):
            found.append(h)
        return True

    u32.EnumWindows(cb, 0)
    return found[-1] if found else None


def dump_dialog():
    """枚举 Connect 对话框的全部子控件：class/title/id。"""
    u32.EnumChildWindows = u32.EnumChildWindows  # noqa: PLW0127
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    dlg = find_connect_dialog()
    if not dlg:
        print("NO_DIALOG")
        return
    print(f"dlg={dlg}")

    @WNDENUMPROC
    def cb(h, l):
        cls = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cls, 256)
        txt = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(h, txt, 256)
        cid = u32.GetDlgCtrlID(h)
        vis = u32.IsWindowVisible(h)
        chk = ""
        if cls.value == "Button" and cid in (251, 252, 253, 254):
            st = u32.SendDlgItemMessageW(dlg, cid, 0x00F0, 0, 0)  # BM_GETCHECK
            chk = f" check={st}"
        print(f"  ctrl id={cid} class={cls.value!r} text={txt.value!r} "
              f"visible={vis}{chk}")
        return True

    u32.EnumChildWindows(dlg, cb, 0)


def set_combo(cid, index):
    """CB_SETCURSEL 选择下拉项并广播选择。"""
    CB_SETCURSEL = 0x14E
    WM_COMMAND = 0x111
    dlg = find_connect_dialog()
    if not dlg:
        print("NO_DIALOG")
        return
    u32.SendDlgItemMessageW(dlg, cid, CB_SETCURSEL, index, 0)
    # 通知对话框选择已变（ComboBox 通知 CBN_SELCHANGE=-1）
    u32.SendMessageW(dlg, WM_COMMAND,
                     (0xFFFF << 16) | 1, 0)  # 通知码 -1(CBN_SELCHANGE), id=1 占位
    print(f"combo {cid} -> index {index}")


def click(cid):
    dlg = find_connect_dialog()
    if not dlg:
        print("NO_DIALOG")
        return
    u32.SendDlgItemMessageW(dlg, cid, 0x00F5, 0, 0)  # BM_CLICK
    print(f"clicked {cid}")


PUL = ctypes.Structure
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]
class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def send_char(ch):
    INPUT_KEYBOARD = 1
    KEYEVENTF_UNICODE = 0x0004
    KEYEVENTF_KEYUP = 0x0002
    def mk(scan, flags):
        i = INPUT()
        i.type = INPUT_KEYBOARD
        i.ki = KEYBDINPUT(0, ord(scan), flags, 0, None)
        return i
    down = mk(ch, KEYEVENTF_UNICODE)
    up = mk(ch, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP)
    arr = (INPUT * 2)(down, up)
    u32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
    u32.SendInput(2, arr, ctypes.sizeof(INPUT))


def type_text(text):
    for ch in text:
        send_char(ch)
        time.sleep(0.01)
    print(f"typed {text!r}")


def press_enter():
    VK_RETURN = 0x0D
    INPUT_KEYBOARD = 1
    i1, i2 = INPUT(), INPUT()
    i1.type = i2.type = INPUT_KEYBOARD
    i1.ki = KEYBDINPUT(VK_RETURN, 0, 0, 0, None)
    i2.ki = KEYBDINPUT(VK_RETURN, 0, 2, 0, None)
    arr = (INPUT * 2)(i1, i2)
    u32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
    u32.SendInput(2, arr, ctypes.sizeof(INPUT))
    print("pressed ENTER")


def clickat(cid):
    """真实鼠标点击控件中心（GetWindowRect + SendInput）。"""
    import time as _t
    dlg = find_connect_dialog()
    if not dlg:
        print("NO_DIALOG")
        return
    hwnd_btn = u32.GetDlgItem(dlg, cid)
    if not hwnd_btn:
        print("NO_CTRL", cid)
        return
    rect = wt.RECT()
    u32.GetWindowRect(hwnd_btn, ctypes.byref(rect))
    x = (rect.left + rect.right) // 2
    y = (rect.top + rect.bottom) // 2
    u32.SetCursorPos(x, y)
    _t.sleep(0.1)
    for down in (0x0002, 0x0004):   # MOUSEEVENTF_LEFTDOWN, LEFTUP
        u32.mouse_event(down, 0, 0, 0, None)
        _t.sleep(0.05)
    print(f"mouse-clicked ctrl {cid} at ({x},{y})")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "type":
        dlg = find_connect_dialog()
        if dlg:
            u32.SetForegroundWindow(dlg)
            time.sleep(0.3)
        type_text(sys.argv[2])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "enter":
        press_enter()
        return
    if len(sys.argv) > 1 and sys.argv[1] == "clickat":
        err = u32.FindWindowW("#32770", "Connection error")
        if err:
            u32.PostMessageW(err, 0x0010, 0, 0)
        clickat(int(sys.argv[2]))
        return
    if len(sys.argv) > 1 and sys.argv[1] == "windows":
        enum_client_windows()
        return
    if len(sys.argv) > 1 and sys.argv[1] == "dlg2":
        # 枚举任意 #32770 对话框子控件：dlg2 <title>
        WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        d = u32.FindWindowW("#32770", sys.argv[2])
        if not d:
            print("NO_DIALOG")
            return

        @WNDENUMPROC
        def cb2(h, l):
            cls = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(h, cls, 256)
            txt = ctypes.create_unicode_buffer(1024)
            u32.GetWindowTextW(h, txt, 1024)
            print(f"  id={u32.GetDlgCtrlID(h)} class={cls.value!r} text={txt.value!r} "
                  f"vis={u32.IsWindowVisible(h)}")
            return True

        u32.EnumChildWindows(d, cb2, 0)
        return
    if len(sys.argv) > 1 and sys.argv[1] == "dlg":
        dump_dialog()
        return
    if len(sys.argv) > 1 and sys.argv[1] == "combo":
        set_combo(int(sys.argv[2]), int(sys.argv[3]))
        return
    if len(sys.argv) > 1 and sys.argv[1] == "radio":
        # 先关掉可能存在的错误对话框
        err = u32.FindWindowW("#32770", "Connection error")
        if err:
            u32.PostMessageW(err, 0x0010, 0, 0)  # WM_CLOSE
        # 选 radio：BM_SETCHECK + 向对话框发 BN_CLICKED 通知
        dlg = find_connect_dialog()
        if not dlg:
            print("NO_DIALOG")
            return
        rid = int(sys.argv[2])
        for cid in (251, 252, 253, 254):
            u32.SendDlgItemMessageW(dlg, cid, 0x00F1, 0, 0)   # BM_SETCHECK BST_UNCHECKED
        u32.SendDlgItemMessageW(dlg, rid, 0x00F1, 1, 0)       # BM_SETCHECK BST_CHECKED
        hwnd_btn = u32.GetDlgItem(dlg, rid)
        u32.SendMessageW(dlg, 0x111, (0 << 16) | rid, hwnd_btn)  # WM_COMMAND BN_CLICKED
        print(f"radio {rid} set+notified")
        return
    if len(sys.argv) > 1 and sys.argv[1] == "settext":
        dlg = find_connect_dialog()
        ok = u32.SetDlgItemTextW(dlg, int(sys.argv[2]), sys.argv[3])
        print(f"settext {sys.argv[2]}={sys.argv[3]!r} ok={ok}")
        return
    if len(sys.argv) > 1 and sys.argv[1] == "click":
        click(int(sys.argv[2]))
        return
    hwnd = u32.FindWindowW("WO Mic Window Class", None)
    if not hwnd:
        print("NO_WINDOW")
        return
    print(f"hwnd={hwnd}")
    if len(sys.argv) < 2:
        hmenu = u32.GetMenu(hwnd)
        print(f"menu={hmenu}")

        def walk(h, depth, path):
            n = u32.GetMenuItemCount(h)
            for i in range(n):
                t = menu_text(h, i)
                sub = u32.GetSubMenu(h, i)
                mid = u32.GetMenuItemID(h, i)
                print("  " * depth + f"[{i}] text={t!r} id={mid if sub is None else '(submenu)'}")
                if sub:
                    walk(sub, depth + 1, path + [t])

        if hmenu:
            walk(hmenu, 0, [])
        else:
            print("NO_MENU_BAR")
        return

    # 触发模式：按 '>' 分隔的菜单路径
    hmenu = u32.GetMenu(hwnd)
    target = sys.argv[1].split(">")
    cur = hmenu
    cmd_id = None
    for i, name in enumerate(target):
        n = u32.GetMenuItemCount(cur)
        found = None
        for j in range(n):
            t = menu_text(cur, j).replace("&", "")
            if name.lower() in t.lower():
                sub = u32.GetSubMenu(cur, j)
                if i == len(target) - 1:
                    cmd_id = u32.GetMenuItemID(cur, j)
                else:
                    cur = sub
                found = True
                break
        if not found:
            print(f"MENU_NOT_FOUND: {name}")
            return
    if cmd_id is not None and cmd_id != 0xFFFFFFFF:
        print(f"POST WM_COMMAND id={cmd_id}")
        u32.PostMessageW(hwnd, WM_COMMAND, cmd_id, 0)
    else:
        print("ITEM_NOT_COMMAND")


if __name__ == "__main__":
    main()
