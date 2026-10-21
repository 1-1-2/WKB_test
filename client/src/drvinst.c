/* drvinst.c — 驱动检测 / 安装 / 卸载
 *
 * 三道坑（架构文档 1.2，都踩过）：
 *   1. 架构/硬件 ID 不匹配 → 0xe0000228。用 GetNativeSystemInfo() 选包，
 *      硬件 ID 固定 Root\WOMic（2022 版），2017 版的 *WOMic 一律不用。
 *   2. INF+SYS+CAT 必须成套改回原名 womic.inf/sys/cat → 否则 0xE000024B
 *      （CAT 里是按文件名索引的哈希）。
 *   3. 签名者证书要导入 TrustedPublisher（用 crypt32 API，不派生 certutil）
 *      → 否则 0xE0000242。
 *
 * 判定成功**不看安装工具输出**：devcon 会先打 "Device node created" 再选驱动，
 * 失败也是这句。只看 L1/L2/L3。
 */
#include "drvinst.h"
#include "drvpkg.h"
#include "devio.h"
#include <setupapi.h>
#include <devpkey.h>
#include <newdev.h>
#include <wincrypt.h>
#include <winuser.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GUID_DEVCLASS_MEDIA —— 这里自己定义，避免依赖 devguid.h 在各版本 mingw 里的
 * 可用性；值与 Windows SDK 完全一致。 */
static const GUID K_GUID_MEDIA = {
    0x4d36e96cu, 0xe325u, 0x11ceu,
    { 0xbfu, 0xc1u, 0x08u, 0x00u, 0x2bu, 0xe1u, 0x03u, 0x18u }
};

#define MEDIA_CLASS_KEY \
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e96c-e325-11ce-bfc1-08002be10318}"

static wchar_t g_errw[512];
static char    g_err[512];
static wchar_t g_extpack[MAX_PATH];   /* --driver-pack <dir> */
static wchar_t g_lastdir[MAX_PATH];   /* 本次释放目录，成功后清理 */

const char *drv_last_error(void)     { return g_err; }
const wchar_t *drv_last_error_w(void) { return g_errw; }

static void set_errw(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_errw, 511, fmt, ap);
    va_end(ap);
    g_errw[511] = 0;
    WideCharToMultiByte(CP_UTF8, 0, g_errw, -1, g_err, sizeof(g_err), NULL, NULL);
}

/* ------------------------------------------------------------ 提权判定 */
int drv_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION te;
    DWORD sz = 0;
    int ok = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz))
        ok = te.TokenIsElevated ? 1 : 0;
    CloseHandle(tok);
    return ok;
}

void drv_set_external_pack(const wchar_t *dir)
{
    if (dir) wcsncpy(g_extpack, dir, MAX_PATH - 1);
    else g_extpack[0] = 0;
}

const wchar_t *drv_external_pack(void) { return g_extpack[0] ? g_extpack : NULL; }

/* ------------------------------------------------------------ 小工具 */
static int wcsistr(const wchar_t *hay, const wchar_t *needle)
{
    size_t i, n = wcslen(needle);
    if (!n) return 1;
    for (i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < n && hay[i + j] && towlower(hay[i + j]) == towlower(needle[j])) j++;
        if (j == n) return 1;
    }
    return 0;
}

static int reg_get_sz(HKEY k, const wchar_t *name, wchar_t *out, int cap)
{
    DWORD type = 0, sz = (DWORD)(cap * sizeof(wchar_t));
    out[0] = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS) return 0;
    out[cap - 1] = 0;
    return (type == REG_SZ || type == REG_EXPAND_SZ) ? 1 : 0;
}

/* --------------------------------------------------- L2：注册表 InfPath
 * DEVPKEY_Device_DriverInfPath 的同一份数据就存在这里：
 *   HKLM\SYSTEM\CCS\Control\Class\{MEDIA}\NNNN\InfPath
 * 直接读注册表比 SetupDiGetDeviceProperty + devpkey.h 更省心（不依赖 DEVPROPKEY
 * 在运行期库里的可用性），且与 diagnosis.md:172 用过的判据同源。 */
