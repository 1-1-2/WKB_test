<#
.SYNOPSIS
    生成**测试用**代码签名证书，并用它对驱动的 .sys / .cat 做测试签名。

.DESCRIPTION
    流程：
      1) New-SelfSignedCertificate 生成自签 CodeSigning 证书；
      2) 导出 PFX（随机口令，打印在屏幕上）；
      3) **导入本机**的 Trusted Root CA 与 Trusted Publisher（测试签名要被接受必须导入）；
      4) signtool sign（SHA256 / RFC3161 时间戳）；
      5) signtool verify + Get-AuthenticodeSignature 复核。

    安全边界（重要）：
      * 生成的证书**仅供本机开发自测**。它让"这台机器"信任"你自己"，
        绝对不能导出到他人的机器，更不能用于任何对外分发；
      * 对外分发必须走 EV 代码签名证书 + WHQL / Attestation 签名，见 docs/Signing-Guide.md。

.PARAMETER Files
    要签名的文件（.sys / .cat）。默认在仓库常见输出目录下搜索。

.PARAMETER CertSubject
    证书主题，默认 CN=MicLink Open Test Signing (DO NOT DISTRIBUTE)。

.PARAMETER PfxPath
    PFX 输出路径，默认 tools\_out\MicLinkTestSigning.pfx。

.PARAMETER SignToolPath
    signtool.exe 路径；不指定时自动在 Windows Kits 目录下搜索。

.PARAMETER TimestampUrl
    RFC3161 时间戳服务器，默认 http://timestamp.digicert.com。

.PARAMETER SkipTimestamp
    离线环境跳过时间戳（签名依然有效，只是没有时间戳背书）。

.PARAMETER DoNotTrust
    只生成证书并签名，不把证书导入本机 Trusted Root / Trusted Publisher。
    （导入后系统才会接受测试签名；不导入则安装时会被拒绝。）

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1 -Files C:\build\miclinkvirtualmic.sys,C:\build\MicLinkVirtualMic.cat
#>
#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [string[]]$Files = @(),

    [Parameter()]
    [string]$CertSubject = 'CN=MicLink Open Test Signing (DO NOT DISTRIBUTE)',

    [Parameter()]
    [string]$PfxPath = '',

    [Parameter()]
    [string]$SignToolPath = '',

    [Parameter()]
    [string]$TimestampUrl = 'http://timestamp.digicert.com',

    [Parameter()]
    [switch]$SkipTimestamp,

    [Parameter()]
    [switch]$DoNotTrust
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Banner {
    param([string]$Text, [string]$Color = 'Yellow')
    $line = '*' * 78
    Write-Host ''
    Write-Host $line -ForegroundColor $Color
    Write-Host "* $Text" -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ''
}
function Write-Step { param([string]$Message) Write-Host "[cert] $Message" -ForegroundColor Cyan }
function Write-Ok { param([string]$Message) Write-Host "[  OK  ] $Message" -ForegroundColor Green }
function Write-Warn2 { param([string]$Message) Write-Host "[ WARN ] $Message" -ForegroundColor Yellow }
function Write-Fail { param([string]$Message) Write-Host "[ FAIL ] $Message" -ForegroundColor Red }

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function New-RandomPassword {
    param([int]$Length = 24)
    $chars = 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
    $bytes = New-Object 'System.Byte[]' $Length
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
        $sb = New-Object System.Text.StringBuilder
        for ($i = 0; $i -lt $Length; $i++) {
            [void]$sb.Append($chars[$bytes[$i] % $chars.Length])
        }
        return $sb.ToString()
    } finally {
        $rng.Dispose()
    }
}

