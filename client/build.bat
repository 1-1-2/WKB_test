@echo off
rem WO Mic Link — Windows 原生构建（mingw-w64 的 gcc 在 PATH 即可，无需 VS/SDK）
rem 用法：build.bat [all^|app^|drvinst]
rem   all     （默认）两个产物都编
rem   app     只编 womic.exe（应用本体，不含第三方驱动二进制）
rem   drvinst 只编 womic-drvinst.exe（驱动安装器，唯一携带驱动包）
rem
rem 与 build.sh 等价：编资源（windres）×2 + 静态链接 opus decoder + 体积/导入表/资源面自检。
rem 不用 MSVC：MSVC 产物依赖 vcruntime140.dll，正是本项目要避免的额外运行库。
rem
rem 注意：本文件含 UTF-8 中文注释。Windows PowerShell 5.1 用 GBK 读无 BOM 的脚本会
rem 乱码，但 cmd.exe 有自己的代码页行为，这里统一保持 UTF-8 且注释不影响执行。

setlocal
pushd "%~dp0"

set CC=gcc
set WINDRES=windres
set MODE=%1
if "%MODE%"=="" set MODE=all
if not exist build mkdir build

rem 大小预算：应用与安装器分开设（见 build.sh 顶部注释）
set APP_OUT=build\womic.exe
set DRVINST_OUT=build\womic-drvinst.exe
if "%SIZE_BUDGET_APP%"=="" set SIZE_BUDGET_APP=327680
if "%SIZE_BUDGET_DRVINST%"=="" set SIZE_BUDGET_DRVINST=262144

echo ============================================
echo  WO Mic Link build  (mode=%MODE%)
echo ============================================

if "%MODE%"=="all" goto :gen
if "%MODE%"=="app" goto :gen
if "%MODE%"=="drvinst" goto :gen
echo BUILD FAILED: 未知 mode=%MODE%（应为 all/app/drvinst）
popd
endlocal
exit /b 1

rem ---------------------------------------------------------- 生成内置资源
rem 第三方已签名驱动二进制不入库（.gitignore），干净检出时找不到官方驱动目录就
rem 用 --empty 生成合法空包兜底（16 B, count=0），构建不中断；那种安装器需
rem /driver-pack ^<dir^> 外置驱动。app.ico 是自绘图标，两种模式都能生成。
rem 注意 drvpkg.bin 只被 res\drvinst.rc 引用：应用本体不带驱动包。
:gen
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
echo [3/4] windres
"%WINDRES%" -I res -i res\app.rc -o build\app_res.o
if errorlevel 1 goto :fail
"%WINDRES%" -I res -i res\drvinst.rc -o build\drvinst_res.o
if errorlevel 1 goto :fail

rem ---------------------------------------------------------- 编译（应用）
if "%MODE%"=="all" goto :build_app
if "%MODE%"=="app" goto :build_app
goto :build_drvinst

:build_app
echo [4/4] compiling %APP_OUT%
"%CC%" -Os -s -mwindows -std=c99 -Wall -D_WIN32_WINNT=0x0A00 ^
    %OPUSCFG% -Wl,--gc-sections -o "%APP_OUT%" ^
    src\main.c src\engine.c src\devio.c src\opusdec.c src\drvpkg.c src\drvinst.c ^
    src\singleinst.c src\cli.c src\theme.c src\ui_draw.c src\ui_layout.c ^
    src\ui_voicemeter.c src\ui_help.c src\drvbridge.c ^
    build\app_res.o %OPUSLIB% ^
    -lgdi32 -luser32 -lws2_32 -luxtheme -ldwmapi -lshell32 -lole32 ^
    -lsetupapi -lcrypt32 -lnewdev -ladvapi32
if errorlevel 1 goto :fail
python tools\check_pe.py "%APP_OUT%" app
if errorlevel 1 goto :fail
if "%MODE%"=="app" goto :done

rem ---------------------------------------------------------- 编译（安装器）
:build_drvinst
echo [4/4] compiling %DRVINST_OUT%
"%CC%" -Os -s -mconsole -std=c99 -Wall -D_WIN32_WINNT=0x0A00 ^
    -Wl,--gc-sections -o "%DRVINST_OUT%" ^
    src\drvinst_main.c src\drvinst.c src\drvpkg.c src\devio.c ^
    build\drvinst_res.o ^
    -lsetupapi -lnewdev -lcrypt32 -ladvapi32 -lole32 -lshell32
if errorlevel 1 goto :fail
python tools\check_pe.py "%DRVINST_OUT%" drvinst
if errorlevel 1 goto :fail

:done
for %%F in ("%APP_OUT%") do if exist "%%~fF" echo built: %%~fF  (%%~zF bytes)
for %%F in ("%DRVINST_OUT%") do if exist "%%~fF" echo built: %%~fF  (%%~zF bytes)
echo done.
popd
endlocal
exit /b 0

:fail
echo BUILD FAILED
popd
endlocal
exit /b 1