static int scan_class_registry(DrvStatus *out)
{
    HKEY root = NULL, sub = NULL;
    DWORD i;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MEDIA_CLASS_KEY, 0, KEY_READ, &root)
        != ERROR_SUCCESS)
        return 0;
    for (i = 0; i < 256; i++) {
        wchar_t name[64], match[128], inf[64], prov[128], ver[64], desc[256];
        DWORD nlen = 64;
        if (RegEnumKeyExW(root, i, name, &nlen, NULL, NULL, NULL, NULL)
            != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        if (reg_get_sz(sub, L"MatchingDeviceId", match, 128) && wcsistr(match, L"WOMic")) {
            out->inf_ok = 1;
            if (reg_get_sz(sub, L"InfPath", inf, 64)) {
                wchar_t windir[MAX_PATH];
                wcsncpy(out->inf_path, inf, MAX_PATH - 1);
                GetWindowsDirectoryW(windir, MAX_PATH);
                _snwprintf(out->inf_full, MAX_PATH, L"%s\\INF\\%s", windir, inf);
            }
            if (reg_get_sz(sub, L"ProviderName", prov, 128))
                wcsncpy(out->provider, prov, 63);
            if (reg_get_sz(sub, L"DriverVersion", ver, 64))
                wcsncpy(out->version, ver, 31);
            if (reg_get_sz(sub, L"DriverDesc", desc, 256)) {
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, out->detail,
                                    sizeof(out->detail), NULL, NULL);
            }
            RegCloseKey(sub);
            RegCloseKey(root);
            return 1;
        }
        RegCloseKey(sub);
        sub = NULL;
    }
    RegCloseKey(root);
    return 0;
}

/* 用 SetupAPI 确认设备真的存在（DIGCF_PRESENT），作为 L2 的第二条独立证据。
 * dev 用于卸载时定位；detect 时传 NULL。 */
