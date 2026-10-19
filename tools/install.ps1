<#
.SYNOPSIS
    安装 MicLink 虚拟麦克风驱动（测试签名 / 生产签名两种模式）。

.DESCRIPTION
    TestSign（默认）   : 本机开发自测。会检查并提示开启 bcdedit /set testsigning on
                         （需要重启，桌面会出现"测试模式"水印）。
    Production         : 只接受**已经正式签名**的 .cat / .sys；发现未签名或测试签名
                         立即报错退出，**绝不悄悄降级到测试签名模式**。

    两种模式都会在屏幕最上方打印醒目的模式横幅。

.PARAMETER Mode
    TestSign（默认）或 Production。

.PARAMETER InfPath
    驱动 INF 路径。默认在仓库常见输出目录下搜索 MicLinkVirtualMic.inf。

.PARAMETER Force
    跳过交互确认，并在需要时直接执行 bcdedit /set testsigning on。
    非交互运行（CI）时必须显式指定。

.PARAMETER TimeoutSeconds
    等待设备枚举的超时时间（默认 90 秒）。

.PARAMETER NoWait
    安装后不等待设备枚举。

.PARAMETER AllowNoDevice
    设备未枚举到时仍以 0 退出（保留旧的宽松行为，供"装完就重启、稍后再验"的
    场景使用）。默认**不**指定：未枚举到设备即以退出码 7 判定安装失败，
    便于 CI / 批处理脚本据此分支。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode Production -InfPath C:\build\MicLinkVirtualMic.inf

.NOTES
    必须以管理员权限运行。测试签名驱动**仅限本机开发自测**，不得分发给普通用户。
#>
#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('TestSign', 'Production')]
    [string]$Mode = 'TestSign',

    [Parameter()]
    [string]$InfPath = '',

    [Parameter()]
    [switch]$Force,

    [Parameter()]
    [int]$TimeoutSeconds = 90,

    [Parameter()]
    [switch]$NoWait,

    [Parameter()]
    [switch]$AllowNoDevice
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# 常量与工具函数（脚本作用域，避免嵌套作用域取不到值）
# ---------------------------------------------------------------------------
$script:DriverFriendlyName = 'MicLink Virtual Microphone'
$script:DriverServiceName = 'MicLinkVirtualMic'
$script:IsInteractive = [Environment]::UserInteractive -and -not ([Console]::IsInputRedirected)

function Write-Banner {
    param([string]$Text, [string]$Color = 'Yellow')
    $line = '*' * 78
    Write-Host ''
    Write-Host $line -ForegroundColor $Color
    Write-Host "* $Text" -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ''
}

function Write-Step {
    param([string]$Message)
    Write-Host "[install] $Message" -ForegroundColor Cyan
}

function Write-Ok {
    param([string]$Message)
    Write-Host "[  OK  ] $Message" -ForegroundColor Green
}

function Write-Warn2 {
    param([string]$Message)
    Write-Host "[ WARN ] $Message" -ForegroundColor Yellow
}

function Write-Fail {
    param([string]$Message)
    Write-Host "[ FAIL ] $Message" -ForegroundColor Red
}

function Confirm-Or-Stop {
    param([string]$Question)
    if ($Force) {
        Write-Warn2 "-Force 已指定，跳过确认：$Question"
        return $true
    }
    if (-not $script:IsInteractive) {
        Write-Fail "非交互环境且未指定 -Force，无法确认：$Question"
        return $false
    }
    $answer = Read-Host "$Question [y/N]"
    return ($answer -match '^(y|Y|yes|YES)$')
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TestSigningState {
    <#
    .OUTPUTS
    PSCustomObject { Enabled: bool, Raw: string }
    #>
    try {
        $raw = (& bcdedit.exe /enum '{current}' 2>$null) -join "`n"
    } catch {
        return [PSCustomObject]@{ Enabled = $false; Raw = '' }
    }
    $enabled = $false
    if ($raw -match 'testsigning\s+(Yes|是|True)') {
        $enabled = $true
    }
    return [PSCustomObject]@{ Enabled = $enabled; Raw = $raw }
}

function Resolve-MicLinkInf {
    param([string]$ExplicitPath)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "指定的 INF 不存在：$ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $repoRoot 'driver\x64\Debug\MicLinkVirtualMic.inf'),
        (Join-Path $repoRoot 'driver\x64\Release\MicLinkVirtualMic.inf'),
        (Join-Path $repoRoot 'driver\Source\Main\x64\Debug\MicLinkVirtualMic.inf'),
        (Join-Path $repoRoot 'driver\build\MicLinkVirtualMic.inf'),
        (Join-Path $repoRoot 'build\driver\MicLinkVirtualMic.inf')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Leaf) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }

    # 兜底：全仓库搜索
    $found = Get-ChildItem -Path $repoRoot -Filter 'MicLinkVirtualMic.inf' -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $found) {
        return $found.FullName
    }
    throw @"
