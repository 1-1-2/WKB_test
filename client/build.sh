#!/usr/bin/env bash
# 交叉编译 WO Mic Link 客户端（Linux → Windows x64）
#
# 依赖：gcc-mingw-w64-x86-64（apt install gcc-mingw-w64-x86-64）
#       python3（生成驱动包与图标；驱动产物不入库，干净检出时现场生成，见下）
#       deps/opus/（首次跑 tools/build_opus.sh；缺失时自动降级为 opus.dll 动态加载）
#
# 产物：build/womic.exe
#   - 单文件、只依赖 Windows 自带 DLL
#   - 官方驱动在时：内置三套官方驱动（RT_RCDATA，deflate）→ ~396 KB
#   - 官方驱动不在时：空包兜底（16 B，count=0）→ ~300 KB，靠 --driver-pack 外置驱动
#   - 内嵌 VS_VERSION_INFO / 图标 / asInvoker manifest
#
# 自检：构建后打印体积与导入表，并在体积超预算或导入表出现非系统 DLL 时报错。
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
PYTHON=${PYTHON:-python3}
MODE=${1:-exe}
OUT=build/womic.exe
if [ "$MODE" = "test" ]; then OUT=build/test_drvpkg.exe; fi
SIZE_BUDGET=${SIZE_BUDGET:-460800}   # 450 KiB

mkdir -p build

# ---------------------------------------------------------------- 资源生成
# 内置驱动包与图标是生成物；源码树上没有时才重新生成（保证离线可构建）。
#
# ⚠ 第三方已签名驱动（womic.sys 等）的二进制**不进入公开源码库**（架构文档 R8），
#   res/drvpkg.bin 与 src/drvpkg_data.h 都在 .gitignore 里。因此：
#   1) 先尝试真实打包（tools/pack_driver.py 会按优先级找官方驱动目录）；
#   2) 找不到驱动时用 --empty 生成合法空包（magic 正确、ver=1、count=0，16 B）
#      兜底，只打 warned 日志、不中断构建 —— 那种 exe 靠 --driver-pack 外置驱动；
#   3) app.ico 是自绘图标、与驱动无关，两种模式下都保证能生成。
if [ ! -f src/drvpkg_data.h ] || [ ! -f res/drvpkg.bin ] || [ ! -f res/app.ico ]; then
    echo "==> 生成内置驱动包与图标"
    if ! "$PYTHON" tools/pack_driver.py; then
        echo "!! warned: 未找到官方驱动目录，改用空包模式（exe 不含内置驱动，" \
             "运行时请用 --driver-pack <dir> 指定）" >&2
        "$PYTHON" tools/pack_driver.py --empty
    fi
fi

# ---------------------------------------------------------------- Opus
# 静态优先：有 deps/opus 就 -DMICLINK_HAVE_OPUS，opusdec.c 直接调用静态符号；
# 没有就编成“运行时 LoadLibrary(opus.dll)”，功能不缺，只是少一层保障。
OPUS_CFLAGS=()
OPUS_LIBS=()
if [ -f deps/opus/lib/libopus.a ]; then
    echo "==> Opus：静态链接（deps/opus）"
    OPUS_CFLAGS=(-Ideps/opus/include -DMICLINK_HAVE_OPUS=1)
    OPUS_LIBS=(-Ldeps/opus/lib -lopus)
else
    echo "==> Opus：未找到 deps/opus，改为运行时加载 opus.dll（tools/build_opus.sh 可补建）"
fi

# ---------------------------------------------------------------- 资源
echo "==> windres res/app.rc"
"$WINDRES" -I res -i res/app.rc -o build/app_res.o

# ---------------------------------------------------------------- 编译
# -Os            体积优先
# -s             去掉符号表
# -mwindows      GUI 子系统（双击不闪黑窗；CLI 输出靠 AttachConsole 兜）
# --gc-sections  回收未引用段（把 libopus 的 encoder 全部裁掉）
#
# ⚠ 不要加 -ffunction-sections / -fdata-sections：
#   实测加上后产物从 81 KB 涨到 113 KB（+36%）。这两个选项把每个变量拆成独立
#   section，触发 mingw 的对齐填充，导致 .data 段里 99.7% 是零字节。
#   此结论基于实测五种参数组合，勿轻易推翻。
# ------------------------------------------------------------ 单测模式
# ./build.sh test —— 编一个只含 drvpkg 的小 exe，校验 9 个驱动文件的字节一致性
if [ "$MODE" = "test" ]; then
    echo "==> 编解压自检程序"
    "$CC" -Os -std=c99 -Wall -D_WIN32_WINNT=0x0A00 -DMICLINK_DRVPKG_EMBED_ARRAY=1 \
        -o "$OUT" tools/test_drvpkg.c src/drvpkg.c \
        -lkernel32 -luser32 -lshell32 -lole32
    if command -v wine >/dev/null 2>&1; then
        echo "==> wine 下运行"
        # 空包构建下没有可校验的驱动文件：test_drvpkg.exe 报告 SKIP 并以退出码 2
        # 结束（不假装 PASS）。显式接住退出码，别让 set -e 抢先退出、丢了这行日志。
        rc=0
        WINEDEBUG=-all wine "$OUT" || rc=$?
        echo "==> test_drvpkg 退出码 $rc"
        exit $rc
    fi
    echo "未安装 wine，已产出 $OUT，请拷贝到 Windows 上运行"
    exit 0