static int find_device(HDEVINFO *set_out, SP_DEVINFO_DATA *dd_out)
{
    HDEVINFO set = SetupDiGetClassDevsW(&K_GUID_MEDIA, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA dd;
    DWORD i;
    if (set == INVALID_HANDLE_VALUE) return 0;
    memset(&dd, 0, sizeof(dd));
    dd.cbSize = sizeof(dd);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        wchar_t hwids[512] = { 0 };
        DWORD type = 0;
        if (SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_HARDWAREID, &type,
                                              (PBYTE)hwids, sizeof(hwids), NULL)) {
            if (wcsistr(hwids, L"WOMic")) {
                if (set_out) *set_out = set;
                if (dd_out) *dd_out = dd;
                if (!set_out) SetupDiDestroyDeviceInfoList(set);
                return 1;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return 0;
}

/* ------------------------------------------------------------ 检测 */
int drv_detect(DrvStatus *out, int probe_handle)
{
    wchar_t sysdir[MAX_PATH];

    memset(out, 0, sizeof(*out));
    out->arch = drvpkg_detect_arch();
    if (out->arch == DRVARCH_UNKNOWN) {
        set_errw(L"无法识别的处理器架构，无法选择驱动包");
        out->state = DRV_UNKNOWN;
        return 0;
    }

    /* L2（权威）：InfPath 非空 */
    scan_class_registry(out);

    /* L3（辅助）：SYS 是否真的落盘 —— 防「INF 装了但 SYS 没复制」 */
    GetSystemDirectoryW(sysdir, MAX_PATH);
    _snwprintf(out->sys_path, MAX_PATH, L"%s\\drivers\\womic.sys", sysdir);
    out->sys_on_disk = GetFileAttributesW(out->sys_path) != INVALID_FILE_ATTRIBUTES;

    /* L1（快路径）：复用 devio 的全局唯一句柄。
     * ⚠ 绝不能为了“探测”临时 CreateFile —— 那会把驱动门控清零。 */
    if (probe_handle && !devio_is_open()) devio_open();
    out->handle_ok = devio_is_open();

    if (out->handle_ok && out->inf_ok) out->state = DRV_READY;
    else if (out->inf_ok) out->state = DRV_NEED_REBOOT;
    else out->state = DRV_NOT_INSTALLED;

    if (out->state == DRV_READY) {
        set_errw(L"驱动已就绪（架构 %s）", drvpkg_arch_label(out->arch));
        return 1;
    }
    if (out->state == DRV_NEED_REBOOT) {
        set_errw(L"INF 已安装但设备无法打开，通常需要重启电脑或重新加载驱动");
        return 0;
    }
    set_errw(L"未检测到 WO Mic 虚拟麦克风驱动");
    return 0;
}

/* ------------------------------------------- catalog 签名者导入（crypt32） */
#ifndef CERT_STORE_PROV_SYSTEM_W
#define CERT_STORE_PROV_SYSTEM_W ((LPCSTR)10)
#endif

static int add_cert_to_trusted_publisher(PCCERT_CONTEXT cert)
{
    HCERTSTORE tp = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, (HCRYPTPROV_LEGACY)0,
                                  CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPublisher");
    int ok = 0;
    if (!tp) return 0;
    ok = CertAddCertificateContextToStore(tp, cert, CERT_STORE_ADD_NEWER, NULL) ? 1 : 0;
    CertCloseStore(tp, 0);
    return ok;
}

/* .cat 是 PKCS#7 SignedData（eContent 是 catalog）。用 CryptQueryObject 取出
 * 消息里的证书，定位签发者（leaf）后加入 TrustedPublisher。
 * 不派生 certutil —— 子进程无法把结果结构化地带回来，且在锁定环境里可能被拦。 */
static int import_catalog_signer(const wchar_t *cat_path)
{
    DWORD enc = 0, type = 0, fmt = 0;
    HCERTSTORE store = NULL;
    HCRYPTMSG msg = NULL;
    const void *ctx = NULL;
    int added = 0;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, (const void *)cat_path,
                          CERT_QUERY_CONTENT_FLAG_ALL, CERT_QUERY_FORMAT_FLAG_ALL,
                          0, &enc, &type, &fmt, &store, &msg, &ctx)) {
        /* 解析失败不致命：WHCP 签名的驱动在多数机器上不需要这一步，
         * 但某些环境（尤其干净装机）会因此报 0xE0000242。 */
        return 0;
    }

    if (!store && msg)
        store = CertOpenStore(CERT_STORE_PROV_MSG, enc, 0, 0, (const void *)msg);

    if (store) {
        PCCERT_CONTEXT leaf = NULL;
        if (msg) {
            DWORD sz = 0;
            if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &sz) && sz) {
                unsigned char *buf = (unsigned char *)malloc(sz);
                if (buf && CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf, &sz)) {
                    PCMSG_SIGNER_INFO si = (PCMSG_SIGNER_INFO)buf;
                    CERT_ID id;
                    id.dwIdChoice = CERT_ID_ISSUER_SERIAL_NUMBER;
                    id.IssuerSerialNumber.Issuer = si->Issuer;
                    id.IssuerSerialNumber.SerialNumber = si->SerialNumber;
                    leaf = CertFindCertificateInStore(store,
                              X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                              CERT_FIND_CERT_ID, (const void *)&id, NULL);
                }
                free(buf);
            }
        }
        if (leaf) {
            added = add_cert_to_trusted_publisher(leaf);
            CertFreeCertificateContext(leaf);
        } else {
            /* 退而求其次：把消息里的证书都加进去（上限 8 张，避免异常包） */
            PCCERT_CONTEXT c = NULL;
            int n = 0;
            while ((c = CertEnumCertificatesInStore(store, c)) != NULL && n < 8) {
                if (add_cert_to_trusted_publisher(c)) added++;
                n++;
            }
        }
        CertCloseStore(store, 0);
    }
    if (msg) CryptMsgClose(msg);
    if (ctx && type == CERT_QUERY_CONTENT_CERT) CertFreeCertificateContext((PCCERT_CONTEXT)ctx);
    if (ctx && type == CERT_QUERY_CONTENT_CTL) CertFreeCTLContext((PCTL_CONTEXT)ctx);
    if (ctx && type == CERT_QUERY_CONTENT_CRL) CertFreeCRLContext((PCCRL_CONTEXT)ctx);
    return added;
}

