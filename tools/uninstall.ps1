<#
.SYNOPSIS
    卸载 Womic 虚拟麦克风设备与驱动包，并检查残留。

.DESCRIPTION
    1) 移除设备实例（pnputil /remove-device）
    2) 删除驱动包（pnputil /delete-driver，必要时 /force）
    3) 残留检查：PnP 设备、驱动服务、注册表服务项、DriverStore 目录、
       声音设备列表、控制设备 \\.\WomicControl 是否还能打开

    输出 PASS / FAIL 清单。

.PARAMETER Force
    删除驱动包时带 /force（可能失败于仍有设备引用时）。非交互运行需显式指定。

.PARAMETER KeepDriverPackage
    只移除设备，保留驱动包在 Driver Store（便于快速重装）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1 -Force
#>
#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [switch]$Force,

    [Parameter()]
    [switch]$KeepDriverPackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:DriverFriendlyName = 'Womic Virtual Microphone'
$script:DriverServiceName = 'WomicVirtualMic'
$script:ControlDevicePath = '\\.\WomicControl'

function Write-Banner {
    param([string]$Text, [string]$Color = 'Yellow')
    $line = '*' * 78
    Write-Host ''
    Write-Host $line -ForegroundColor $Color
    Write-Host "* $Text" -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ''
}

function Write-Step { param([string]$Message) Write-Host "[uninstall] $Message" -ForegroundColor Cyan }
function Write-Ok { param([string]$Message) Write-Host "[  OK  ] $Message" -ForegroundColor Green }
function Write-Warn2 { param([string]$Message) Write-Host "[ WARN ] $Message" -ForegroundColor Yellow }
function Write-Fail { param([string]$Message) Write-Host "[ FAIL ] $Message" -ForegroundColor Red }

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-WomicPnpDevices {
    $all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
    if ($null -eq $all) { return @() }
    return @($all | Where-Object {
            ($_.FriendlyName -like "*$script:DriverFriendlyName*") -or
            ($_.InstanceId -like '*WomicVirtualMic*')
        })
}

function Get-WomicDriverPackages {
    <#
    .OUTPUTS
    PSCustomObject 数组：PublishedName / OriginalName / Provider / Class
    #>
    $raw = & pnputil.exe /enum-drivers 2>&1
    $text = ($raw | Out-String)
    $packages = New-Object System.Collections.Generic.List[object]

    $current = $null
    foreach ($line in ($text -split "`r?`n")) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^Published Name\s*:\s*(.+)$') {
            if ($null -ne $current) { $packages.Add($current) }
            $current = [PSCustomObject]@{
                PublishedName = $Matches[1].Trim()
                OriginalName  = ''
                ProviderName  = ''
                ClassName     = ''
                DriverVersion = ''
            }
            continue
        }
        if ($null -eq $current) { continue }
        if ($trimmed -match '^Original Name\s*:\s*(.+)$') { $current.OriginalName = $Matches[1].Trim() }
        elseif ($trimmed -match '^Provider Name\s*:\s*(.+)$') { $current.ProviderName = $Matches[1].Trim() }
        elseif ($trimmed -match '^Class Name\s*:\s*(.+)$') { $current.ClassName = $Matches[1].Trim() }
        elseif ($trimmed -match '^Driver Version\s*:\s*(.+)$') { $current.DriverVersion = $Matches[1].Trim() }
    }
    if ($null -ne $current) { $packages.Add($current) }

    return @($packages | Where-Object {
            ($_.OriginalName -like '*WomicVirtualMic*') -or
            ($_.PublishedName -like '*womicvirtualmic*') -or
            ($_.ProviderName -like '*Womic*') -or
            ($_.OriginalName -like '*Womic*')
        })
}

