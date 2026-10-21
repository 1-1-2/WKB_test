@echo off
rem 在 Windows 上构建（需要 mingw-w64 的 gcc 在 PATH 中）
rem 推荐用 winlibs 的免安装包，解压后把 bin 目录加入 PATH 即可，无需装 VS/SDK。
setlocal
if not exist build mkdir build
gcc -Os -s -mwindows -ffunction-sections -fdata-sections ^
    -o build\womic.exe src\main.c src\engine.c ^
    -lgdi32 -luser32 -lws2_32 -luxtheme -ldwmapi -lshell32 -lole32
if errorlevel 1 (
  echo 构建失败：请确认 gcc.exe 在 PATH 中
  exit /b 1
)
for %%F in (build\womic.exe) do echo built: %%~fF  (%%~zF bytes)
endlocal