/* ------------------------------------------------------------ 错误解释 */
static void explain(DWORD e, const wchar_t *where)
{
    switch (e) {
    case 0xE0000228:
        set_errw(L"%s：找不到匹配的驱动（0xE0000228）—— 硬件 ID 或架构不匹配。"
                 L"请确认用的是 2022 版三件套（硬件 ID Root\\WOMic）且与本机架构一致。", where);
        break;
    case 0xE000024B:
        set_errw(L"%s：文件哈希不在目录中（0xE000024B）—— INF/SYS/CAT 没有成套"
                 L"改回原名 womic.inf / womic.sys / womic.cat，或三件套版本不一致。", where);
        break;
    case 0xE0000242:
        set_errw(L"%s：签名者不受信任（0xE0000242）—— catalog 签名者证书未能导入"
                 L"TrustedPublisher。可在管理员 PowerShell 里手动执行："
                 L"certutil -addstore TrustedPublisher <womic.cat>", where);
        break;
    case ERROR_ACCESS_DENIED:
        set_errw(L"%s：拒绝访问（需要管理员权限）。请以管理员身份运行安装。", where);
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        set_errw(L"%s：找不到 INF 文件，驱动包可能没有正确释放。", where);
        break;
    default:
        set_errw(L"%s：失败（0x%08lX）", where, (unsigned long)e);
        break;
    }
}

/* ------------------------------------------------------------ devlog */
int drv_grab_devlog(wchar_t *out, int cap)
{
    wchar_t path[MAX_PATH], windir[MAX_PATH];
    HANDLE h;
    DWORD sz, got = 0;
    char *buf;
    int keep;

    out[0] = 0;
    GetWindowsDirectoryW(windir, MAX_PATH);
    _snwprintf(path, MAX_PATH, L"%s\\inf\\setupapi.dev.log", windir);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return 0; }
    if (sz > 262144) { SetFilePointer(h, -262144, NULL, FILE_END); sz = 262144; }
    else SetFilePointer(h, 0, NULL, FILE_BEGIN);
    buf = (char *)malloc(sz + 2);
    if (!buf) { CloseHandle(h); return 0; }
    if (!ReadFile(h, buf, sz, &got, NULL)) { free(buf); CloseHandle(h); return 0; }
    CloseHandle(h);
    buf[got] = 0;

    /* 只留最后 4000 字符，够看最近一次安装的结论 */
    keep = (int)got > 4000 ? 4000 : (int)got;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, buf + (got - keep), keep, out, cap - 1);
        if (wlen <= 0) {
            wlen = MultiByteToWideChar(CP_ACP, 0, buf + (got - keep), keep, out, cap - 1);
        }
        if (wlen < 0) wlen = 0;
        out[wlen] = 0;
    }
    free(buf);
    return 1;
}

