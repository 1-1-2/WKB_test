/* drvpkg.c — 内置驱动包解压与落地
 *
 * 三套驱动以 raw deflate（wbits=-15）嵌在 exe 里，这里自带一个精简 inflate
 * 把它还原。为什么不直接用系统 API：Windows 没有公开的「deflate 解压」用户态
 * API（RtlDecompressBuffer 只支持 LZNT1/XPRESS），而为此引入 zlib 会破坏
 * 「零第三方依赖」这条约束。整个 inflate 约 200 行，编译后 < 2 KB。
 *
 * inflate 实现参考 Mark Adler 的 puff.c（public domain）的码表解码结构，
 * 针对本项目做了裁剪：只解 raw deflate、输出缓冲由调用方按已知长度精确分配。
 */
#include "drvpkg.h"
/* drvpkg_data.h 只供单测（tools/test_drvpkg.c，带 -DMICLINK_DRVPKG_EMBED_ARRAY）：
 * 它内含第三方已签名驱动的 C 字节数组，**不进入公开源码库**。干净检出时该文件
 * 缺失，exe 主路径只走 .rsrc 资源（res/drvpkg.bin，由 build.sh 生成；找不到
 * 官方驱动目录时降级为 16 字节空包）。 */
#ifdef MICLINK_DRVPKG_EMBED_ARRAY
#include "drvpkg_data.h"
#endif
#include <objbase.h>   /* CoTaskMemFree */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- 错误暂存 */
static char g_err[256];

const char *drvpkg_last_error(void) { return g_err; }

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_err, sizeof(g_err) - 1, fmt, ap);
    va_end(ap);
    g_err[sizeof(g_err) - 1] = 0;
}

/* --------------------------------------------------------------- inflate */
typedef struct {
    const unsigned char *in;
    unsigned long in_len, in_pos;
    unsigned long tag;          /* 已缓冲但尚未取出的比特 */
    int           bit_cnt;
    unsigned char *out;
    unsigned long out_cap, out_pos;
} Inf;

static int inf_bits(Inf *s, int need)
{
    unsigned long v = s->tag;
    while (s->bit_cnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        v |= (unsigned long)s->in[s->in_pos++] << s->bit_cnt;
        s->bit_cnt += 8;
    }
    s->tag = v >> need;
    s->bit_cnt -= need;
    return (int)(v & ((1UL << need) - 1UL));
}

typedef struct { short count[16]; short symbol[288]; } Huff;

