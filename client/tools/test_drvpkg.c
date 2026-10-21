/* tools/test_drvpkg.c — 内置驱动包 inflate 的字节级自检
 *
 * 为什么要单独一个测试：驱动三件套一旦解压错一个字节，装驱动会以
 * 0xE000024B（catalog 哈希不符）或 0xE0000242（签名者不受信）的形式失败，
 * 而这两个错误在真机上排查成本极高。这里在构建机就能把 9 个文件的
 * 长度 + FNV-1a-64 与打包时的原始值对一遍。
 *
 * 编译（Linux 交叉 + wine 跑，或 Windows 原生）：
 *   x86_64-w64-mingw32-gcc -Os -std=c99 -o build/test_drvpkg.exe \
 *       tools/test_drvpkg.c src/drvpkg.c -lkernel32 -luser32 -lshell32 -lole32
 *   wine build/test_drvpkg.exe
 *
 * 空包构建（tools/pack_driver.py --empty，源码库不内置第三方驱动二进制）下没有
 * 可校验的文件：明确报告 SKIP 并以退出码 2 结束，绝不假装 PASS。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/drvpkg.h"
#include "../src/drvpkg_data.h"

static unsigned long long fnv1a64(const unsigned char *p, unsigned long n)
{
    unsigned long long h = 0xCBF29CE484222325ULL;
    unsigned long i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned long long)p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* 与 drvpkg.c::rd32 同口径：小端 u32 */
static unsigned long rd32b(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned char *slurp(const wchar_t *path, unsigned long *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD sz, got = 0;
    unsigned char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE) { CloseHandle(h); return NULL; }
    buf = (unsigned char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, sz, &got, NULL) || got != sz) {
        free(buf); CloseHandle(h); return NULL;
    }
    CloseHandle(h);
    *len = (unsigned long)sz;
    return buf;
}

int main(void)
{
    wchar_t base[MAX_PATH], arch_dir[MAX_PATH];
    int arch, i, fail = 0;
    static const int archs[3] = { DRVARCH_INTEL, DRVARCH_AMD64, DRVARCH_ARM64 };

    GetTempPathW(MAX_PATH, base);
    wcsncat(base, L"miclink-drvtest", 32);

    printf("blob bytes = %u, expect entries = %d\n",
           (unsigned)DRVPKG_BLOB_LEN, DRVPKG_EXPECT_N);
    if (DRVPKG_BLOB_LEN < 16 || memcmp(drvpkg_blob, DRVPKG_BLOB_MAGIC, 8) != 0) {
        printf("FAIL: 内置包长度或 magic 不符（len=%u）\n", (unsigned)DRVPKG_BLOB_LEN);
        return 1;
    }
    /* 空包（tools/pack_driver.py --empty，count=0）：源码库不内置第三方驱动二进制，
     * 干净检出的正常形态。明确报告“无内置包”并非零退出 —— 不假装 PASS，
     * 也不要把空包当成“包头损坏”报错。 */
    if (rd32b(drvpkg_blob + 12) == 0 || DRVPKG_EXPECT_N == 0) {
        printf("SKIP: 无内置驱动包（空包构建，tools/pack_driver.py --empty）\n");
        printf("      需要校验 9 个驱动文件时，请自备官方驱动目录后重跑：\n");
        printf("      python3 tools/pack_driver.py --driver-dir <dir>\n");
        printf("RESULT=SKIP\n");
        return 2;
    }

    for (arch = 0; arch < 3; arch++) {
        int a = archs[arch];
        _snwprintf(arch_dir, MAX_PATH, L"%s\\%s", base, drvpkg_arch_dir(a));
        if (!drvpkg_extract(a, arch_dir, 1)) {
            printf("FAIL: arch=%d 解压失败: %s\n", a, drvpkg_last_error());
            fail++;
            continue;
        }
        for (i = 0; i < DRVPKG_EXPECT_N; i++) {
            if (drvpkg_expect[i].arch != a) continue;
            wchar_t path[MAX_PATH];
            unsigned char *buf;
            unsigned long len = 0;
            unsigned long long h;
            _snwprintf(path, MAX_PATH, L"%s\\womic.%hs", arch_dir, drvpkg_expect[i].kind);
            buf = slurp(path, &len);
            if (!buf) { printf("FAIL: 读回失败 %ls\n", path); fail++; continue; }
            h = fnv1a64(buf, len);
            if (len != drvpkg_expect[i].raw_len || h != drvpkg_expect[i].fnv) {
                printf("FAIL: arch=%d %s len=%lu/%lu fnv=%016llX/%016llX\n",
                       a, drvpkg_expect[i].kind, len, drvpkg_expect[i].raw_len,
                       h, drvpkg_expect[i].fnv);
                fail++;
            } else {
                printf("ok  : arch=%d womic.%s  %lu B  fnv=%016llX\n",
                       a, drvpkg_expect[i].kind, len, h);
            }
            free(buf);
        }
        if (!drvpkg_cleanup(arch_dir)) { printf("warn: 清理失败 %ls\n", arch_dir); }
    }
    drvpkg_cleanup(base);
    printf(fail ? "RESULT=FAIL (%d)\n" : "RESULT=PASS\n", fail);
    return fail ? 1 : 0;
}
