# womicserver Windows 构建说明

`womicserver` 是 Windows 虚拟麦克风系统的用户态服务端：
Android 手机采集 → UDP/Opus → **本进程** → `DeviceIoControl` → 内核虚拟麦克风驱动。

## 1. 依赖

| 依赖 | 必需 | 用途 | vcpkg 包名 |
|---|---|---|---|
| libopus | ✅ 必需 | Opus 解码 + PLC（丢包隐藏） | `opus` |
| libsoxr | 推荐 | 服务端重采样后端（音质更好） | `soxr` |
| libspeexdsp | 可选 | 备用重采样后端（依赖更小） | `speexdsp` |

```powershell
# 以 x64 静态链接为例（服务端是单文件绿色部署，优先静态）
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install opus:x64-windows-static soxr:x64-windows-static speexdsp:x64-windows-static
```

## 2. 用 CMake + vcpkg 构建

```powershell
cmake -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DWOMIC_WITH_FAKE_DEVICE=OFF
cmake --build build --config Release
```

产物：`build\Release\womicserver.exe`

选项说明：

| CMake 选项 | 默认 | 说明 |
|---|---|---|
| `WOMIC_WITH_FAKE_DEVICE` | Windows 上 OFF | 内置 Fake 设备后端并把 `auto` 解析为它。**Windows 上必须 OFF**，`--backend auto` 才会走真实 IOCTL |
| `WOMIC_WITH_SOXR` | ON | 找到 libsoxr 则启用 |
| `WOMIC_WITH_SPEEXDSP` | ON | 找到 libspeexdsp 则启用 |
| `WOMIC_BUILD_TESTS` | ON | 构建 `test_pipeline`（Windows 上也会跑，用 Fake 后端） |

## 3. 用 MSVC 直接构建（不使用 CMake）

```powershell
cl /O2 /EHsc /std:c++17 /I src /I ..\shared\core\include ^
   /I <vcpkg>\installed\x64-windows-static\include ^
   src\*.cpp src\main.cpp ..\shared\core\src\*.c ^
   /Fe:womicserver.exe ^
   /link ws2_32.lib <vcpkg>\installed\x64-windows-static\lib\opus.lib ^
         <vcpkg>\installed\x64-windows-static\lib\soxr.lib
```

## 4. 运行

```powershell
# 最基本：48000 Hz / 单声道端点，监听 5004
womicserver.exe --port 5004 --rate 48000

# 手机侧是 16 kHz Opus、端点是 48 kHz：服务端做重采样（驱动侧绝不重采样）
womicserver.exe --port 5004 --rate 48000 --opus-rate 16000 --resampler soxr

# 开启发现服务 + 详细日志 + 电平打印
womicserver.exe --discovery --verbose --level-log
```

前置条件：内核驱动 `womic.sys` 已安装并创建了控制设备 `\\.\WomicControl`
（符号链接 `\DosDevices\Global\WomicControl`）。未安装时启动会报
`CreateFileW(\\.\WomicControl) 失败，GetLastError=2`。

退出：Ctrl+C。会依次执行 STOP → RESET → CloseHandle，端点不会停在 RUNNING 状态。

## 5. 与 Linux 冒烟构建的差异

Linux 上：`cmake -B build -DWOMIC_WITH_FAKE_DEVICE=ON && cmake --build build`，
`--backend auto` 会解析成 Fake 后端，因此可以真跑端到端流水线（真实 UDP + 真实 opus）。
Windows 上 `auto` 解析成 IOCTL 后端。

只有 3 个薄适配层含 Win32 代码，其余全部平台中立：

| 文件 | Windows | Linux |
|---|---|---|
| `src/net_socket.cpp` | winsock2 / `closesocket` / `WSAGetLastError` / `ioctlsocket(FIONBIO)` | Berkeley / `close` / `errno` / `fcntl(O_NONBLOCK)` |
| `src/threads_sync.cpp` | `CRITICAL_SECTION` / `CONDITION_VARIABLE` / `SetThreadPriority` | `std::mutex` / `std::condition_variable` / `SCHED_FIFO`+nice |
| `src/injector.cpp` | `CreateFileW` + `DeviceIoControl` | Fake 后端（同状态机，语义等价） |

音频热路径上**没有任何锁**：jitter buffer 与 ring 都是 SPSC 无锁结构，统计用原子量，
因此不存在优先级反转（详见 `src/threads.cpp` 顶部注释）。

## 6. 只能在 Windows 上验证的部分

Linux 构建无法覆盖，必须在真机上验证：

1. `CreateFileW` 打开 `\\.\WomicControl` 与 IOCTL 码的实际对接
   （`GET_VERSION/GET_CAPS/SET_FORMAT/START/WRITE_PCM/GET_STATUS/STOP/RESET`）；
2. `DeviceIoControl` 的错误码映射（`ERROR_INSUFFICIENT_BUFFER → WOMIC_ERR_BUFFER_FULL` 等）；
3. Zoom / OBS / 录音机里能否选中并听到 "Womic Virtual Microphone" 端点；
4. 真机端到端延迟与欠载/溢出计数（内核 DMA 时钟与用户态注入的匹配）。