function Find-SignTool {
    param([string]$Explicit)

    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (-not (Test-Path -LiteralPath $Explicit -PathType Leaf)) {
            throw "指定的 signtool 不存在：$Explicit"
        }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    $cmd = Get-Command -Name 'signtool.exe' -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { return $cmd.Source }

    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "$env:ProgramFiles\Windows Kits\10\bin",
        "${env:ProgramFiles(x86)}\Windows Kits\11\bin"
    )
    $best = $null
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $hits = @(Get-ChildItem -Path $root -Filter 'signtool.exe' -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' })
        if ($hits.Count -gt 0) {
            $candidate = ($hits | Sort-Object { $_.FullName } -Descending | Select-Object -First 1).FullName
            if ($null -eq $best) { $best = $candidate }
        }
    }
    if ($null -eq $best) {
        throw @"
未找到 signtool.exe。
请安装 Windows SDK 或 WDK（见 docs/BUILD.md），或用 -SignToolPath 显式指定，例如：

  -SignToolPath "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe"
"@
    }
    return $best
}

function Resolve-TargetFiles {
    param([string[]]$Explicit)

    if ($Explicit.Count -gt 0) {
        $resolved = New-Object System.Collections.Generic.List[string]
        foreach ($f in $Explicit) {
            if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
                throw "要签名的文件不存在：$f"
            }
            $resolved.Add((Resolve-Path -LiteralPath $f).Path)
        }
        return , $resolved.ToArray()
    }

    $repoRoot = Split-Path -Parent $PSScriptRoot
    $names = @('miclinkvirtualmic.sys', 'MicLinkVirtualMic.cat')
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($n in $names) {
        $hit = Get-ChildItem -Path $repoRoot -Filter $n -Recurse -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $hit) { $found.Add($hit.FullName) }
    }
    if ($found.Count -eq 0) {
        throw @"
未找到要签名的 .sys / .cat。
请先编译驱动包（见 docs/BUILD.md），或用 -Files 显式指定：

  -Files C:\build\miclinkvirtualmic.sys,C:\build\MicLinkVirtualMic.cat
"@
    }
    return , $found.ToArray()
}

# ---------------------------------------------------------------------------
Write-Banner '生成测试证书并做测试签名（仅限本机开发自测）' 'Yellow'
Write-Host @'
  本脚本会让**这台机器**信任**你自己签的**测试证书，因此：
    * 只能在你的开发机上运行；
    * 生成的 PFX / 证书**不得**分发给他人；
    * 面向普通用户的分发必须走 EV 代码签名 + WHQL / Attestation（docs/Signing-Guide.md）。
'@ -ForegroundColor Yellow

Write-Step '检查管理员权限…'
if (-not (Test-IsAdministrator)) {
    Write-Fail '需要管理员权限（写 LocalMachine 证书存储）。请以管理员身份运行 PowerShell。'
    exit 2
}
Write-Ok '管理员权限 OK'

Write-Step '定位 signtool…'
try {
    $signtool = Find-SignTool -Explicit $SignToolPath
} catch {
    Write-Fail $_.Exception.Message
    exit 3
}
Write-Ok "signtool: $signtool"

Write-Step '定位待签名文件…'
try {
    $targets = Resolve-TargetFiles -Explicit $Files
} catch {
    Write-Fail $_.Exception.Message
    exit 4
}
foreach ($t in $targets) { Write-Host "       $t" -ForegroundColor Gray }

