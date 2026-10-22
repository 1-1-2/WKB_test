param([string]$Mode = "dump", [string]$Host_ = "127.0.0.1")
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$py = "C:\hostedtoolcache\windows\Python\3.12.10\x64\python.exe"

if ($Mode -eq "dump") {
    Stop-Process -Name WOMicClient -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Start-Process "D:\a\_temp\extract\WOMicClient.exe" -WorkingDirectory "D:\a\_temp\extract"
    Start-Sleep 4
} elseif ($Mode -eq "attach") {
    # 客户端已启动，仅转储
}

$procs = Get-Process WOMicClient -ErrorAction SilentlyContinue
if (-not $procs) { Write-Output "NO_CLIENT_PROCESS"; exit 1 }
$cpid = ($procs | Select-Object -First 1).Id
Write-Output "CLIENT_PID=$cpid"

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $cpid)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
Write-Output "WINDOW_COUNT=$($wins.Count)"

function Dump-Tree($el, $depth, $visitDialogs) {
    if ($depth -gt 8) { return }
    $c = $el.Current
    $pad = "  " * $depth
    Write-Output ("{0}[{1}] name='{2}' class='{3}' autoId='{4}'" -f `
        $pad, $c.ControlType.ProgrammaticName, $c.Name, $c.ClassName, $c.AutomationId)
    $kids = $el.FindAll([System.Windows.Automation.TreeScope]::Children,
                         [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($k in $kids) { Dump-Tree $k ($depth + 1) $visitDialogs }
}

foreach ($w in $wins) {
    Write-Output "===== WINDOW ====="
    Dump-Tree $w 0 $true
}
Write-Output "DUMP_DONE"