/* ------------------------------------------------------------ 安装 */
int drv_install(void)
{
    int arch;
    wchar_t dir[MAX_PATH], inf[MAX_PATH], cat[MAX_PATH];
    wchar_t hwids[32] = L"Root\\WOMic\0";
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dd;
    BOOL reboot = FALSE;
    DWORD err;
    DrvStatus st;

    if (!drv_is_elevated()) {
        set_errw(L"安装驱动需要管理员权限（当前进程未提权）");
        return 0;
    }
    arch = drvpkg_detect_arch();
    if (arch == DRVARCH_UNKNOWN) { set_errw(L"无法识别处理器架构"); return 0; }
    if (arch != DRVARCH_AMD64 && arch != DRVARCH_INTEL && arch != DRVARCH_ARM64) {
        set_errw(L"不支持的架构 %d", arch);
        return 0;
    }

    /* 1) 释放三件套到 %ProgramData%\MicLink\driver\<arch>\
     *    不用 %TEMP%：会被清理工具中途删掉，且不像正规软件。 */
    drvpkg_target_dir(arch, dir, MAX_PATH);
    wcsncpy(g_lastdir, dir, MAX_PATH - 1);
    if (g_extpack[0]) {
        if (!drvpkg_extract_dir(g_extpack, dir, 1)) {
            set_errw(L"从外部驱动包 %ls 释放失败：%hs", g_extpack, drvpkg_last_error());
            return 0;
        }
    } else {
        /* 空包构建（源码库不内置第三方驱动二进制）时的降级路径：
         * 提前给出可操作的提示，不要走到解压阶段才报一句看不懂的错。 */
        if (!drvpkg_available(arch)) {
            set_errw(L"本 exe 未内置驱动包（空包构建）：请用 --driver-pack <dir> 指定"
                     L"驱动目录（需 womic.inf/womic.sys/womic.cat 三件套），"
                     L"或从官方渠道 wolicheng.com/womic 获取驱动后重新打包");
            return 0;
        }
        if (!drvpkg_extract(arch, dir, 1)) {
            set_errw(L"释放内置驱动包失败：%hs", drvpkg_last_error());
            return 0;
        }
    }
    _snwprintf(inf, MAX_PATH, L"%s\\womic.inf", dir);
    _snwprintf(cat, MAX_PATH, L"%s\\womic.cat", dir);

    /* 2) 信任 catalog 签名者（幂等；失败不致命则继续） */
    {
        int n = import_catalog_signer(cat);
        if (n <= 0) {
            /* 不致命：WHCP 签名在多数机器上已经受信任。记录但继续。 */
            set_errw(L"提示：未能导入 catalog 签名者证书（%ls），继续尝试安装", cat);
        }
    }

    /* 3) 创建 root-enumerated devnode（等价于 devcon install，自己实现，
     *    不依赖外部工具：devgen.exe 不保证随系统自带，DIFx/DPInst 已废弃，
     *    pnputil /add-driver /install 又不创建 root devnode）。 */
    if (!find_device(NULL, NULL)) {
        set = SetupDiCreateDeviceInfoList(&K_GUID_MEDIA, NULL);
        if (set == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            explain(err, L"SetupDiCreateDeviceInfoList");
            return 0;
        }
        memset(&dd, 0, sizeof(dd));
        dd.cbSize = sizeof(dd);
        if (!SetupDiCreateDeviceInfoW(set, WOMIC_HWID, &K_GUID_MEDIA,
                                      L"WO Mic virtual microphone", NULL,
                                      DICD_GENERATE_ID, &dd)) {
            /* 某些系统上 DeviceName 不允许带反斜杠，退一步用短名再设硬件 ID */
            memset(&dd, 0, sizeof(dd));
            dd.cbSize = sizeof(dd);
            if (!SetupDiCreateDeviceInfoW(set, L"WOMic", &K_GUID_MEDIA,
                                          L"WO Mic virtual microphone", NULL,
                                          DICD_GENERATE_ID, &dd)) {
                err = GetLastError();
                explain(err, L"SetupDiCreateDeviceInfo");
                SetupDiDestroyDeviceInfoList(set);
                return 0;
            }
        }
        /* 硬件 ID 必须是 multi-sz（双 0 结尾），且大小写不敏感匹配 Root\WOMic */
        if (!SetupDiSetDeviceRegistryPropertyW(set, &dd, SPDRP_HARDWAREID,
                                               (PBYTE)hwids, sizeof(hwids))) {
            err = GetLastError();
            explain(err, L"设置硬件 ID");
            SetupDiDestroyDeviceInfoList(set);
            return 0;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dd)) {
            err = GetLastError();
            /* 已存在时不算致命（可能是上次残留），继续往下走 */
            if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
                explain(err, L"注册设备节点");
                SetupDiDestroyDeviceInfoList(set);
                return 0;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    /* 4) 绑定驱动 */
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, WOMIC_HWID, inf,
                                            INSTALLFLAG_FORCE, &reboot)) {
        err = GetLastError();
        if (err == ERROR_NO_MORE_ITEMS || err == ERROR_NO_SUCH_DEVINST) {
            /* 没有待更新的设备（可能已经装好了）→ 交给下面的 L1/L2 判定 */
        } else {
            explain(err, L"安装驱动");
            return 0;
        }
    }

    /* 5) 判定：回到 L1/L2/L3，不看任何工具输出 */
    drv_detect(&st, 1);
    if (st.state == DRV_READY) {
        /* 成功后立即删除释放目录，不留散落的 .sys（驱动本体已在 DriverStore） */
        drvpkg_cleanup(dir);
        g_lastdir[0] = 0;
        set_errw(L"安装成功：%s，INF=%s%s", drvpkg_arch_label(arch),
                 st.inf_path[0] ? st.inf_path : L"(未知)",
                 reboot ? L"（需要重启）" : L"");
        return 1;
    }
    if (st.state == DRV_NEED_REBOOT) {
        drvpkg_cleanup(dir);
        g_lastdir[0] = 0;
        set_errw(L"INF 已安装但设备尚未就绪，请重启电脑后重试");
        return 0;
    }
    set_errw(L"安装流程已执行，但三级判定仍不通过（InfPath=%s，设备句柄=%s，SYS=%s）",
             st.inf_path[0] ? st.inf_path : L"空",
             st.handle_ok ? L"可打开" : L"打不开",
             st.sys_on_disk ? L"已落盘" : L"缺失");
    return 0;
}