static int huff_build(Huff *h, const unsigned char *len, int n)
{
    int offs[16], i;
    memset(h, 0, sizeof(*h));
    for (i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;                       /* 长度 0 的码不参与解码 */
    /* 用 32 位累加检测码表溢出（超完全码），避免 int 溢出导致静默错表 */
    {
        int left = 1;
        for (i = 1; i < 15; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return 0; }
    }
    offs[0] = 0; offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return 1;
}

static int huff_decode(Inf *s, Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1);
        int count;
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const short LBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short LEXT[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const short DBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short DEXT[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inf_stored(Inf *s)
{
    unsigned long len;
    unsigned char hdr[4];
    s->tag = 0; s->bit_cnt = 0;            /* 字节对齐 */
    if (s->in_pos + 4 > s->in_len) return -1;
    memcpy(hdr, s->in + s->in_pos, 4);
    len = (unsigned long)hdr[0] | ((unsigned long)hdr[1] << 8);
    if (len != (~((unsigned long)hdr[2] | ((unsigned long)hdr[3] << 8)) & 0xFFFFUL)) return -1;
    s->in_pos += 4;
    if (s->in_pos + len > s->in_len) return -1;
    if (s->out_pos + len > s->out_cap) return -1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len; s->out_pos += len;
    return 0;
}

/* 固定 Huffman 码长（RFC 1951 §3.2.6）：用代码生成比手写 320 个字面量可靠 */
static void fixed_tables(Huff *lit, Huff *dist)
{
    unsigned char len[320];
    int i;
    for (i = 0; i < 144; i++)     len[i] = 8;
    for (; i < 256; i++)          len[i] = 9;
    for (; i < 280; i++)          len[i] = 7;
    for (; i < 288; i++)          len[i] = 8;
    for (i = 288; i < 320; i++)   len[i] = 5;
    huff_build(lit, len, 288);
    huff_build(dist, len + 288, 30);
}
static int inf_block(Inf *s, Huff *lit, Huff *dist)
{
    int sym;
    for (;;) {
        sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) return -1;
            s->out[s->out_pos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dsym, d, i;
            sym -= 257;
            if (sym >= 29) return -1;
            len = LBASE[sym] + inf_bits(s, LEXT[sym]);
            dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            d = DBASE[dsym] + inf_bits(s, DEXT[dsym]);
            if ((unsigned long)d > s->out_pos) return -1;
            if (s->out_pos + (unsigned long)len > s->out_cap) return -1;
            for (i = 0; i < len; i++) {
                s->out[s->out_pos] = s->out[s->out_pos - d];
                s->out_pos++;
            }
        }
    }
}

static int inf_dynamic(Inf *s)
{
    unsigned char lengths[320];
    static const unsigned char ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    Huff lit, dist;
    int nlen, ndist, ncode, i, index = 0;

    nlen = inf_bits(s, 5) + 257;
    ndist = inf_bits(s, 5) + 1;
    ncode = inf_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < ncode; i++) lengths[ORDER[i]] = (unsigned char)inf_bits(s, 3);
    for (; i < 19; i++) lengths[ORDER[i]] = 0;
    huff_build(&lit, lengths, 19);
    while (index < nlen + ndist) {
        int sym = huff_decode(s, &lit);
        if (sym < 0) return -1;
        if (sym < 16) lengths[index++] = (unsigned char)sym;
        else {
            unsigned char fill = 0;
            int repeat;
            if (sym == 16) {
                if (index == 0) return -1;
                fill = lengths[index - 1];
                repeat = 3 + inf_bits(s, 2);
            } else if (sym == 17) {
                repeat = 3 + inf_bits(s, 3);
            } else {
                repeat = 11 + inf_bits(s, 7);
            }
            if (index + repeat > nlen + ndist) return -1;
            while (repeat--) lengths[index++] = fill;
        }
    }
    huff_build(&lit, lengths, nlen);
    huff_build(&dist, lengths + nlen, ndist);
    return inf_block(s, &lit, &dist);
}

static int inf_raw(const unsigned char *in, unsigned long in_len,
                   unsigned char *out, unsigned long out_cap)
{
    Inf s;
    int last, type;
    memset(&s, 0, sizeof(s));
    s.in = in; s.in_len = in_len;
    s.out = out; s.out_cap = out_cap;
    do {
        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            if (inf_stored(&s) != 0) return -1;
        } else if (type == 1) {
            Huff lit, dist;
            fixed_tables(&lit, &dist);
            if (inf_block(&s, &lit, &dist) != 0) return -1;
        } else if (type == 2) {
            if (inf_dynamic(&s) != 0) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return (s.out_pos == out_cap) ? 0 : -1;
}

/* --------------------------------------------------------- 内置 blob 取值 */
static const unsigned char *blob_get(unsigned long *len)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_DRVPKG), (LPCWSTR)RT_RCDATA);
    if (r) {
        HGLOBAL mem = LoadResource(NULL, r);
        if (mem) {
            void *p = LockResource(mem);
            DWORD sz = SizeofResource(NULL, r);
            if (p && sz) { *len = (unsigned long)sz; return (const unsigned char *)p; }
        }
    }
#ifdef MICLINK_DRVPKG_EMBED_ARRAY
    /* 回退：编译进 .rodata 的 C 数组（与 .rsrc 同源），仅供单测使用 */
    *len = (unsigned long)DRVPKG_BLOB_LEN;
    return drvpkg_blob;
