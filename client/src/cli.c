/* cli.c — 参数解析、控制台挂接、命令执行
 *
 * -mwindows 子系统下没有控制台。这里的两条规则：
 *   · 只有真的要输出（--help/--version/--status/--install-driver/--uninstall-driver
 *     /--connect --no-gui）才挂控制台；
 *   · 优先 AttachConsole(ATTACH_PARENT_PROCESS)（cmd / PowerShell 里跑就复用
 *     那个窗口，输出直接出现在当前终端），失败才 AllocConsole()。
 *   双击 exe（无任何参数）不调用 cli_ensure_console，因此永不闪黑窗。
 */
#include "cli.h"
#include "drvinst.h"
#include "drvpkg.h"
#include "drvbridge.h"      /* --install-driver / --uninstall-driver 转发给安装器 */
#include "devio.h"
#include "opusdec.h"
#include "singleinst.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_exit_code = CLI_EXIT_OK;
static int g_has_console;

int cli_exit_code(void) { return g_exit_code; }

void cli_set_defaults(CliArgs *a)
{
    memset(a, 0, sizeof(*a));
    a->port = 8125;
    a->rate = 48000;
    a->rate_idx = 2;
    a->buffer_ms = 60;
    a->gain = 100;
}

/* --------------------------------------------------------------- 帮助文本 */
const char *cli_help_text(void)
{
    return
    "WO Mic Link " APP_VERSION " — 把 Android 手机麦克风变成 Windows 虚拟麦克风\n"
    "\n"
    "用法:\n"
    "  womic.exe [--host <IP|主机名>] [--port <1..65535>]\n"
    "            [--rate 48000|16000|8000] [--buffer 26|60|120] [--gain 0..200]\n"
    "            [--connect] [--no-gui] [--status]\n"
    "            [--install-driver] [--uninstall-driver] [--driver-pack <dir>]\n"
    "            [--theme dark|light] [--help] [--version]\n"
    "  womic.exe <IP> [port]                 # 位置参数简写（兼容旧用法）\n"
    "\n"
    "选项:\n"
    "  --host <IP>            手机在局域网里的地址；--connect 时必填\n"
    "  --port <n>             TCP 控制端口，默认 8125（媒体端口由本机随机分配）\n"
    "  --rate 48000|16000|8000\n"
    "                         向手机请求的采样率，默认 48000。\n"
    "                         无论协商到多少，灌给驱动的一律是 48000（驱动不重采样）\n"
    "  --buffer 26|60|120     驱动环形缓冲档位（毫秒），默认 60。\n"
    "                         26 = 低延迟，120 = 更抗抖动；低于 ~11ms 会失效\n"
    "  --gain 0..200          本地增益百分比，默认 100\n"
    "  --connect              启动后立刻连接（需要 --host）\n"
    "  --no-gui               配合 --connect：不显示窗口，跑完退出（脚本/CI 用）\n"
    "  --status               打印架构、驱动状态、INF 路径、版本、解码器来源后退出\n"
    "  --install-driver       安装/修复虚拟麦克风驱动：转发给同目录的\n"
    "                         womic-drvinst.exe（自动申请一次 UAC 提权）\n"
    "  --uninstall-driver     卸载虚拟麦克风驱动（同样转发给安装器）\n"
    "  --driver-pack <dir>    用指定目录里的驱动三件套代替内置包\n"
    "                         （需要 womic.inf / womic.sys / womic.cat，或 womic_1.* / womic_2.*）\n"
    "  --theme dark|light     界面主题（--dark / --light 同义）\n"
    "  --help                 显示本帮助\n"
    "  --version              显示版本\n"
    "\n"
    "退出码:\n"
    "  0  成功\n"
    "  1  参数错误\n"
    "  2  连接失败\n"
    "  3  驱动不可用（--status 检测到驱动未就绪时也返回 3）\n"
    "  4  驱动安装/卸载失败\n"
    "\n"
    "示例:\n"
    "  womic.exe --host 192.168.1.20 --connect\n"
    "  womic.exe --host 192.168.1.20 --rate 16000 --buffer 26 --gain 130 --connect\n"
    "  womic.exe --status\n"
    "  womic.exe --install-driver\n"
    "  womic.exe --driver-pack D:\\drivers\\womic2022 --install-driver\n"
    "  womic.exe --host 192.168.1.20 --connect --no-gui        # CI 冒烟\n"
    "\n"
    "驱动安装的分工：\n"
    "  驱动的安装/卸载不在本 exe 里 —— 它们由同目录的 womic-drvinst.exe 负责。\n"
    "  --install-driver / --uninstall-driver 只是转发：找到它、带上同样的参数\n"
    "  拉起来、把它的退出码带回来。找不到就明确提示，不会假装成功。\n"
    "  凡是用官方安装器装过驱动的机器，直接拿本 exe 就能用，不需要那个安装器。\n"
    "  安装器自身也可单独使用：womic-drvinst.exe /install | /uninstall | /status\n"
    "\n"
    "驱动归属:\n"
    "  womic.sys / womic.inf / womic.cat 版权归 " DRV_OWNER " 所有，\n"
    "  版本 " DRV_VERSION "，来源 " DRV_SOURCE_URL "。\n"
    "  本项目原样再分发：未修改、未重新签名，仅复用其已签名的虚拟声卡功能。\n"
    "  ⚠ 这些第三方二进制**不在本 exe 里**，只随 womic-drvinst.exe 分发；\n"
    "    本 exe 不含任何第三方二进制，下面的「驱动包来源」也会如实显示未内置。\n"
    "\n"
    "被杀软误报时的处理:\n"
    "  1. 右键 womic.exe → 属性 → 勾选「解除锁定」（若有）\n"
    "  2. Windows 安全中心 → 病毒和威胁防护 → 管理设置 → 排除项 → 添加本文件\n"
    "  3. 仍被拦截可向微软提交误报申诉:\n"
    "     https://www.microsoft.com/en-us/wdsi/filesubmission\n"
    "  本程序不加壳、不自解压、不运行时下载、不写自启动、不建服务、不注入进程。\n";
}

