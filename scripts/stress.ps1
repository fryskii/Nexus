<#
.SYNOPSIS
  Runs the NEXUS UI stress mode, optionally under a process-churn load, and prints the summary.

.DESCRIPTION
  Starts nexus_churn (if given) to keep many short-lived processes running, waits for it to ramp
  up, then runs "NEXUS.exe --stress <seconds> <csv> <seed>". The stress mode drives the window with
  random but reproducible actions and samples NEXUS's own private bytes, working set, handles,
  GDI/USER objects, threads and CPU time. It never saves settings and suppresses all dialogs and
  menus, so nothing is ended, changed or confirmed. Run it on an otherwise idle machine.

  A growing "last-quarter median" versus "2nd-quarter median" in the summary indicates a leak;
  a flat value after warm-up does not.

.EXAMPLE
  .\scripts\stress.ps1 -Nexus .\build\Release\NEXUS.exe -Churn .\build\Release\nexus_churn.exe -Seconds 1800
#>
param(
    [Parameter(Mandatory = $true)][string]$Nexus,
    [string]$Churn = "",
    [int]$Seconds = 600,
    [int]$Alive = 300,
    [int]$ReplacePerSecond = 10,
    [int]$Seed = 1,
    [string]$Only = "",   # e.g. "navigate,hover" or "idle"; empty = full action mix
    [string]$OutDir = $env:TEMP
)
$ErrorActionPreference = "Stop"
$csv = Join-Path $OutDir ("nexus-stress-{0:yyyyMMdd-HHmmss}.csv" -f (Get-Date))

$churnProc = $null
if ($Churn) {
    $churnProc = Start-Process -FilePath $Churn -ArgumentList $Alive, $ReplacePerSecond, ($Seconds + 120) -PassThru -WindowStyle Hidden
    Write-Host "Churn load: $Alive processes, $ReplacePerSecond replaced per second. Ramping up..."
    Start-Sleep -Seconds 30
}

if ($Only) { $env:NEXUS_STRESS_ONLY = $Only } else { Remove-Item Env:NEXUS_STRESS_ONLY -ErrorAction SilentlyContinue }
Write-Host "Stressing NEXUS for $Seconds s (seed $Seed)..."
$p = Start-Process -FilePath $Nexus -ArgumentList "--stress", $Seconds, "`"$csv`"", $Seed -PassThru
if (-not $p.WaitForExit(($Seconds + 180) * 1000)) {
    Write-Warning "NEXUS did not finish in time (hang?). Process $($p.Id) left running for inspection."
} elseif ($p.ExitCode -ne 0) {
    Write-Warning "NEXUS exited with code $($p.ExitCode) (crash?)."
}
if ($churnProc -and -not $churnProc.HasExited) { $churnProc.WaitForExit(150000) | Out-Null }

if (Test-Path "$csv.summary.txt") {
    Get-Content "$csv.summary.txt"
    Write-Host "`nSamples: $csv"
} else {
    Write-Warning "No summary was written."
}