#else
    return NULL;
#endif
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* 取出 arch 这一套的 kind（"inf"/"sys"/"cat"）解压结果；返回 1 成功。
 * *buf 由 malloc 分配，调用方 free。 */
static int entry_get(const unsigned char *blob, unsigned long blob_len,
                     int arch, const char *kind,
                     unsigned char **buf, unsigned long *len)
{
    unsigned long ver, count, i;
    int kind_idx;
    if (blob_len < 16 || memcmp(blob, DRVPKG_BLOB_MAGIC, 8) != 0) {
        set_err("内置驱动包 magic 不符");
        return 0;
    }
    ver = rd32(blob + 8);
    count = rd32(blob + 12);
    if (ver != 1 || count > 8 || blob_len < 16 + count * 48) {
        set_err("内置驱动包头损坏 ver=%lu count=%lu", ver, count);
        return 0;
    }
    if (count == 0) {
        /* 空包（tools/pack_driver.py --empty）：源码库不内置第三方驱动二进制，
         * 这是干净检出的正常形态，不是损坏。提示要可操作。 */
        set_err("未内置驱动包（空包）：请用 --driver-pack <dir> 指定驱动目录，"
                "或从官方渠道（wolicheng.com/womic）获取后重新打包");
        return 0;
    }
    if (!strcmp(kind, "inf")) kind_idx = 0;
    else if (!strcmp(kind, "sys")) kind_idx = 1;
    else if (!strcmp(kind, "cat")) kind_idx = 2;
    else { set_err("未知的驱动文件类型 %s", kind); return 0; }

    for (i = 0; i < count; i++) {
        const unsigned char *e = blob + 16 + i * 48;
        unsigned long aid = rd32(e);
        unsigned long raw_len = rd32(e + 4 + kind_idx * 12);
        unsigned long cmp_len = rd32(e + 8 + kind_idx * 12);
        unsigned long ofs = rd32(e + 12 + kind_idx * 12);
        if (aid != (unsigned long)arch) continue;
        if (ofs + cmp_len > blob_len) { set_err("驱动包偏移越界"); return 0; }
        *buf = (unsigned char *)malloc(raw_len ? raw_len : 1);
        if (!*buf) { set_err("内存不足（需要 %lu 字节）", raw_len); return 0; }
        if (raw_len == 0) { *len = 0; return 1; }
        if (inf_raw(blob + ofs, cmp_len, *buf, raw_len) != 0) {
            free(*buf); *buf = NULL;
            set_err("驱动包解压失败 arch=%d kind=%s", arch, kind);
            return 0;
        }
        *len = raw_len;
        return 1;
    }
    set_err("内置驱动包里没有 arch=%d 的套件", arch);
    return 0;
}

/* --------------------------------------------------------------- 架构 */
int drvpkg_detect_arch(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);   /* 不用 IsWow64Process：x64 上的 32 位进程会误判 */
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return DRVARCH_AMD64;
    case PROCESSOR_ARCHITECTURE_ARM64: return DRVARCH_ARM64;
    case PROCESSOR_ARCHITECTURE_INTEL: return DRVARCH_INTEL;
    default: return DRVARCH_UNKNOWN;
    }
}

const wchar_t *drvpkg_arch_dir(int arch)
{
    switch (arch) {
    case DRVARCH_AMD64: return L"amd64";
    case DRVARCH_ARM64: return L"arm64";
    case DRVARCH_INTEL: return L"x86";
    default: return L"unknown";
    }
}

const wchar_t *drvpkg_arch_label(int arch)
{
    switch (arch) {
    case DRVARCH_AMD64: return L"x64 (NTamd64)";
    case DRVARCH_ARM64: return L"ARM64 (NTARM64)";
    case DRVARCH_INTEL: return L"x86 (NTx86)";
    default: return L"未知架构";
    }
}