static void out(const char *s) { fputs(s, stdout); fflush(stdout); }

void cli_print_help(void) { out(cli_help_text()); }

/* --------------------------------------------------------------- 控制台 */
void cli_ensure_console(int force_alloc)
{
    if (g_has_console) return;
    /* 已经有可用的标准输出就别折腾：控制台子系统、或从 cmd 启动时句柄已经继承
     * 过来了。再去 AttachConsole 反而可能把 freopen 弄失败（wine 下实测即如此）。 */
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
            g_has_console = 1;
            SetConsoleOutputCP(CP_UTF8);
            return;
        }
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_has_console = 1;
    } else if (force_alloc) {
        if (AllocConsole()) g_has_console = 1;
    } else {
        return;
    }
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);       /* 让 cmd/PowerShell 直接显示 UTF-8 */
}

void cli_release_console(void)
{
    if (!g_has_console) return;
    fflush(stdout);
    FreeConsole();
    g_has_console = 0;
}

/* --------------------------------------------------------------- 解析 */
static int need_value(int argc, wchar_t **argv, int i)
{
    return (i + 1 < argc);
}

static int parse_int(const wchar_t *s, int *v)
{
    wchar_t *end = NULL;
    long n;
    if (!s || !*s) return 0;
    n = wcstol(s, &end, 10);
    if (end && *end) return 0;
    *v = (int)n;
    return 1;
}