未找到 MicLinkVirtualMic.inf。
请先用 Visual Studio + WDK 编译驱动（见 docs/BUILD.md），再用 -InfPath 显式指定，例如：

  powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -InfPath C:\path\to\MicLinkVirtualMic.inf
"@
}

function Get-DriverBinariesFromInf {
    param([string]$InfFullPath)

    $infDir = Split-Path -Parent $InfFullPath
    $sys = Join-Path $infDir 'miclinkvirtualmic.sys'
    $cat = Join-Path $infDir 'MicLinkVirtualMic.cat'
    return [PSCustomObject]@{
        Inf      = $InfFullPath
        Sys      = $sys
        Cat      = $cat
        SysFound = (Test-Path -LiteralPath $sys -PathType Leaf)
        CatFound = (Test-Path -LiteralPath $cat -PathType Leaf)
    }
}

function Get-SignatureReport {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [PSCustomObject]@{
            Exists = $false; Status = 'Missing'; Signer = ''; ChainOk = $false
        }
    }
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    $chainOk = $false
    if ($null -ne $sig.SignerCertificate) {
        try {
            $chainOk = $sig.SignerCertificate.Verify()
        } catch {
            $chainOk = $false
        }
    }
    return [PSCustomObject]@{
        Exists  = $true
        Status  = $sig.Status.ToString()
        Signer  = if ($null -ne $sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
        ChainOk = $chainOk
    }
}