const char *drvpkg_prefix(int arch)
{
    switch (arch) {
    case DRVARCH_AMD64: return "womic_1";
    case DRVARCH_ARM64: return "womic_2";
    case DRVARCH_INTEL: return "womic";
    default: return "";
    }
}

int drvpkg_available(int arch)
{
    unsigned long len = 0;
    const unsigned char *blob = blob_get(&len);
    unsigned long ver, count, i;
    if (!blob || len < 16 || memcmp(blob, DRVPKG_BLOB_MAGIC, 8) != 0) return 0;
    ver = rd32(blob + 8);
    count = rd32(blob + 12);
    if (ver != 1 || count > 8 || len < 16 + count * 48) return 0;
    for (i = 0; i < count; i++)
        if ((int)rd32(blob + 16 + i * 48) == arch) return 1;
    return 0;
}

const char *drvpkg_origin(void)
{
    static char buf[112];
    unsigned long len = 0;
    const unsigned char *blob = blob_get(&len);
    int from_rsrc = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_DRVPKG),
                                  (LPCWSTR)RT_RCDATA) != NULL;
    if (!blob || len < 16 || memcmp(blob, DRVPKG_BLOB_MAGIC, 8) != 0) {
        _snprintf(buf, sizeof(buf), "未内置驱动包（请用 --driver-pack 指定目录）");
    } else if (rd32(blob + 12) == 0) {
        _snprintf(buf, sizeof(buf), "空包（未内置第三方驱动二进制，请用 --driver-pack 指定目录）");
    } else if (from_rsrc) {
        _snprintf(buf, sizeof(buf), "内置资源 (.rsrc, deflate %lu B)", len);
    } else {
        _snprintf(buf, sizeof(buf), "内置数组 (.rodata, deflate %lu B)", len);
    }
    return buf;
}

