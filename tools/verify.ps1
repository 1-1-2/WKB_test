<#
.SYNOPSIS
    安装后的自动检查脚本：输出 PASS / FAIL 清单。

.DESCRIPTION
    检查项：
      1)  管理员权限
      2)  测试签名状态（bcdedit testsigning）—— 测试签名模式下必须为开
      3)  驱动服务 MicLinkVirtualMic 是否存在
      4)  INF 是否已注册到 Driver Store
      5)  PnP 设备是否存在且状态 OK
      6)  设备管理器问题码（重点：无代码 52 = 签名未通过）
      7)  WMI Win32_SoundDevice 中是否出现 "MicLink Virtual Microphone"
      8)  Core Audio 捕获端点（注册表 MMDevices）中是否出现该设备
      9)  控制设备 \\.\MicLinkControl 能否被 CreateFile 打开
      10) IOCTL GET_VERSION 是否能返回版本号
      M)  录音机 / OBS / Zoom 的真实采集验证 —— **必须人工执行**（标记为 MANUAL）

    退出码：0 = 全部通过（MANUAL 项不计入），1 = 存在 FAIL。

.PARAMETER ExpectTestSigning
    $true（默认）时把"未开启 testsigning"算作 FAIL（测试签名安装的常见根因）。
    生产签名环境下用 -ExpectTestSigning:$false。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\verify.ps1
#>
#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [bool]$ExpectTestSigning = $true
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:DriverFriendlyName = 'MicLink Virtual Microphone'
$script:DriverServiceName = 'MicLinkVirtualMic'
$script:ControlDevicePath = '\\.\MicLinkControl'
$script:Results = New-Object System.Collections.Generic.List[object]

function Write-Banner {
    param([string]$Text, [string]$Color = 'Cyan')
    $line = '*' * 78
    Write-Host ''
    Write-Host $line -ForegroundColor $Color
    Write-Host "* $Text" -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ''
}
function Write-Step { param([string]$Message) Write-Host "[verify] $Message" -ForegroundColor Cyan }

function Add-Result {
    param(
        [Parameter(Mandatory = $true)][string]$Item,
        [Parameter(Mandatory = $true)][ValidateSet('PASS', 'FAIL', 'MANUAL', 'INFO')][string]$Result,
        [Parameter()][string]$Detail = ''
    )
    $script:Results.Add([PSCustomObject]@{
            Item   = $Item
            Result = $Result
            Detail = $Detail
        })
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TestSigningState {
    try {
        $raw = (& bcdedit.exe /enum '{current}' 2>$null) -join "`n"
    } catch {
        return $false
    }
    return ($raw -match 'testsigning\s+(Yes|是|True)')
}

function Get-MicLinkPnpDevices {
    $all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
    if ($null -eq $all) { return @() }
    return @($all | Where-Object {
            ($_.FriendlyName -like "*$script:DriverFriendlyName*") -or
            ($_.InstanceId -like '*MicLinkVirtualMic*')
        })
}

function Get-MicLinkDriverPackages {
    $raw = & pnputil.exe /enum-drivers 2>&1
    $text = ($raw | Out-String)
    return @($text -split "`r?`n" | Where-Object { $_ -match 'MicLinkVirtualMic' })
}

# ---------------------------------------------------------------------------
# Native 探针：CreateFile + DeviceIoControl
# ---------------------------------------------------------------------------
$script:NativeReady = $false
try {
    if (-not ([System.Management.Automation.PSTypeName]'MicLinkVerifyNative').Type) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public class MicLinkVerifyNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, uint nInBufferSize, byte[] lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint GetLastError();

    public static IntPtr Open(string path) {
        return CreateFileW(path, 0xC0000000u, 3u, IntPtr.Zero, 3u, 0u, IntPtr.Zero);
    }

    public static bool IsInvalid(IntPtr h) {
        return (h == IntPtr.Zero) || (h.ToInt64() == -1L);
    }

    /// <summary>MICLINK_IOCTL_GET_VERSION：返回 4 个 uint32（major/minor/patch/build）。</summary>
    public static int GetVersion(IntPtr h, out uint[] version, out uint win32Error) {
        version = new uint[4];
        win32Error = 0;
        byte[] outBuf = new byte[16];
        uint returned = 0;
        // CTL_CODE(0x22, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
        uint code = (0x22u << 16) | (0u << 14) | (0x900u << 2) | 0u;
        bool ok = DeviceIoControl(h, code, null, 0, outBuf, (uint)outBuf.Length, out returned, IntPtr.Zero);
        if (!ok) {
            win32Error = GetLastError();
            return -1;
        }
        if (returned < 16) { return -2; }
        for (int i = 0; i < 4; i++) { version[i] = BitConverter.ToUInt32(outBuf, i * 4); }
        return 0;
    }
}
'@ -Language CSharp -ErrorAction Stop | Out-Null
    }
    $script:NativeReady = $true
} catch {
    $script:NativeReady = $false
}

