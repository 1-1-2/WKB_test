/* drvpkg.h — 内置驱动包的解压与落地
 *
 * 三套 2022 版（2.1.0.0）官方驱动经过 raw deflate 后嵌在 exe 里：
 *   优先从 .rsrc（RT_RCDATA, id=101）取 → 失败回退到 src/drvpkg_data.h 的 C 数组。
 *   两条路径由 tools/pack_driver.py 同一次生成，数据完全一致。
 *
 * 落地三条铁律（违反任一都会重现 0xe0000228 / 0xE000024B / 0xE0000242）：
 *   1. 落在 %ProgramData%\MicLink\driver\<arch>\（不用 %TEMP%，会被清理工具中途删掉）；
 *   2. 三件套必须成套改回原名 womic.inf / womic.sys / womic.cat
 *      —— CAT 里是按文件名索引的哈希，改名不一致就 0xE000024B；
 *   3. 装完确认成功后删除释放目录（由 drvinst 调 drvpkg_cleanup）。
 */
#ifndef MICLINK_DRVPKG_H
#define MICLINK_DRVPKG_H

#include "app.h"

/* 资源 id，与 res/app.rc 的 `101 RCDATA` 对应 */
#ifndef IDR_DRVPKG
#define IDR_DRVPKG 101
#endif

/* 内置包 magic。正常由 src/drvpkg_data.h 定义，但那个文件内含第三方驱动二进制、
 * 不进入公开源码库（干净检出时缺失），所以这里给一份同样的兜底定义：
 * drvpkg_data.h 里是逐字相同的宏，重复定义属“良性重定义”，不会告警。 */
#ifndef DRVPKG_BLOB_MAGIC
#define DRVPKG_BLOB_MAGIC "MICLDRV1"
#endif

/* 内置包里三件套的原始文件名前缀（2022 版） */
const char *drvpkg_prefix(int arch);

/* 落地目录：%ProgramData%\MicLink\driver\<arch>\，dir 需容纳 MAX_PATH 个 wchar_t */
int  drvpkg_target_dir(int arch, wchar_t *dir, int cap);

/* 删除释放目录（含其下所有文件） */
int  drvpkg_cleanup(const wchar_t *dir);

/* 驱动包来源描述，用于 --status / GUI 展示 */
const char *drvpkg_origin(void);

#endif /* MICLINK_DRVPKG_H */