int cli_parse(CliArgs *a, int argc, wchar_t **argv)
{
    int i, pos = 0;
    a->argc = argc;
    a->argv = argv;
    for (i = 1; i < argc; i++) {
        const wchar_t *s = argv[i];
        if (s[0] != L'-' && s[0] != L'/') {
            if (pos == 0) { a->host_pos = s; pos = 1; }
            else if (pos == 1) { if (!parse_int(s, &a->port)) a->port = 8125; pos = 2; }
            continue;
        }
        s += 1;                             /* 吃掉一个 '-'/'/' */
        if (*s == L'-') s += 1;

        if (!_wcsicmp(s, L"help") || !_wcsicmp(s, L"?")) { a->help = 1; continue; }
        if (!_wcsicmp(s, L"version")) { a->version = 1; continue; }
        if (!_wcsicmp(s, L"status")) { a->status = 1; continue; }
        if (!_wcsicmp(s, L"connect") || !_wcsicmp(s, L"auto")) { a->connect = 1; continue; }
        if (!_wcsicmp(s, L"no-gui") || !_wcsicmp(s, L"nogui")) { a->no_gui = 1; continue; }
        if (!_wcsicmp(s, L"install-driver") || !_wcsicmp(s, L"install")) { a->install = 1; continue; }
        if (!_wcsicmp(s, L"uninstall-driver") || !_wcsicmp(s, L"uninstall")) { a->uninstall = 1; continue; }
        if (!_wcsicmp(s, L"dark")) { a->dark = 1; a->light = 0; a->theme_set = 1; continue; }
        if (!_wcsicmp(s, L"light")) { a->light = 1; a->dark = 0; a->theme_set = 1; continue; }

        if (!_wcsicmp(s, L"host")) {
            if (!need_value(argc, argv, i)) return 0;
            a->host_pos = argv[++i]; continue;
        }
        if (!_wcsicmp(s, L"port")) {
            if (!need_value(argc, argv, i)) return 0;
            if (!parse_int(argv[++i], &a->port)) return 0;
            continue;
        }
        if (!_wcsicmp(s, L"rate")) {
            int v;
            if (!need_value(argc, argv, i)) return 0;
            if (!parse_int(argv[++i], &v)) return 0;
            if (v == 48000) { a->rate = 48000; a->rate_idx = 2; }
            else if (v == 16000) { a->rate = 16000; a->rate_idx = 1; }
            else if (v == 8000) { a->rate = 8000; a->rate_idx = 0; }
            else return 0;
            continue;
        }
        if (!_wcsicmp(s, L"buffer")) {
            int v;
            if (!need_value(argc, argv, i)) return 0;
            if (!parse_int(argv[++i], &v)) return 0;
            if (v != 26 && v != 60 && v != 120) return 0;
            a->buffer_ms = v;
            continue;
        }
        if (!_wcsicmp(s, L"gain")) {
            int v;
            if (!need_value(argc, argv, i)) return 0;
            if (!parse_int(argv[++i], &v)) return 0;
            if (v < 0 || v > 200) return 0;
            a->gain = v;
            continue;
        }
        if (!_wcsicmp(s, L"theme")) {
            if (!need_value(argc, argv, i)) return 0;
            ++i;
            if (!_wcsicmp(argv[i], L"dark")) { a->dark = 1; a->light = 0; a->theme_set = 1; }
            else if (!_wcsicmp(argv[i], L"light")) { a->light = 1; a->dark = 0; a->theme_set = 1; }
            else return 0;
            continue;
        }
        if (!_wcsicmp(s, L"driver-pack") || !_wcsicmp(s, L"driverpack")) {
            if (!need_value(argc, argv, i)) return 0;
            a->driver_pack = argv[++i];
            continue;
        }
        if (!_wcsicmp(s, L"feed")) {
            /* 灌音路径：ioctl（默认，已实测）/ write（返回真实字节数，可背压）。
             * 详见 src/devio.c 顶部注释。 */
            if (!need_value(argc, argv, i)) return 0;
            ++i;
            if (!_wcsicmp(argv[i], L"write")) a->feed_write = 1;
            else if (!_wcsicmp(argv[i], L"ioctl")) a->feed_write = 0;
            else return 0;
            continue;
        }
        return 0;                            /* 未知参数 */
    }
    return 1;
}

/* --------------------------------------------------------------- 执行 */
static const char *state_name(DrvState s)
{
    switch (s) {
    case DRV_READY: return "已就绪";
    case DRV_NOT_INSTALLED: return "未安装";
    case DRV_NEED_REBOOT: return "需重启";
    default: return "未知";
    }
}