# ---------------------------------------------------------------------------
Write-Banner 'MicLink 虚拟麦克风 —— 安装后自动验证' 'Cyan'

# 1) 管理员权限
Write-Step '检查管理员权限…'
$isAdmin = Test-IsAdministrator
Add-Result -Item '管理员权限' -Result $(if ($isAdmin) { 'PASS' } else { 'FAIL' }) -Detail `
    $(if ($isAdmin) { '已提权' } else { '部分检查（pnputil/服务）可能不可用，建议管理员运行' })

# 2) 测试签名状态
Write-Step '检查测试签名状态…'
$tsEnabled = Get-TestSigningState
if ($tsEnabled) {
    Add-Result -Item 'bcdedit testsigning' -Result 'PASS' -Detail '已开启（测试签名模式；桌面会有"测试模式"水印）'
} else {
    if ($ExpectTestSigning) {
        Add-Result -Item 'bcdedit testsigning' -Result 'FAIL' -Detail '未开启：测试签名驱动会被拒绝加载（代码 52）。执行 bcdedit /set testsigning on 并重启'
    } else {
        Add-Result -Item 'bcdedit testsigning' -Result 'PASS' -Detail '未开启（生产签名环境预期如此）'
    }
}

# 3) 驱动服务
Write-Step '检查驱动服务…'
$service = Get-Service -Name $script:DriverServiceName -ErrorAction SilentlyContinue
if ($null -ne $service) {
    Add-Result -Item "驱动服务 $script:DriverServiceName" -Result 'PASS' -Detail "存在，状态=$($service.Status)，启动类型=$($service.StartType)"
} else {
    Add-Result -Item "驱动服务 $script:DriverServiceName" -Result 'FAIL' -Detail '未找到服务：驱动未安装或未成功加载'
}

# 4) INF 注册
Write-Step '检查 Driver Store 中的 INF…'
$pkgLines = Get-MicLinkDriverPackages
if ($pkgLines.Count -gt 0) {
    Add-Result -Item 'INF 已注册到 Driver Store' -Result 'PASS' -Detail "匹配 $($pkgLines.Count) 行：$(($pkgLines[0]).Trim())"
} else {
    Add-Result -Item 'INF 已注册到 Driver Store' -Result 'FAIL' -Detail 'pnputil /enum-drivers 中没有 MicLinkVirtualMic，需重新安装'
}

# 5) / 6) PnP 设备与问题码
Write-Step '检查 PnP 设备与问题码…'
$devices = Get-MicLinkPnpDevices
if ($devices.Count -eq 0) {
    Add-Result -Item 'PnP 设备存在' -Result 'FAIL' -Detail "未找到名称包含 '$script:DriverFriendlyName' 的设备"
    Add-Result -Item '设备无问题码（无代码 52）' -Result 'FAIL' -Detail '设备不存在，无法判定'
} else {
    $d = $devices[0]
    $statusOk = ($d.Status -eq 'OK')
    Add-Result -Item 'PnP 设备存在' -Result $(if ($statusOk) { 'PASS' } else { 'FAIL' }) -Detail `
        "$($d.FriendlyName) / $($d.InstanceId) / 状态=$($d.Status)"

    $problemProp = $d.PSObject.Properties['Problem']
    $problem = if ($null -ne $problemProp) { $problemProp.Value } else { $null }
    if ($null -eq $problem -or $problem -eq 0) {
        Add-Result -Item '设备无问题码（无代码 52）' -Result 'PASS' -Detail 'ConfigManager 问题码为 0'
    } elseif ($problem -eq 52) {
        Add-Result -Item '设备无问题码（无代码 52）' -Result 'FAIL' -Detail '代码 52：签名未通过。确认 testsigning 已开启并重启，或驱动未正确签名'
    } else {
        Add-Result -Item '设备无问题码（无代码 52）' -Result 'FAIL' -Detail "问题码 $problem（见设备管理器设备状态说明）"
    }
}

