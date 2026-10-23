/* drvinst_main.c — womic-drvinst.exe：WO Mic 虚拟麦克风驱动安装器
 *
 * 为什么单独一个 exe：
 *   驱动三件套（womic.sys/inf/cat，Wolicheng 版权、WHCP 已签名）是**第三方二进制**。
 *   把它塞进应用本体 == 每次分发应用都在分发第三方驱动，合规面过大。
 *   拆出来之后：
 *     · 已经装过驱动的机器（官方安装器或本机装过一次）→ 只要 womic.exe；
 *     · 没装过的机器 → 才需要拿一次这个安装器。
 *
 * 铁律：**安装逻辑只有一份**。菜单里的 1) 安装驱动 调用的就是 /install 那条
 * 路径（do_install），不存在两套实现。菜单只是一个壳。
 *
 * 操作之外的两个细节：
 *   · manifest 是 requireAdministrator（双击自动弹 UAC）；菜单模式若检测到
 *     未提权，选 1/2 会明确提示「请以管理员身份运行」，不会静默失败。
 *   · 空包构建（源码树里没有官方驱动目录时的合法产物，count=0）下 install
 *     必须报明确错误、绝不静默成功；status 照实报未安装。
 *
 * 退出码与应用的 --status 对齐（用 app.h 里同一组 CLI_EXIT_* 宏保证）：
 *   0 成功 / 1 参数错误 / 3 驱动未安装（仅 /status）/ 4 安装或卸载失败
 */
#define WIN32_LEAN_AND_MEAN
#include "app.h"
#include "drvinst.h"
#include "drvpkg.h"
#include "devio.h"
#include <shellapi.h>      /* CommandLineToArgvW */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRVINST_TITLE  "WO Mic Link 驱动安装器"
#define DEVLOG_CHARS   4096

static int g_quiet;            /* /quiet：只保留结果行，不打横幅 */
static const wchar_t *g_pack;  /* /driver-pack <dir> */

/* --------------------------------------------------------------- 输出 */
static void print_banner(void)
{
    if (!g_quiet) printf("%s %s\n", DRVINST_TITLE, APP_VERSION);
}

static const char *w2u(const wchar_t *w, char *buf, size_t cap)
{
    int n;
    if (!w || !w[0]) { if (cap) buf[0] = 0; return buf; }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    if (n <= 0) buf[0] = 0;
    buf[cap - 1] = 0;
    return buf;
}

static const char *state_name(DrvState s)
{
    switch (s) {
    case DRV_READY: return "已就绪";
    case DRV_NOT_INSTALLED: return "未安装";
    case DRV_NEED_REBOOT: return "需重启";
    default: return "未知";
    }
}

static void print_help(void)
{
    printf(
    "%s %s —— 安装/卸载/查询 WO Mic 虚拟麦克风驱动\n"
    "\n"
    "用法:\n"
    "  womic-drvinst.exe                      # 无参运行，进交互菜单\n"
    "  womic-drvinst.exe /install   [/quiet]\n"
    "  womic-drvinst.exe /uninstall [/quiet]\n"
    "  womic-drvinst.exe /status    [/quiet]\n"
    "  womic-drvinst.exe /driver-pack <dir> /install\n"
    "  womic-drvinst.exe /help\n"
    "\n"
    "选项:\n"
    "  /install              安装（或修复）驱动；已装且就绪时直接成功\n"
    "  /uninstall            卸载驱动；本来就没装时也算成功\n"
    "  /status               打印驱动状态后退出\n"
    "  /driver-pack <dir>    用指定目录里的三件套代替内置包\n"
    "                        （需要 womic.inf / womic.sys / womic.cat，\n"
    "                          或 womic_1.* / womic_2.*）\n"
    "  /quiet                静默：不打横幅，只输出结果与错误\n"
    "  /help                 显示本帮助\n"
    "\n"
    "退出码（与应用 womic.exe 的 CLI 一致）:\n"
    "  0  成功\n"
    "  1  参数错误\n"
    "  3  驱动未安装（仅 /status 使用）\n"
    "  4  安装/卸载失败\n"
    "\n"
    "驱动归属:\n"
    "  womic.sys / womic.inf / womic.cat 版权归 " DRV_OWNER " 所有，\n"
    "  版本 " DRV_VERSION "，来源 " DRV_SOURCE_URL "。\n"
    "  本项目原样分发：未修改、未重新签名，仅复用其已签名的虚拟声卡功能。\n"
    "\n"
    "注意: 安装/卸载需要管理员权限。本 exe 的 manifest 已声明\n"
    "requireAdministrator，双击会自动申请提权。\n",
    DRVINST_TITLE, APP_VERSION);
}