/* --------------------------------------------------------------- 目录 */
static int make_dirs(const wchar_t *dir)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *p;
    if (!dir[0]) return 0;
    wcsncpy(tmp, dir, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = 0;
    /* 跳过盘符/UNC 前缀，逐段创建 */
    for (p = tmp; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            if (wcslen(tmp) > 3) CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
    return GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES &&
           (GetFileAttributesW(dir) & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

int drvpkg_target_dir(int arch, wchar_t *dir, int cap)
{
    wchar_t progdata[MAX_PATH] = { 0 };
    /* SHGetKnownFolderPath 动态加载：避免为 knownfolders/uuid 增加链接期依赖 */
    typedef HRESULT (WINAPI *fn_kf)(const GUID *, DWORD, HANDLE, PWSTR *);
    HMODULE sh = GetModuleHandleW(L"shell32.dll");
    fn_kf kf = sh ? (fn_kf)(void *)GetProcAddress(sh, "SHGetKnownFolderPath") : NULL;
    PWSTR out = NULL;
    /* FOLDERID_ProgramData = {62AB5D82-FDC1-4DC3-A9DD-070D1D495D97} */
    static const GUID kProgramData = {
        0x62AB5D82u, 0xFDC1u, 0x4DC3u, { 0xA9u, 0xDDu, 0x07u, 0x0Du, 0x1Du, 0x49u, 0x5Du, 0x97u }
    };
    if (kf && kf(&kProgramData, 0, NULL, &out) == 0 && out) {
        wcsncpy(progdata, out, MAX_PATH - 1);
        CoTaskMemFree(out);
    }
    if (!progdata[0]) {
        if (!GetEnvironmentVariableW(L"ProgramData", progdata, MAX_PATH) || !progdata[0])
            wcscpy(progdata, L"C:\\ProgramData");
    }
    _snwprintf(dir, cap, L"%s\\MicLink\\driver\\%s", progdata, drvpkg_arch_dir(arch));
    return 1;
}

/* --------------------------------------------------------------- 落地 */
static int write_file(const wchar_t *path, const unsigned char *data, unsigned long len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (h == INVALID_HANDLE_VALUE) {
        set_err("无法写入 %ls (gle=%lu)", path, GetLastError());
        return 0;
    }
    if (!WriteFile(h, data, (DWORD)len, &written, NULL) || written != (DWORD)len) {
        CloseHandle(h);
        set_err("写入不完整 %ls (%lu/%lu)", path, (unsigned long)written, len);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

/* 把 src_dir 下的 <prefix>.inf/.sys/.cat 复制成 out_dir 下的 womic.inf/.sys/.cat。
 * 兼容两种情况：原名（已是 womic.*）与带后缀名（womic_1.*）。
 * 文件名必须成套改成原名，否则 catalog 哈希对不上 → 0xE000024B。 */
static int copy_three(const wchar_t *src_dir, const char *prefix, const wchar_t *out_dir)
{
    static const char *kinds[3] = { "inf", "sys", "cat" };
    int i;
    for (i = 0; i < 3; i++) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        _snwprintf(src, MAX_PATH, L"%s\\%hs.%s", src_dir, prefix, kinds[i]);
        _snwprintf(dst, MAX_PATH, L"%s\\womic.%s", out_dir, kinds[i]);
        if (!CopyFileW(src, dst, FALSE)) {
            set_err("复制失败 %ls → %ls (gle=%lu)", src, dst, GetLastError());
            return 0;
        }
    }
    return 1;
}

int drvpkg_extract_dir(const wchar_t *src_dir, const wchar_t *out_dir, int force)
{
    wchar_t probe[MAX_PATH];
    int arch = drvpkg_detect_arch();
    const char *prefixes[3];
    int i;
    prefixes[0] = drvpkg_prefix(arch);
    prefixes[1] = "womic";
    prefixes[2] = NULL;

    if (!force) {
        _snwprintf(probe, MAX_PATH, L"%s\\womic.inf", out_dir);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    if (!make_dirs(out_dir)) { set_err("无法创建目录 %ls", out_dir); return 0; }
    for (i = 0; prefixes[i]; i++) {
        _snwprintf(probe, MAX_PATH, L"%s\\%hs.inf", src_dir, prefixes[i]);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES)
            return copy_three(src_dir, prefixes[i], out_dir);
    }
    set_err("%ls 下找不到 womic*.inf（需要 womic.inf/womic.sys/womic.cat 三件套）", src_dir);
    return 0;
}

int drvpkg_extract(int arch, const wchar_t *out_dir, int force)
{
    unsigned long blob_len = 0;
    const unsigned char *blob = blob_get(&blob_len);
    static const char *kinds[3] = { "inf", "sys", "cat" };
    int i;

    if (!blob) {
        set_err("未内置驱动包：请用 --driver-pack <dir> 指定驱动目录，"
                "或从官方渠道（wolicheng.com/womic）获取后重新打包");
        return 0;
    }
    if (!force) {
        wchar_t probe[MAX_PATH];
        _snwprintf(probe, MAX_PATH, L"%s\\womic.inf", out_dir);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    if (!make_dirs(out_dir)) { set_err("无法创建目录 %ls", out_dir); return 0; }

    for (i = 0; i < 3; i++) {
        unsigned char *buf = NULL;
        unsigned long len = 0;
        wchar_t path[MAX_PATH];
        int ok;
        if (!entry_get(blob, blob_len, arch, kinds[i], &buf, &len)) return 0;
        _snwprintf(path, MAX_PATH, L"%s\\womic.%hs", out_dir, kinds[i]);
        ok = write_file(path, buf, len);
        free(buf);
        if (!ok) return 0;
    }
    return 1;
}

static int del_tree(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf(child, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) del_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
    return 1;
}

int drvpkg_cleanup(const wchar_t *dir)
{
    if (!dir || !dir[0]) return 0;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return 1;
    return del_tree(dir);
}