static int do_status(const CliArgs *a)
{
    DrvStatus st;
    char buf[512], tmp[256];
    int probe = 1;

    cli_ensure_console(1);
    if (a->driver_pack) drv_set_external_pack(a->driver_pack);
    drv_detect(&st, probe);

    out("WO Mic Link " APP_VERSION "\n");
    _snprintf(buf, sizeof(buf), "  架构        : %ls (%ls)\n",
              drvpkg_arch_label(st.arch), drvpkg_arch_dir(st.arch));
    out(buf);
    _snprintf(buf, sizeof(buf), "  驱动状态    : %s\n", state_name(st.state));
    out(buf);
    WideCharToMultiByte(CP_UTF8, 0, st.inf_path, -1, tmp, sizeof(tmp), NULL, NULL);
    _snprintf(buf, sizeof(buf), "  INF 路径    : %s\n", tmp[0] ? tmp : "(无)");
    out(buf);
    WideCharToMultiByte(CP_UTF8, 0, st.version, -1, tmp, sizeof(tmp), NULL, NULL);
    _snprintf(buf, sizeof(buf), "  驱动版本    : %s\n", tmp[0] ? tmp : DRV_VERSION);
    out(buf);
    WideCharToMultiByte(CP_UTF8, 0, st.provider, -1, tmp, sizeof(tmp), NULL, NULL);
    _snprintf(buf, sizeof(buf), "  驱动提供者  : %s\n", tmp[0] ? tmp : "-");
    out(buf);
    _snprintf(buf, sizeof(buf), "  SYS 落盘    : %s\n", st.sys_on_disk ? "是" : "否");
    out(buf);
    _snprintf(buf, sizeof(buf), "  设备句柄    : %s\n", st.handle_ok ? "可打开" : "打不开");
    out(buf);
    _snprintf(buf, sizeof(buf), "  驱动包来源  : %s\n",
              a->driver_pack ? "外部 --driver-pack" : drvpkg_origin());
    out(buf);
    _snprintf(buf, sizeof(buf), "  Opus 解码器 : %s\n", opusdec_source());
    out(buf);
    out("  驱动归属    : " DRV_OWNER " / " DRV_VERSION " / " DRV_SOURCE_URL "\n");
    g_exit_code = (st.state == DRV_READY) ? CLI_EXIT_OK : CLI_EXIT_DEVUNAVA;
    return 1;
}

/* ---------------------------------------------------- 驱动操作：转发给安装器 */
/* 安装逻辑有且只有一份：在 womic-drvinst.exe 里（UI 的「安装/卸载驱动」按钮
 * 走的也是同一条路，见 main.c 的 request_driver_op）。这里只负责找到它、
 * 带上同样的参数拉起、把退出码翻译成帮助里那张退出码表的语义（0 成功 / 4 失败）。
 * 找不到就明确提示，绝不假装成功。 */
static int do_driver_op(const CliArgs *a)
{
    wchar_t inst[MAX_PATH];
    int code = -1;
    DrvOp op = a->install ? DRVOP_INSTALL : DRVOP_UNINSTALL;

    cli_ensure_console(1);

    if (!drvbridge_find(inst, MAX_PATH)) {
        out(a->install ? "无法安装驱动：" : "无法卸载驱动：");
        out(drvbridge_last_error());
        out("\n");
        out("  说明：驱动的安装/卸载已拆到独立的 womic-drvinst.exe（只有它携带第三方驱动包）。\n");
        out("  请把 womic-drvinst.exe 与本 exe 放在同一目录后重试；\n");
        out("  若本机已用官方安装器装过驱动，则无需任何安装操作，直接用本 exe 即可。\n");
        g_exit_code = CLI_EXIT_DEVINST;
        return 1;
    }

    out(a->install ? "正在调用 womic-drvinst.exe 安装驱动…\n"
                   : "正在调用 womic-drvinst.exe 卸载驱动…\n");
    if (!drvbridge_run(op, a->driver_pack, &code)) {
        out(a->install ? "驱动安装失败：" : "驱动卸载失败：");
        out(drvbridge_last_error());
        out("\n");
        g_exit_code = CLI_EXIT_DEVINST;
        return 1;
    }
    if (code == CLI_EXIT_OK) {
        out(a->install ? "驱动安装完成。\n" : "驱动卸载完成。\n");
        g_exit_code = CLI_EXIT_OK;
        return 1;
    }
    {
        char line[160];
        _snprintf(line, sizeof(line),
                  "%s详见上方安装器输出（退出码 %d）\n",
                  a->install ? "驱动安装失败：" : "驱动卸载失败：", code);
        out(line);
    }
    if (code == CLI_EXIT_DEVUNAVA)
        out("  驱动仍未就绪；可重试一次，或右键 womic-drvinst.exe → 以管理员身份运行。\n");
    g_exit_code = CLI_EXIT_DEVINST;
    return 1;
}