/* --------------------------------------------------------------- 状态 */
static void print_status(const DrvStatus *st)
{
    char buf[MAX_PATH * 4];
    printf("  架构        : %s\n",
           w2u(drvpkg_arch_label(st->arch), buf, sizeof(buf)));
    printf("  驱动状态    : %s\n", state_name(st->state));
    printf("  INF 路径    : %s\n",
           st->inf_path[0] ? w2u(st->inf_path, buf, sizeof(buf)) : "(无)");
    printf("  驱动版本    : %s\n",
           st->version[0] ? w2u(st->version, buf, sizeof(buf)) : DRV_VERSION " (内置包)");
    printf("  驱动提供者  : %s\n",
           st->provider[0] ? w2u(st->provider, buf, sizeof(buf)) : "-");
    printf("  SYS 落盘    : %s\n", st->sys_on_disk ? "是" : "否");
    printf("  设备句柄    : %s\n", st->handle_ok ? "可打开" : "打不开");
    printf("  驱动包来源  : %s\n", g_pack ? "外部 /driver-pack" : drvpkg_origin());
    printf("  本机可用包  : %s\n",
           drvpkg_available(st->arch) ? "有" : "无（空包构建，需 /driver-pack <dir>）");
}

static int do_status(void)
{
    DrvStatus st;
    print_banner();
    drv_detect(&st, 1);
    print_status(&st);
    return (st.state == DRV_READY) ? CLI_EXIT_OK : CLI_EXIT_DEVUNAVA;
}

/* --------------------------------------------------------------- 安装 */
static void print_last_error(const char *head)
{
    char tmp[1024];
    w2u(drv_last_error_w(), tmp, sizeof(tmp));
    printf("%s%s\n", head, tmp);
}

static void print_devlog(void)
{
    wchar_t log[DEVLOG_CHARS];
    char tmp[DEVLOG_CHARS * 3];
    if (!drv_grab_devlog(log, DEVLOG_CHARS)) return;
    printf("---- setupapi.dev.log（尾部）----\n");
    printf("%s\n", w2u(log, tmp, sizeof(tmp)));
}

static int require_admin(const char *what)
{
    if (drv_is_elevated()) return 1;
    fprintf(stderr, "%s%s需要管理员权限。请右键 womic-drvinst.exe →"
                    "「以管理员身份运行」，或在管理员终端里执行 /%s。\n",
            g_quiet ? "" : "错误：", what, what);
    return 0;
}

static int do_install(void)
{
    DrvStatus st;
    int ok;

    print_banner();
    if (!require_admin("install")) return CLI_EXIT_DEVINST;

    /* 幂等：已装且就绪就不再折腾SetupAPI（对 CI 重复执行友好） */
    drv_detect(&st, 1);
    if (st.state == DRV_READY) {
        printf("驱动已安装且就绪，无需重复安装\n");
        if (!g_quiet) print_status(&st);
        return CLI_EXIT_OK;
    }

    ok = drv_install();
    if (ok) {
        print_last_error("");            /* drv_install 的成功说明本身就是一句完整的话 */
        return CLI_EXIT_OK;
    }
    print_last_error("安装失败：");
    if (!g_quiet) print_devlog();
    /* 空包构建的典型死法要说人话；drv_install 已经给了可操作提示，
     * 这里只补一句「怎么办」。 */
    if (!g_pack && !drvpkg_available(drvpkg_detect_arch())) {
        printf("提示：本安装器未内置可安装的驱动包（空包构建）。\n"
               "      请换自带驱动包的安装器，或用 /driver-pack <dir> 指定"
               "官方 2022 版三件套所在目录。\n");
    }
    return CLI_EXIT_DEVINST;
}

static int do_uninstall(void)
{
    int ok;
    print_banner();
    if (!require_admin("uninstall")) return CLI_EXIT_DEVINST;
    ok = drv_uninstall();
    if (ok) {
        print_last_error("");
        return CLI_EXIT_OK;
    }
    print_last_error("卸载失败：");
    if (!g_quiet) print_devlog();
    return CLI_EXIT_DEVINST;
}

/* --------------------------------------------------------------- 菜单 */
static void menu_print(void)
{
    printf(
    "\n"
    "  %s\n"
    "    1) 安装驱动\n"
    "    2) 卸载驱动\n"
    "    3) 查看驱动状态\n"
    "    0) 退出\n"
    "  请选择：", DRVINST_TITLE);
    fflush(stdout);
}

