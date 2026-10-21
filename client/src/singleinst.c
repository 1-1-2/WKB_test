/* singleinst.c — 全局互斥量 + 命令行转发 */
#include "singleinst.h"
#include <string.h>

#define MUTEX_GLOBAL  L"Global\\MicLink.WOMic.SingleInstance"
#define MUTEX_SESSION L"Local\\MicLink.WOMic.SingleInstance"
#define IPC_CLASS     L"MicLinkIpcClass"
#define IPC_TITLE     L"MicLinkIpcWindow"

static HANDLE      g_mutex = NULL;
static HWND        g_ipc   = NULL;
static HINSTANCE   g_hinst = NULL;
static SingleInstCb       g_cb  = NULL;
static SingleInstActivate g_act = NULL;

static LRESULT CALLBACK ipc_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
            const wchar_t *s = (const wchar_t *)cds->lpData;
            if (g_act) g_act();               /* 把已存在的窗口提到前台 */
            if (g_cb) g_cb(s);                /* 让已有实例按新参数行动 */
        }
        return TRUE;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int register_ipc_class(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ipc_proc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = IPC_CLASS;
    return (int)(RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

/* 消息专用窗口（HWND_MESSAGE）：只用来收 WM_COPYDATA，不参与绘制。
 * 它挂在主线程上，主消息循环顺带就把它的消息派发了，不需要额外线程。 */
static int create_ipc_window(void)
{
    if (!register_ipc_class()) return 0;
    g_ipc = CreateWindowExW(0, IPC_CLASS, IPC_TITLE, 0,
                            0, 0, 0, 0, HWND_MESSAGE, NULL, g_hinst, NULL);
    return g_ipc != NULL;
}

int singleinst_init(SingleInstCb cb, SingleInstActivate act)
{
    DWORD err;
    g_cb = cb;
    g_act = act;
    g_hinst = GetModuleHandleW(NULL);

    /* 先试 Global\（跨会话唯一），失败退到会话内名字。非管理员账户一般也有
     * SeCreateGlobalPrivilege，但企业管控机器上可能被收走，所以必须有退路。 */
    g_mutex = CreateMutexW(NULL, FALSE, MUTEX_GLOBAL);
    if (!g_mutex) g_mutex = CreateMutexW(NULL, FALSE, MUTEX_SESSION);
    if (!g_mutex) return -1;
    err = GetLastError();

    if (err == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(IPC_CLASS, IPC_TITLE);
        if (other) {
            LPWSTR cmd = GetCommandLineW();
            COPYDATASTRUCT cds;
            cds.dwData = 1;
            cds.cbData = (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t));
            cds.lpData = cmd;
            /* 同步发送：确保我们退出前对方已收到（SendMessage 跨进程会等待） */
            SendMessageW(other, WM_COPYDATA, (WPARAM)(HWND)NULL, (LPARAM)&cds);
        }
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }

    /* 我们是唯一实例：起一个 IPC 窗口等后来的实例把命令行递过来 */
    create_ipc_window();
    return 1;
}

void singleinst_shutdown(void)
{
    if (g_ipc) { DestroyWindow(g_ipc); g_ipc = NULL; }
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
}