# 7) WMI 声音设备
Write-Step '检查 WMI Win32_SoundDevice…'
$soundDevices = @(Get-CimInstance -ClassName Win32_SoundDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*MicLink*' -or $_.Caption -like '*MicLink*' })
if ($soundDevices.Count -gt 0) {
    Add-Result -Item 'WMI Win32_SoundDevice 出现该设备' -Result 'PASS' -Detail `
        "$($soundDevices[0].Name)（状态=$($soundDevices[0].Status)）"
} else {
    Add-Result -Item 'WMI Win32_SoundDevice 出现该设备' -Result 'FAIL' -Detail '未找到名称含 MicLink 的声音设备'
}

# 8) Core Audio 捕获端点（注册表）
Write-Step '检查 Core Audio 捕获端点（MMDevices 注册表）…'
$mmRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$endpointFound = $false
$endpointDetail = '未找到对应捕获端点'
if (Test-Path -Path $mmRoot) {
    $nameKey = '{a45c254e-df1c-4efd-8020-67d146a850e0}, 31'   # DEVPKEY_Device_FriendlyName
    $descKey = '{a45c254e-df1c-4efd-8020-67d146a850e0}, 2'    # DEVPKEY_Device_DeviceDesc
    foreach ($sub in @(Get-ChildItem -Path $mmRoot -ErrorAction SilentlyContinue)) {
        $propPath = Join-Path $sub.PSPath 'Properties'
        if (-not (Test-Path -Path $propPath)) { continue }
        $fn = Get-ItemPropertyValue -Path $propPath -Name $nameKey -ErrorAction SilentlyContinue
        if ([string]::IsNullOrWhiteSpace($fn)) {
            $fn = Get-ItemPropertyValue -Path $propPath -Name $descKey -ErrorAction SilentlyContinue
        }
        if (-not [string]::IsNullOrWhiteSpace($fn) -and $fn -like '*MicLink*') {
            $endpointFound = $true
            $endpointDetail = "$fn（端点 GUID：$(Split-Path -Leaf $sub.PSPath)）"
            break
        }
    }
}
Add-Result -Item 'Core Audio 捕获端点出现该设备' -Result $(if ($endpointFound) { 'PASS' } else { 'FAIL' }) -Detail $endpointDetail

# 9) 控制设备 CreateFile
Write-Step '检查控制设备 \\.\MicLinkControl…'
$handle = [IntPtr]::Zero
$opened = $false
$openDetail = ''
if ($script:NativeReady) {
    $handle = [MicLinkVerifyNative]::Open($script:ControlDevicePath)
    if ([MicLinkVerifyNative]::IsInvalid($handle)) {
        $opened = $false
        $err = [MicLinkVerifyNative]::GetLastError()
        $openDetail = "CreateFile 失败，GetLastError=$err（5=拒绝访问/权限，2=找不到设备）"
    } else {
        $opened = $true
        $openDetail = 'CreateFile 成功'
    }
} else {
    $openDetail = '无法加载 native 探针（Add-Type 失败），未执行'
}
Add-Result -Item '控制设备 \\.\MicLinkControl 可打开' -Result $(if ($opened) { 'PASS' } else { 'FAIL' }) -Detail $openDetail

# 10) IOCTL GET_VERSION
Write-Step '检查 IOCTL GET_VERSION…'
if ($opened -and $script:NativeReady) {
    $version = New-Object 'System.UInt32[]' 4
    $win32Error = 0u
    $rc = [MicLinkVerifyNative]::GetVersion($handle, [ref]$version, [ref]$win32Error)
    if ($rc -eq 0) {
        Add-Result -Item 'IOCTL GET_VERSION 返回版本号' -Result 'PASS' -Detail `
            "v$($version[0]).$($version[1]).$($version[2]).$($version[3])"
    } elseif ($rc -eq -1) {
        Add-Result -Item 'IOCTL GET_VERSION 返回版本号' -Result 'FAIL' -Detail "DeviceIoControl 失败，Win32Error=$win32Error"
    } else {
        Add-Result -Item 'IOCTL GET_VERSION 返回版本号' -Result 'FAIL' -Detail '返回字节数不足 16（驱动与服务端 IOCTL 契约不一致？）'
    }
} else {
    Add-Result -Item 'IOCTL GET_VERSION 返回版本号' -Result 'FAIL' -Detail '控制设备未打开，跳过'
}
if ($opened -and $script:NativeReady) {
    try { [void][MicLinkVerifyNative]::CloseHandle($handle) } catch { /* ignore */ }
}

