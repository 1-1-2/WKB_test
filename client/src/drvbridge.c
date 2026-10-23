/* drvbridge.c — 应用 → womic-drvinst.exe 的转发（实现）
 *
 * 只做三件事：找到安装器、把它按正确姿势拉起来、把退出码带回来。
 * 真正的安装/卸载在 drvinst.c（与安装器共用同一份代码）。
 *
 * ⚠ 传参一律用绝对路径 + 绝对路径安装器：runas 拉起的子进程工作目录可能是
 *   System32，任何相对路径都不可靠。
 */
#include "drvbridge.h"
#include "drvinst.h"
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ERROR_ELEVATION_REQUIRED
#define ERROR_ELEVATION_REQUIRED 740L
#endif

static char g_err[256];

const char *drvbridge_last_error(void) { return g_err; }

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_err, sizeof(g_err) - 1, fmt, ap);
    va_end(ap);
    g_err[sizeof(g_err) - 1] = 0;
}

/* --------------------------------------------------------------- 小工具 */
static int is_file(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return (a != INVALID_FILE_ATTRIBUTES &&
            (a & FILE_ATTRIBUTE_DIRECTORY) == 0) ? 1 : 0;
}

/* 本 exe 所在目录（不含文件名）。取得失败的代价是找不到安装器，直接返回 0。 */
static int self_dir(wchar_t *dir, int cap)
{
    wchar_t self[MAX_PATH];
    wchar_t *cut;
    size_t len;
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return 0;
    self[MAX_PATH - 1] = 0;
    cut = wcsrchr(self, L'\\');
    if (!cut) cut = wcsrchr(self, L'/');
    len = cut ? (size_t)(cut - self) : wcslen(self);
    if (len == 0 || len + 1 > (size_t)cap) return 0;
    memcpy(dir, self, len * sizeof(wchar_t));
    dir[len] = 0;
    return 1;
}

/* --------------------------------------------------------------- 定位 */
int drvbridge_find(wchar_t *path, int cap)
{
    wchar_t buf[MAX_PATH];

    path[0] = 0;

    /* 1) 显式覆盖（调试/自测用：MICLINK_DRVINST 指向某个安装器） */
    if (GetEnvironmentVariableW(DRVINST_ENV_W, buf, MAX_PATH) > 0 && is_file(buf)) {
        wcsncpy(path, buf, (size_t)cap - 1);
        path[cap - 1] = 0;
        return 1;
    }
    /* 2) 与本 exe 同目录（正常部署形态，唯一推荐位） */
    if (self_dir(buf, MAX_PATH)) {
        wchar_t cand[MAX_PATH];
        _snwprintf(cand, MAX_PATH, L"%s\\%s", buf, DRVINST_EXE_W);
        cand[MAX_PATH - 1] = 0;
        if (is_file(cand)) {
            wcsncpy(path, cand, (size_t)cap - 1);
            path[cap - 1] = 0;
            return 1;
        }
    }
    /* 3) 当前工作目录（从别的目录调 womic.exe 时兜一下） */
    if (GetCurrentDirectoryW(MAX_PATH, buf)) {
        wchar_t cand[MAX_PATH];
        _snwprintf(cand, MAX_PATH, L"%s\\%s", buf, DRVINST_EXE_W);
        cand[MAX_PATH - 1] = 0;
        if (is_file(cand)) {
            wcsncpy(path, cand, (size_t)cap - 1);
            path[cap - 1] = 0;
            return 1;
        }
    }
    set_err("未找到驱动安装器 %s（请把它与 %s 放在同一目录，"
            "或单独运行 %s）", DRVINST_EXE_A, "womic.exe", DRVINST_EXE_A);
    return 0;
}

/* --------------------------------------------------------------- 参数 */
/* 一律带 /quiet：应用的输出由应用自己组织，安装器只负责干活与报错，
 * 否则同一份信息会在终端里打两遍。 */
static int build_params(DrvOp op, const wchar_t *pack, wchar_t *params, int cap)
{
    const wchar_t *verb = NULL;
    switch (op) {
    case DRVOP_INSTALL:   verb = L"/install";   break;
    case DRVOP_UNINSTALL: verb = L"/uninstall"; break;
    case DRVOP_STATUS:    verb = L"/status";    break;
    default: return 0;
    }
    if (pack && pack[0])
        _snwprintf(params, cap, L"%s /quiet /driver-pack \"%ls\"", verb, pack);
    else
        _snwprintf(params, cap, L"%s /quiet", verb);
    params[cap - 1] = 0;
    return 1;
}

/* --------------------------------------------------------------- 启动 */
static int start_direct(const wchar_t *exe, const wchar_t *params, HANDLE *ph)
{
    wchar_t cmd[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    _snwprintf(cmd, 1024, L"\"%s\" %s", exe, params);
    cmd[1023] = 0;
    /* bInheritHandles=TRUE：安装器是控制台程序，继承父进程的 stdout/stderr 才能
     * 把结果写进同一个终端（CI 重定向日志文件时也才拿得到它的输出）。
     * 父进程没有控制台时（双击 GUI）它自己开一个控制台，正是想要的效果。 */
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return 0;
    if (pi.hThread) CloseHandle(pi.hThread);
    *ph = pi.hProcess;
    return 1;
}

static int start_elevated(const wchar_t *exe, const wchar_t *params, HANDLE *ph)
{
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 0;
    if (!sei.hProcess) return 0;          /* 被 DDE 等机制接管时拿不到句柄 */
    *ph = sei.hProcess;
    return 1;
}

static int start_child(const wchar_t *exe, const wchar_t *params, HANDLE *ph)
{
    DWORD err;
    *ph = NULL;
    if (start_direct(exe, params, ph)) return 1;
    err = GetLastError();
    if (err != ERROR_ELEVATION_REQUIRED) {
        set_err("无法启动 %s（0x%08lX）", DRVINST_EXE_A, (unsigned long)err);
        return 0;
    }
    /* 本进程没提权，而安装器 manifest 是 requireAdministrator → 弹一次 UAC */
    if (start_elevated(exe, params, ph)) return 1;
    err = GetLastError();
    if (err == ERROR_CANCELLED || err == 0)
        set_err("提权被取消：安装/卸载驱动需要管理员权限");
    else
        set_err("启动 %s 失败（0x%08lX）", DRVINST_EXE_A, (unsigned long)err);
    return 0;
}

/* --------------------------------------------------------------- 对外 */
int drvbridge_run(DrvOp op, const wchar_t *pack, int *exit_code)
{
    wchar_t exe[MAX_PATH], params[512];
    HANDLE h = NULL;
    DWORD code = 0;

    if (exit_code) *exit_code = -1;
    if (!drvbridge_find(exe, MAX_PATH)) return 0;
    if (!build_params(op, pack, params, 512)) {
        set_err("未知的驱动操作");
        return 0;
    }
    if (!start_child(exe, params, &h)) return 0;
    WaitForSingleObject(h, INFINITE);
    if (!GetExitCodeProcess(h, &code)) code = 0;
    CloseHandle(h);
    if (exit_code) *exit_code = (int)code;
    return 1;
}

int drvbridge_launch(DrvOp op, const wchar_t *pack)
{
    wchar_t exe[MAX_PATH], params[512];
    HANDLE h = NULL;

    if (!drvbridge_find(exe, MAX_PATH)) return 0;
    if (!build_params(op, pack, params, 512)) {
        set_err("未知的驱动操作");
        return 0;
    }
    if (!start_child(exe, params, &h)) return 0;
    CloseHandle(h);            /* 不等：GUI 靠定时器复检驱动状态 */
    return 1;
}