/* 读一行。返回 EOF 表示 stdin 已关（非交互/重定向），调用方据此退出而不是死循环。 */
static int read_line(char *buf, int cap)
{
    if (!fgets(buf, cap, stdin)) return 0;
    return 1;
}

static int menu_choice(const char *line)
{
    int i;
    for (i = 0; line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t') continue;
        return line[i];
    }
    return 0;
}

static int menu_loop(void)
{
    char line[64];
    print_banner();
    printf("\n提示：本 exe 已声明 requireAdministrator，直接双击会自动申请提权；\n"
           "      若下方显示未提权，请右键 →「以管理员身份运行」。\n");
    if (!drv_is_elevated())
        printf("当前：未提权（安装/卸载不可用，仅可查看状态）\n");
    else
        printf("当前：已提权\n");

    for (;;) {
        int rc;
        menu_print();
        if (!read_line(line, sizeof(line))) {
            printf("\n（标准输入已关闭，退出）\n");
            return CLI_EXIT_OK;
        }
        switch (menu_choice(line)) {
        case '1':
            if (!drv_is_elevated()) {
                printf("请以管理员身份运行本程序后再执行安装。\n");
                break;
            }
            rc = do_install();
            if (rc != CLI_EXIT_OK) printf("（安装未成功，可重试或换 /driver-pack 指定驱动目录）\n");
            break;
        case '2':
            if (!drv_is_elevated()) {
                printf("请以管理员身份运行本程序后再执行卸载。\n");
                break;
            }
            rc = do_uninstall();
            if (rc != CLI_EXIT_OK) printf("（卸载未成功，可重试）\n");
            break;
        case '3':
            rc = do_status();
            if (rc != CLI_EXIT_OK) printf("（驱动尚未就绪）\n");
            break;
        case '0':
            printf("已退出。\n");
            return CLI_EXIT_OK;
        default:
            printf("无效选择，请输入 0-3。\n");
            break;
        }
    }
}

/* --------------------------------------------------------------- 解析 */
static int eq(const wchar_t *s, ...)
{
    va_list ap;
    const wchar_t *cand;
    int hit = 0;
    va_start(ap, s);
    while ((cand = va_arg(ap, const wchar_t *)) != NULL) {
        if (!_wcsicmp(s, cand)) { hit = 1; break; }
    }
    va_end(ap);
    return hit;
}

int main(void)
{
    int argc = 0, i, action = 0, help = 0, rc = CLI_EXIT_OK;
    wchar_t **argv = NULL;

    SetConsoleOutputCP(CP_UTF8);        /* cmd/PowerShell 直接显示 UTF-8 */
    SetConsoleCP(CP_UTF8);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fprintf(stderr, "参数解析失败\n");
        return CLI_EXIT_PARAM;
    }

    for (i = 1; i < argc; i++) {
        const wchar_t *s = argv[i];
        if (s[0] != L'/' && s[0] != L'-') {
            fprintf(stderr, "未知参数：%ls\n", s);
            LocalFree(argv);
            return CLI_EXIT_PARAM;
        }
        while (*s == L'/' || *s == L'-') s++;
        if (eq(s, L"install", L"i", NULL))          { action = 1; continue; }
        if (eq(s, L"uninstall", L"remove", NULL))   { action = 2; continue; }
        if (eq(s, L"status", L"query", NULL))       { action = 3; continue; }
        if (eq(s, L"quiet", L"s", NULL))            { g_quiet = 1; continue; }
        if (eq(s, L"help", L"?", L"h", NULL))       { help = 1; continue; }
        if (eq(s, L"version", NULL)) {
            LocalFree(argv);
            printf("%s %s\n", DRVINST_TITLE, APP_VERSION);
            return CLI_EXIT_OK;
        }
        if (eq(s, L"driver-pack", L"driverpack", NULL)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "/driver-pack 缺少目录参数\n");
                LocalFree(argv);
                return CLI_EXIT_PARAM;
            }
            g_pack = argv[++i];
            continue;
        }
        fprintf(stderr, "未知参数：%ls（用 /help 查看用法）\n", s);
        LocalFree(argv);
        return CLI_EXIT_PARAM;
    }

    if (g_pack) drv_set_external_pack(g_pack);

    if (help) {
        print_help();                     /* 首行自带标题，不再打一次横幅 */
        LocalFree(argv);
        return CLI_EXIT_OK;
    }

    switch (action) {
    case 1: rc = do_install();   break;
    case 2: rc = do_uninstall(); break;
    case 3: rc = do_status();    break;
    default:
        rc = menu_loop();
        break;
    }
    LocalFree(argv);
    fflush(stdout);
    return rc;
}