/* ------------------------------------------------------------ 卸载 */
int drv_uninstall(void)
{
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dd;
    wchar_t driver_key[256], inf[64] = { 0 };
    DWORD type = 0;
    int removed = 0;

    if (!drv_is_elevated()) { set_errw(L"卸载驱动需要管理员权限"); return 0; }

    if (!find_device(&set, &dd)) {
        set_errw(L"没有找到 WO Mic 设备，无需卸载");
        return 1;
    }

    /* 先把 INF 名（oemNN.inf）读出来，DIF_REMOVE 之后就读不到了 */
    if (SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_DRIVER, &type,
                                          (PBYTE)driver_key, sizeof(driver_key), NULL)) {
        wchar_t keypath[320];
        HKEY k = NULL;
        _snwprintf(keypath, 320, L"SYSTEM\\CurrentControlSet\\Control\\Class\\%s", driver_key);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keypath, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            reg_get_sz(k, L"InfPath", inf, 64);
            RegCloseKey(k);
        }
    }

    if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dd)) removed = 1;
    else if (SetupDiRemoveDevice(set, &dd)) removed = 1;

    SetupDiDestroyDeviceInfoList(set);

    if (inf[0]) {
        if (!SetupUninstallOEMInfW(inf, 0, NULL)) {
            set_errw(L"设备已移除，但从 DriverStore 卸载 %s 失败（0x%08lX）",
                     inf, (unsigned long)GetLastError());
            return 0;
        }
    }
    if (!removed) { set_errw(L"未能移除设备节点"); return 0; }
    set_errw(L"卸载完成%s（建议重启电脑）", inf[0] ? L"" : L"（未找到 INF 记录）");
    return 1;
}