function Test-ControlDeviceOpen {
    $source = @'
using System;
using System.Runtime.InteropServices;
public class WomicCtlProbe {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr hObject);

    public static bool TryOpen(string path) {
        IntPtr h = CreateFileW(path, 0xC0000000u, 3u, IntPtr.Zero, 3u, 0u, IntPtr.Zero);
        if (h == IntPtr.Zero || h == new IntPtr(-1)) { return false; }
        CloseHandle(h);
        return true;
    }
}
'@
    try {
        if (-not ([System.Management.Automation.PSTypeName]'WomicCtlProbe').Type) {
            Add-Type -TypeDefinition $source -Language CSharp -ErrorAction Stop | Out-Null
        }
        return [WomicCtlProbe]::TryOpen($script:ControlDevicePath)
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
Write-Banner '卸载 Womic 虚拟麦克风' 'Yellow'

Write-Step '检查管理员权限…'
if (-not (Test-IsAdministrator)) {
    Write-Fail '当前不是管理员权限，请以管理员身份运行 PowerShell 后重试。'
    exit 2
}
Write-Ok '管理员权限 OK'

# ---------------------------------------------------------------------------
# 1. 移除设备实例
# ---------------------------------------------------------------------------
Write-Step '查找设备实例…'
$devices = Get-WomicPnpDevices
if ($devices.Count -eq 0) {
    Write-Warn2 '未找到名为 "Womic Virtual Microphone" 的设备实例（可能已被移除或未安装）。'
} else {
    foreach ($d in $devices) {
        Write-Host "       移除设备：$($d.InstanceId)" -ForegroundColor Gray
        $out = & pnputil.exe /remove-device "$($d.InstanceId)" 2>&1
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Write-Ok "设备已移除：$($d.InstanceId)"
        } else {
            Write-Warn2 "移除设备失败（exit=$code）：$($d.InstanceId)"
            Write-Host "         $(($out | Out-String).Trim())" -ForegroundColor Gray
            # 部分系统版本需要 /subtree 或重启后生效
            $out2 = & pnputil.exe /remove-device "$($d.InstanceId)" /subtree 2>&1
            if ($LASTEXITCODE -eq 0) {
                Write-Ok "已用 /subtree 移除：$($d.InstanceId)"
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 2. 删除驱动包
# ---------------------------------------------------------------------------
if ($KeepDriverPackage) {
    Write-Warn2 '-KeepDriverPackage 已指定，保留驱动包在 Driver Store。'
} else {
    Write-Step '查找驱动包…'
    $packages = Get-WomicDriverPackages
    if ($packages.Count -eq 0) {
        Write-Warn2 '未在 Driver Store 中找到 WomicVirtualMic 驱动包。'
    } else {
        foreach ($p in $packages) {
            Write-Host "       删除驱动包：$($p.PublishedName)（$($p.OriginalName)）" -ForegroundColor Gray
            $out = & pnputil.exe /delete-driver "$($p.PublishedName)" /uninstall 2>&1
            $code = $LASTEXITCODE
            if ($code -ne 0 -and $Force) {
                $out2 = & pnputil.exe /delete-driver "$($p.PublishedName)" /uninstall /force 2>&1
                $code = $LASTEXITCODE
                $out = $out2
            }
            if ($code -eq 0) {
                Write-Ok "驱动包已删除：$($p.PublishedName)"
            } else {
                Write-Fail "删除驱动包失败（exit=$code）：$($p.PublishedName)"
                Write-Host "         $(($out | Out-String).Trim())" -ForegroundColor Gray
                Write-Host '         若提示"仍有设备引用"，请重启后再运行本脚本（或加 -Force）。' -ForegroundColor Yellow
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 3. 残留检查
# ---------------------------------------------------------------------------
Write-Step '残留检查…'
$results = New-Object System.Collections.Generic.List[object]
function Add-Result {
    param([string]$Name, [bool]$Passed, [string]$Detail)
    $results.Add([PSCustomObject]@{ Item = $Name; Result = if ($Passed) { 'PASS' } else { 'FAIL' }; Detail = $Detail })
}

$leftDevices = Get-WomicPnpDevices
Add-Result 'PnP 设备已移除' ($leftDevices.Count -eq 0) "剩余 $($leftDevices.Count) 个"

$leftPackages = Get-WomicDriverPackages
Add-Result 'Driver Store 驱动包已删除' ($leftPackages.Count -eq 0) "剩余 $($leftPackages.Count) 个"

$service = Get-Service -Name $script:DriverServiceName -ErrorAction SilentlyContinue
Add-Result '驱动服务已不存在' ($null -eq $service) `
    $(if ($null -eq $service) { '未找到 WomicVirtualMic 服务' } else { "仍存在：状态=$($service.Status)" })

$regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$script:DriverServiceName"
$regExists = Test-Path -Path $regPath
Add-Result '注册表服务项已清理' (-not $regExists) `
    $(if ($regExists) { "仍存在：$regPath（通常重启后由系统清理）" } else { '不存在' })

$storeDirs = @(Get-ChildItem -Path "$env:SystemRoot\System32\DriverStore\FileRepository" `
        -Directory -Filter 'womicvirtualmic*' -ErrorAction SilentlyContinue)
Add-Result 'DriverStore 文件目录已清理' ($storeDirs.Count -eq 0) "剩余 $($storeDirs.Count) 个"

$soundDevices = @(Get-CimInstance -ClassName Win32_SoundDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*Womic*' })
Add-Result '声音设备列表无残留' ($soundDevices.Count -eq 0) "剩余 $($soundDevices.Count) 个"

$ctlOpen = Test-ControlDeviceOpen
Add-Result '控制设备 \\.\WomicControl 已不可打开' (-not $ctlOpen) `
    $(if ($ctlOpen) { '仍可打开（驱动仍在运行，可能需要重启）' } else { '无法打开（符合预期）' })

Write-Host ''
Write-Host ('{0,-42} {1,-6} {2}' -f '检查项', '结果', '说明') -ForegroundColor Cyan
Write-Host ('-' * 100) -ForegroundColor Gray
foreach ($r in $results) {
    $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host ('{0,-42} {1,-6} {2}' -f $r.Item, $r.Result, $r.Detail) -ForegroundColor $color
}

$failCount = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ''
if ($failCount -eq 0) {
    Write-Banner '卸载完成且无残留' 'Green'
    exit 0
} else {
    Write-Banner "卸载完成，但有 $failCount 项残留（多为重启后自动清理，请重启后再跑一次本脚本确认）" 'Yellow'
    exit 1
}