/* 返回 1 表示进程应退出（退出码见 cli_exit_code()），0 表示继续进 GUI */
int cli_run(CliArgs *a)
{
    if (a->help) {
        cli_ensure_console(1);
        cli_print_help();
        g_exit_code = CLI_EXIT_OK;
        return 1;
    }
    if (a->version) {
        char buf[128];
        cli_ensure_console(1);
        _snprintf(buf, sizeof(buf), APP_NAME " " APP_VERSION "  (%s)\n",
                  opusdec_source());
        out(buf);
        g_exit_code = CLI_EXIT_OK;
        return 1;
    }
    if (a->status) return do_status(a);
    if (a->install || a->uninstall) return do_driver_op(a);

    if (a->connect && !a->host_pos) {
        cli_ensure_console(1);
        out("--connect 需要 --host <IP>（或位置参数给出地址）\n");
        cli_print_help();
        g_exit_code = CLI_EXIT_PARAM;
        return 1;
    }
    if (a->connect && a->no_gui) {
        /* 无窗口冒烟模式：连上跑 5 秒后退出，便于 CI 判断链路是否通 */
        char host[128];
        EngineStats st;
        int i;
        cli_ensure_console(1);
        WideCharToMultiByte(CP_UTF8, 0, a->host_pos, -1, host, sizeof(host), NULL, NULL);
        if (a->driver_pack) drv_set_external_pack(a->driver_pack);
        drv_detect(&g_ui.drv, 1);
        if (!eng_start(host, a->port, a->rate_idx, a->buffer_ms, a->gain)) {
            out("引擎启动失败\n");
            g_exit_code = CLI_EXIT_CONNECT;
            return 1;
        }
        for (i = 0; i < 100; i++) {
            Sleep(100);
            eng_snapshot(&st);
            if (st.state == ST_ERROR) break;
            if (i == 20 && st.state != ST_STREAMING) break;   /* 2s 还没进推流就算失败 */
        }
        eng_snapshot(&st);
        if (st.state == ST_STREAMING) {
            char line[256];
            _snprintf(line, sizeof(line),
                      "OK  帧=%llu 字节=%llu 注入=%llu 电平=%.3f dB=%.1f "
                      "解码=%s 设备=%s\n",
                      (unsigned long long)st.frames, (unsigned long long)st.bytes,
                      (unsigned long long)st.injected, (double)st.level,
                      (double)st.db, st.opus_src, st.device_ok ? "就绪" : "未就绪");
            out(line);
            g_exit_code = CLI_EXIT_OK;
        } else {
            char line[256];
            _snprintf(line, sizeof(line), "FAIL 状态=%d 错误=%s\n",
                      (int)st.state, st.last_error[0] ? st.last_error : "(无)");
            out(line);
            g_exit_code = CLI_EXIT_CONNECT;
        }
        eng_stop();
        return 1;
    }
    return 0;    /* 其余情况照常进 GUI */
}

void cli_apply_to_ui(const CliArgs *a)
{
    if (a->host_pos) SetWindowTextW(g_ui.edit_host, a->host_pos);
    if (a->rate_idx != 2 || a->rate != 48000) g_ui.seg_rate = (a->rate_idx == 2 ? 0 : a->rate_idx == 1 ? 1 : 2);
    if (a->buffer_ms == 26) g_ui.buf_idx = 0;
    else if (a->buffer_ms == 120) g_ui.buf_idx = 2;
    else g_ui.buf_idx = 1;
    if (a->gain != 100) g_ui.gain = a->gain;
}