# ---------------------------------------------------------------------------
# 1. 生成自签证书
# ---------------------------------------------------------------------------
Write-Step '生成自签代码签名证书…'
$notAfter = (Get-Date).AddYears(5)
try {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertSubject `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter $notAfter `
        -FriendlyName 'MicLink Test Signing (DO NOT DISTRIBUTE)' `
        -ErrorAction Stop
} catch {
    Write-Fail "New-SelfSignedCertificate 失败：$($_.Exception.Message)"
    exit 5
}
Write-Ok "证书已生成：Thumbprint=$($cert.Thumbprint)"
Write-Host "       主题：$($cert.Subject)" -ForegroundColor Gray
Write-Host "       有效期至：$($notAfter.ToString('yyyy-MM-dd'))" -ForegroundColor Gray

# ---------------------------------------------------------------------------
# 2. 导出 PFX
# ---------------------------------------------------------------------------
Write-Step '导出 PFX…'
if ([string]::IsNullOrWhiteSpace($PfxPath)) {
    $outDir = Join-Path $PSScriptRoot '_out'
    if (-not (Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }
    $PfxPath = Join-Path $outDir 'MicLinkTestSigning.pfx'
}
$plainPassword = New-RandomPassword
$securePassword = ConvertTo-SecureString -String $plainPassword -AsPlainText -Force
try {
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password $securePassword -Force | Out-Null
} catch {
    Write-Fail "导出 PFX 失败：$($_.Exception.Message)"
    exit 6
}
Write-Ok "PFX 已导出：$PfxPath"
Write-Host "       PFX 口令（仅本次显示，请另行保存）：$plainPassword" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
# 3. 导入本机信任存储（测试签名被接受的前提）
# ---------------------------------------------------------------------------
if ($DoNotTrust) {
    Write-Warn2 '-DoNotTrust 已指定：不导入 Trusted Root / Trusted Publisher，安装时大概率被拒绝。'
} else {
    Write-Step '导入本机 Trusted Root CA 与 Trusted Publisher…'
    foreach ($store in @('Root', 'TrustedPublisher')) {
        try {
            Import-PfxCertificate -FilePath $PfxPath `
                -CertStoreLocation "Cert:\LocalMachine\$store" `
                -Password $securePassword -ErrorAction Stop | Out-Null
            Write-Ok "已导入 Cert:\LocalMachine\$store"
        } catch {
            Write-Fail "导入 Cert:\LocalMachine\$store 失败：$($_.Exception.Message)"
            exit 7
        }
    }
    Write-Warn2 '本机现已信任该测试证书。请勿把此 PFX 带到其它机器，也不要用于任何分发。'
}

# ---------------------------------------------------------------------------
# 4. 签名
# ---------------------------------------------------------------------------
Write-Step '签名…'
foreach ($t in $targets) {
    $args = @('sign', '/f', $PfxPath, '/p', $plainPassword, '/fd', 'SHA256')
    if (-not $SkipTimestamp) {
        $args += @('/tr', $TimestampUrl, '/td', 'SHA256')
    } else {
        Write-Warn2 'SkipTimestamp：跳过时间戳（离线签名）。'
    }
    $args += $t

    & $signtool @args 2>&1 | ForEach-Object { Write-Host "       $_" -ForegroundColor Gray }
    if ($LASTEXITCODE -ne 0) {
        Write-Fail "signtool sign 失败（exit=$LASTEXITCODE）：$t"
        exit 8
    }
    Write-Ok "已签名：$t"
}

# ---------------------------------------------------------------------------
# 5. 复核
# ---------------------------------------------------------------------------
Write-Step '复核签名…'
$allValid = $true
foreach ($t in $targets) {
    & $signtool verify /v /pa $t 2>&1 | ForEach-Object { Write-Host "       $_" -ForegroundColor Gray }
    $sig = Get-AuthenticodeSignature -LiteralPath $t
    if ($sig.Status -eq 'Valid') {
        Write-Ok "$t -> $($sig.Status) / $($sig.SignerCertificate.Subject)"
    } else {
        Write-Fail "$t -> $($sig.Status)"
        $allValid = $false
    }
}

Write-Host ''
if ($allValid) {
    Write-Banner '测试签名完成' 'Green'
    Write-Host '下一步：' -ForegroundColor Cyan
    Write-Host '  1) powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force' -ForegroundColor Gray
    Write-Host '     （会提示开启 bcdedit /set testsigning on，需重启，桌面出现"测试模式"水印）' -ForegroundColor Gray
    Write-Host '  2) 重启后运行 tools\verify.ps1 做自动检查' -ForegroundColor Gray
    exit 0
} else {
    Write-Banner '签名复核未通过，请检查 signtool 输出' 'Red'
    exit 9
}