# M) 人工验证项
Add-Result -Item 'Windows 录音机能采到 MicLink 麦克风' -Result 'MANUAL' -Detail '见 docs\Windows-Manual-Verification.md 第 5 节'
Add-Result -Item 'OBS 能添加 MicLink 麦克风并看到电平' -Result 'MANUAL' -Detail '见 docs\Windows-Manual-Verification.md 第 6 节'
Add-Result -Item 'Zoom 能选择 MicLink 麦克风并被对方听到' -Result 'MANUAL' -Detail '见 docs\Windows-Manual-Verification.md 第 7 节'
Add-Result -Item '24 小时稳定性（无掉设备/无爆音）' -Result 'MANUAL' -Detail '长跑观察，见 docs\VERIFICATION.md B 类清单'

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host ('{0,-46} {1,-7} {2}' -f '检查项', '结果', '说明') -ForegroundColor Cyan
Write-Host ('-' * 110) -ForegroundColor Gray
foreach ($r in $script:Results) {
    $color = switch ($r.Result) {
        'PASS' { 'Green'; break }
        'FAIL' { 'Red'; break }
        'MANUAL' { 'Yellow'; break }
        default { 'Gray' }
    }
    Write-Host ('{0,-46} {1,-7} {2}' -f $r.Item, $r.Result, $r.Detail) -ForegroundColor $color
}

$passCount = @($script:Results | Where-Object { $_.Result -eq 'PASS' }).Count
$failItems = @($script:Results | Where-Object { $_.Result -eq 'FAIL' })
$manualCount = @($script:Results | Where-Object { $_.Result -eq 'MANUAL' }).Count

Write-Host ''
Write-Host "PASS=$passCount  FAIL=$($failItems.Count)  MANUAL（需人工）=$manualCount" -ForegroundColor Cyan

if ($failItems.Count -gt 0) {
    Write-Host ''
    Write-Host '失败项排查指引：' -ForegroundColor Yellow
    foreach ($f in $failItems) {
        Write-Host "  * $($f.Item)：$($f.Detail)" -ForegroundColor Yellow
    }
    Write-Host ''
    Write-Host '  【典型坑】若 \\.\MicLinkControl 能打开，但声音输入列表里看不到麦克风：' -ForegroundColor Yellow
    Write-Host '    多半是驱动的 IRP_MJ_* dispatch 例程覆盖了 KS 过滤器所需的处理路径，' -ForegroundColor Yellow
    Write-Host '    或端点/拓扑 miniport 未正确暴露捕获引脚（capture pin）。' -ForegroundColor Yellow
    Write-Host '    排查：check driver/Source/Main/miclinkctl.cpp 与 adapter.cpp 的 dispatch 设置、' -ForegroundColor Yellow
    Write-Host '    micarraytopo.cpp / minwavertstream.cpp 的引脚描述符与数据格式范围。' -ForegroundColor Yellow
    Write-Host '    详见 docs\Windows-Manual-Verification.md 第 8 节"反面检查"。' -ForegroundColor Yellow
    Write-Banner '存在 FAIL 项，请先修复后再做人工验证' 'Red'
    exit 1
}

Write-Banner "自动检查全部通过；仍需人工完成 $manualCount 项真实应用验证" 'Green'
Write-Host '下一步：按 docs\Windows-Manual-Verification.md 用录音机 / OBS / Zoom 逐条验证。' -ForegroundColor Cyan
exit 0