# ---------------------------------------------------------------------------
# 0. 模式横幅（必须醒目）
# ---------------------------------------------------------------------------
if ($Mode -eq 'TestSign') {
    Write-Banner "模式：TestSign（测试签名）—— 仅限本机开发自测" 'Yellow'
    Write-Host @'
  本模式会用自签证书签名的驱动，并要求开启 Windows 测试签名：

    * 执行 bcdedit /set testsigning on 后**必须重启**才能生效；
    * 重启后桌面右下角会出现"测试模式"水印，这是正常现象；
    * 测试签名驱动**不能**分发给普通用户：在未开启测试模式的机器上会加载失败
      （设备管理器代码 52），并且要求用户关闭系统安全保护本身就是不可接受的做法。
    * 真正的生产分发请走 EV 代码签名 + WHQL / Attestation 签名，见 docs/Signing-Guide.md。
'@ -ForegroundColor Yellow
} else {
    Write-Banner "模式：Production（生产签名）—— 只接受已正式签名的文件" 'Cyan'
    Write-Host @'
  本模式**不会**碰 bcdedit，也**不会**降级为测试签名：
    * .sys 与 .cat 必须都存在，且 Authenticode 状态必须为 Valid；
    * 签名链必须可验证到受信任根（EV / WHQL / Attestation 签名）；
    * 任何一项不满足都会立即报错退出，不会"先装上再说"。
'@ -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# 1. 管理员权限
# ---------------------------------------------------------------------------
Write-Step '检查管理员权限…'
if (-not (Test-IsAdministrator)) {
    Write-Fail '当前不是管理员权限。请以"管理员身份"启动 PowerShell 后重试。'
    exit 2
}
Write-Ok '管理员权限 OK'

# ---------------------------------------------------------------------------
# 2. 定位 INF 与二进制
# ---------------------------------------------------------------------------
Write-Step '定位驱动 INF…'
try {
    $inf = Resolve-MicLinkInf -ExplicitPath $InfPath
} catch {
    Write-Fail $_.Exception.Message
    exit 3
}
$bin = Get-DriverBinariesFromInf -InfFullPath $inf
Write-Ok "INF: $($bin.Inf)"
Write-Host "       sys: $($bin.Sys)（存在=$($bin.SysFound)）" -ForegroundColor Gray
Write-Host "       cat: $($bin.Cat)（存在=$($bin.CatFound)）" -ForegroundColor Gray

# ---------------------------------------------------------------------------
# 3. 模式专属前置检查
# ---------------------------------------------------------------------------
if ($Mode -eq 'TestSign') {
    Write-Step '检查测试签名状态…'
    $ts = Get-TestSigningState
    if ($ts.Enabled) {
        Write-Ok 'bcdedit testsigning 已开启'
    } else {
        Write-Warn2 'bcdedit testsigning 未开启，驱动将被拒绝加载（设备管理器代码 52）。'
        Write-Host '         需要执行：bcdedit /set testsigning on  然后**重启电脑**' -ForegroundColor Yellow
        if (Confirm-Or-Stop '是否现在执行 bcdedit /set testsigning on（随后需手动重启）？') {
            & bcdedit.exe /set testsigning on
            if ($LASTEXITCODE -eq 0) {
                Write-Ok '已设置 testsigning on。**请重启电脑**后再继续安装。'
                Write-Host '  提示：重启后桌面右下角会出现"测试模式"水印，属正常现象。' -ForegroundColor Yellow
                if (-not $Force -and $script:IsInteractive) {
                    $go = Read-Host '是否仍要继续安装（未重启前安装通常会失败）？[y/N]'
                    if ($go -notmatch '^(y|Y|yes|YES)$') {
                        Write-Host '已中止。请重启后重新运行本脚本。' -ForegroundColor Yellow
                        exit 0
                    }
                }
            } else {
                Write-Fail "bcdedit 执行失败（exit=$LASTEXITCODE）：可能是没有管理员权限或系统策略限制。"
                exit 4
            }
        } else {
            Write-Host '已中止：未开启测试签名。' -ForegroundColor Yellow
            exit 0
        }
    }

    # Secure Boot 与测试签名互斥，提前提示
    try {
        $secureBoot = Confirm-SecureBootUEFI -ErrorAction Stop
        if ($secureBoot) {
            Write-Warn2 '检测到 Secure Boot 处于开启状态：测试签名驱动在 Secure Boot 开启时无法加载，'
            Write-Host '         需要在 UEFI 固件设置里临时关闭 Secure Boot（仅本机开发自测场景）。' -ForegroundColor Yellow
        } else {
            Write-Ok 'Secure Boot 已关闭'
        }
    } catch {
        Write-Host '       （无法查询 Secure Boot 状态：非 UEFI 或不支持，忽略）' -ForegroundColor Gray
    }

    # 提示签名情况（不阻塞）
    if ($bin.CatFound) {
        $catSig = Get-SignatureReport -Path $bin.Cat
        Write-Host "       cat 签名状态：$($catSig.Status)（签名者：$($catSig.Signer)）" -ForegroundColor Gray
        if ($catSig.Status -ne 'Valid') {
            Write-Warn2 'cat 未签名或签名无效：测试模式下仍可安装（前提是已开启 testsigning），生产模式会被拒绝。'
        }
    } else {
        Write-Warn2 '未找到 MicLinkVirtualMic.cat：测试模式下可安装，生产模式必须提供已签名的 cat。'
        Write-Host '         可用 tools\make-test-cert.ps1 生成测试证书并签名。' -ForegroundColor Gray
    }
} else {
    Write-Step '校验生产签名（不允许降级）…'
    $problems = New-Object System.Collections.Generic.List[string]

    if (-not $bin.SysFound) { $problems.Add("缺少 $($bin.Sys)") }
    if (-not $bin.CatFound) { $problems.Add("缺少 $($bin.Cat)（生产分发必须提供已签名的目录文件）") }

    if ($bin.SysFound) {
        $sysSig = Get-SignatureReport -Path $bin.Sys
        Write-Host "       sys 签名：$($sysSig.Status) / 链验证=$($sysSig.ChainOk) / $($sysSig.Signer)" -ForegroundColor Gray
        if ($sysSig.Status -ne 'Valid') { $problems.Add("miclinkvirtualmic.sys 签名无效（$($sysSig.Status)）") }
        if (-not $sysSig.ChainOk) { $problems.Add('miclinkvirtualmic.sys 签名链无法验证到受信任根（测试证书不被接受）') }
    }
    if ($bin.CatFound) {
        $catSig = Get-SignatureReport -Path $bin.Cat
        Write-Host "       cat 签名：$($catSig.Status) / 链验证=$($catSig.ChainOk) / $($catSig.Signer)" -ForegroundColor Gray
        if ($catSig.Status -ne 'Valid') { $problems.Add("MicLinkVirtualMic.cat 签名无效（$($catSig.Status)）") }
        if (-not $catSig.ChainOk) { $problems.Add('MicLinkVirtualMic.cat 签名链无法验证到受信任根（测试证书不被接受）') }
    }

    if ($problems.Count -gt 0) {
        Write-Banner 'Production 模式校验失败：发现未签名 / 无效签名，已中止（未做任何安装）' 'Red'
        foreach ($p in $problems) { Write-Fail $p }
        Write-Host ''
        Write-Host '  说明：请先用 EV 代码签名证书签名并通过 WHQL / Attestation 认证，' -ForegroundColor Yellow
        Write-Host '        详见 docs/Signing-Guide.md。本脚本不会降级为测试签名。' -ForegroundColor Yellow
        exit 5
    }
    Write-Ok '生产签名校验通过'

    $ts = Get-TestSigningState
    if ($ts.Enabled) {
        Write-Warn2 '当前系统开启了 testsigning：生产环境应保持关闭（bcdedit /set testsigning off）。'
    }
}

# ---------------------------------------------------------------------------
# 4. 安装驱动
# ---------------------------------------------------------------------------
Write-Step '安装驱动（pnputil /add-driver）…'
$pnputilOutput = & pnputil.exe /add-driver "$($bin.Inf)" /install 2>&1
$pnputilExit = $LASTEXITCODE
$pnputilText = ($pnputilOutput | Out-String)

if ($pnputilExit -ne 0) {
    Write-Warn2 "pnputil /add-driver 返回 $pnputilExit，尝试旧版语法（pnputil -i -a）…"
    $legacyOutput = & pnputil.exe -i -a "$($bin.Inf)" 2>&1
    $pnputilExit = $LASTEXITCODE
    $pnputilText += "`n" + ($legacyOutput | Out-String)
}

Write-Host $pnputilText.Trim() -ForegroundColor Gray

if ($pnputilExit -ne 0) {
    Write-Banner '驱动安装失败' 'Red'
    Write-Fail "pnputil 退出码 $pnputilExit"
    Write-Host @'
  常见原因：
    * 不是管理员权限；
    * 未开启测试签名（TestSign 模式）或 Secure Boot 未关闭；
    * INF 与 sys 不在同一目录 / sys 未签名；
    * 驱动包缺少 cat（生产模式）。
  排障见 docs/BUILD.md 与 docs/Windows-Manual-Verification.md。
'@ -ForegroundColor Yellow
    exit 6
}
Write-Ok 'pnputil 安装命令执行完成'

# ---------------------------------------------------------------------------
# 5. 等待设备枚举
# ---------------------------------------------------------------------------
$device = $null
$enumFailed = $false      # 超时内一个设备都没枚举到
$deviceProblem = $false   # 枚举到了，但有问题码（如 52 = 签名未通过）
if ($NoWait) {
    Write-Warn2 '-NoWait 已指定，跳过设备枚举等待。'
} else {
    Write-Step "等待设备枚举（最长 $TimeoutSeconds 秒）…"
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Seconds 2
        $device = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
                ($_.FriendlyName -like "*$script:DriverFriendlyName*") -or
                ($_.InstanceId -like '*MicLinkVirtualMic*')
            })
        if ($device.Count -gt 0) { break }
    } while ((Get-Date) -lt $deadline)

    if ($device.Count -gt 0) {
        foreach ($d in $device) {
            Write-Ok "设备已枚举：$($d.FriendlyName) / $($d.InstanceId) / 状态=$($d.Status)"
            # StrictMode 下用 PSObject.Properties 安全取属性（不同系统版本字段可能缺失）
            $problemProp = $d.PSObject.Properties['Problem']
            if ($null -ne $problemProp -and $null -ne $problemProp.Value -and $problemProp.Value -ne 0) {
                $deviceProblem = $true
                Write-Fail "设备有问题码：$($problemProp.Value)（常见：52 = 签名未通过，需检查测试签名）"
            }
        }
    } else {
        $enumFailed = $true
        Write-Warn2 "超时内未枚举到设备（可能仍需重启）。"
        Write-Host '         请用 tools\verify.ps1 复查，或打开设备管理器查看"声音、视频和游戏控制器"。' -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------
# 6. 结果
# ---------------------------------------------------------------------------
Write-Banner "安装流程结束（模式：$Mode）" $(if ($Mode -eq 'TestSign') { 'Yellow' } else { 'Cyan' })

Write-Host '后续步骤：' -ForegroundColor Cyan
Write-Host '  1) 若刚开启/修改了 testsigning，务必重启电脑；' -ForegroundColor Gray
Write-Host '  2) 运行： powershell -ExecutionPolicy Bypass -File tools\verify.ps1' -ForegroundColor Gray
Write-Host '  3) 按 docs\Windows-Manual-Verification.md 逐条人工验证（录音机 / OBS / Zoom）。' -ForegroundColor Gray
if ($Mode -eq 'TestSign') {
    Write-Host ''
    Write-Host '  再次强调：测试签名**仅限本机开发自测**，不得面向普通用户分发。' -ForegroundColor Yellow
}

# 退出码必须能反映安装结果，否则 CI / 批处理无法据此分支（缺陷 D-08）：
#   0  成功（设备已枚举且无问题码，或显式 -AllowNoDevice / -NoWait 放宽）
#   7  超时内未枚举到设备（需 -AllowNoDevice 才能按成功处理）
#   8  设备已枚举但有问题码（例如 52 = 测试签名未通过）
if ($deviceProblem) {
    Write-Fail '设备已枚举但存在问题码，安装判定为失败（退出码 8）。'
    exit 8
}
if ($enumFailed) {
    if ($AllowNoDevice) {
        Write-Warn2 '-AllowNoDevice 已指定：未枚举到设备仍按成功退出（退出码 0）。'
        exit 0
    }
    Write-Fail "未在 $TimeoutSeconds 秒内枚举到设备，安装判定为失败（退出码 7）。"
    exit 7
}
exit 0
