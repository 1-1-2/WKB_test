@echo off
rem WO Mic Link — Windows 原生构建（mingw-w64 的 gcc 在 PATH 即可，无需 VS/SDK）
rem 用法：build.bat [输出路径]
rem
rem 与 build.sh 等价：编资源（windres）+ 静态链接 opus decoder + 体积/导入表自检。
rem 不用 MSVC：MSVC 产物依赖 vcruntime140.dll，正是本项目要避免的额外运行库。

setlocal
pushd "%~dp0"

set CC=gcc
set WINDRES=windres
set OUT=%1
if "%OUT%"=="" set OUT=build\womic.exe
if not exist build mkdir build

echo ============================================
echo  WO Mic Link build
echo ============================================

rem ---------------------------------------------------------- 生成内置资源
rem 第三方已签名驱动二进制不入库（.gitignore），干净检出时找不到官方驱动目录就
rem 用 --empty 生成合法空包兜底（16 B, count=0），构建不中断；那种 exe 靠
rem --driver-pack ^<dir^> 外置驱动工作。app.ico 是自绘图标，两种模式都能生成。
set NEEDGEN=0
if not exist "src\drvpkg_data.h" set NEEDGEN=1
if not exist "res\drvpkg.bin" set NEEDGEN=1
if not exist "res\app.ico" set NEEDGEN=1
if "%NEEDGEN%"=="1" (
    echo [1/4] generating driver pack + icon
    python tools\pack_driver.py
    if errorlevel 1 (
        echo [1/4] warned: driver dir not found, falling back to empty pack
        python tools\pack_driver.py --empty
        if errorlevel 1 goto :fail
    )
) else (
    echo [1/4] generated resources already present
)

rem ---------------------------------------------------------- Opus
set OPUSCFG=
set OPUSLIB=
if exist "deps\opus\lib\libopus.a" (
    echo [2/4] opus: static ^(deps\opus^)
    set OPUSCFG=-Ideps\opus\include -DMICLINK_HAVE_OPUS=1
    set OPUSLIB=-Ldeps\opus\lib -lopus
) else (
    echo [2/4] opus: not found, falling back to runtime opus.dll
)

rem ---------------------------------------------------------- 资源
echo [3/4] windres res\app.rc
"%WINDRES%" -I res -i res\app.rc -o build\app_res.o
if errorlevel 1 goto :fail

rem ---------------------------------------------------------- 编译
echo [4/4] compiling
"%CC%" -Os -s -mwindows -std=c99 -Wall -D_WIN32_WINNT=0x0A00 ^
    %OPUSCFG% -Wl,--gc-sections -o "%OUT%" ^
    src\main.c src\engine.c src\devio.c src\opusdec.c src\drvpkg.c src\drvinst.c ^
    src\singleinst.c src\cli.c src\theme.c src\ui_draw.c src\ui_layout.c ^
    src\ui_voicemeter.c src\ui_help.c ^
    build\app_res.o %OPUSLIB% ^
    -lgdi32 -luser32 -lws2_32 -luxtheme -ldwmapi -lshell32 -lole32 ^
    -lsetupapi -lcrypt32 -lnewdev -ladvapi32
if errorlevel 1 goto :fail

for %%F in ("%OUT%") do echo built: %%~fF  (%%~zF bytes)
echo done.
popd
endlocal
exit /b 0

:fail
echo BUILD FAILED
popd
endlocal
exit /b 1