fi

SRC=(
    src/main.c
    src/engine.c
    src/devio.c
    src/opusdec.c
    src/drvpkg.c
    src/drvinst.c
    src/singleinst.c
    src/cli.c
    src/theme.c
    src/ui_draw.c
    src/ui_layout.c
    src/ui_voicemeter.c
    src/ui_help.c
)

echo "==> 编译链接"
"$CC" -Os -s -mwindows \
    -std=c99 -Wall -Wno-unused-function \
    -D_WIN32_WINNT=0x0A00 \
    "${OPUS_CFLAGS[@]}" \
    -Wl,--gc-sections \
    -o "$OUT" \
    "${SRC[@]}" \
    build/app_res.o \
    "${OPUS_LIBS[@]}" \
    -lgdi32 -luser32 -lws2_32 -luxtheme -ldwmapi -lshell32 -lole32 \
    -lsetupapi -lcrypt32 -lnewdev -ladvapi32

# ---------------------------------------------------------------- 自检
SIZE=$(stat -c %s "$OUT" 2>/dev/null || stat -f %z "$OUT")
echo "built : $OUT  (${SIZE} bytes / $((SIZE/1024)) KiB)"

IMPORTS=$(x86_64-w64-mingw32-objdump -p "$OUT" | awk '/DLL Name:/{print $NF}' | sort -u)
echo "imports: $(echo "$IMPORTS" | tr '\n' ' ')"

# 只允许系统 DLL：libgcc/libwinpthread/vcruntime 出现即为失败
BAD=$(echo "$IMPORTS" | grep -iE "libgcc|libwinpthread|libstdc|vcruntime|msvcp|api-ms-win-crt|^libopus|opus" || true)
if [ -n "$BAD" ]; then
    echo "!! 导入表含非系统 DLL: $BAD" >&2
    exit 1
fi
if [ "$SIZE" -gt "$SIZE_BUDGET" ]; then
    echo "!! 产物 ${SIZE} B 超过预算 ${SIZE_BUDGET} B" >&2
    exit 1
fi

# 资源是否真的编进去了（版本信息 + 内置驱动包）
"$PYTHON" - "$OUT" <<'PY'
import sys, struct
path = sys.argv[1]
data = open(path, 'rb').read()
ok = True
def chk(cond, msg):
    global ok
    print(("ok  : " if cond else "FAIL: ") + msg)
    if not cond: ok = False
# .rsrc 段
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
secs = []
off = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
for i in range(nsec):
    name = data[off + i * 40: off + i * 40 + 8].rstrip(b'\0').decode()
    vsz = struct.unpack_from('<I', data, off + i * 40 + 16)[0]
    raw = struct.unpack_from('<I', data, off + i * 40 + 20)[0]
    secs.append((name, vsz, raw))
rsrc = [(v, r) for n, v, r in secs if n == '.rsrc']
chk(bool(rsrc) and rsrc[0][0] > 8192, ".rsrc 段存在且非空 (%d B)" % (rsrc[0][0] if rsrc else 0))
chk("VS_VERSION_INFO".encode('utf-16-le') in data, "VS_VERSION_INFO 已嵌入")
# 内置驱动包：只在 .rsrc 里找 magic。不能对整个文件 find —— .rdata 里还有一份
# "MICLDRV1" 字符串常量（drvpkg.c 的 memcmp 参数），取第一个会读到乱码。
if rsrc:
    vsz, raw = rsrc[0]
    m = data.find(b"MICLDRV1", raw, raw + vsz)
    if m >= 0 and m + 16 <= raw + vsz:
        ver, count = struct.unpack_from("<II", data, m + 8)
        if ver == 1 and count <= 8:
            if count == 0:
                # 空包兜底构建：源码库不内置第三方驱动二进制，这是预期形态，不算失败
                print("note: 空包构建（未内置第三方驱动二进制），运行时用 --driver-pack <dir>")
            chk(True, "内置驱动包 (MICLDRV1) 已嵌入 (ver=%d count=%d)" % (ver, count))
        else:
            chk(False, "内置驱动包头不合法 (ver=%d count=%d)" % (ver, count))
    else:
        chk(False, "内置驱动包 (MICLDRV1) 已嵌入")
else:
    chk(False, "内置驱动包 (MICLDRV1) 已嵌入")
chk("WO Mic Link".encode("utf-16-le") in data, "版本信息里的产品名存在")
sys.exit(0 if ok else 1)
PY
if [ $? -ne 0 ]; then
    echo "!! 资源自检未通过" >&2
    exit 1
fi
echo "OK    : 体积与导入表自检通过"
