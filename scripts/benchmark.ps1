<#
.SYNOPSIS
  Measures NEXUS against Windows Task Manager under identical conditions and writes a report.

.DESCRIPTION
  For each application the script: starts it, measures time until its main window is ready,
  waits a warm-up period, then samples CPU time, working set and private bytes once per second.
  It repeats the measurement with the window visible and minimized.

  Run it from an ELEVATED PowerShell: Task Manager auto-elevates for administrators, so NEXUS must
  also run elevated for an equal comparison. Close other heavy applications first and leave the
  machine idle during the run. Both tools should use their default update speed (Normal, 1 s)
  and start on their Processes page.

.EXAMPLE
  .\scripts\benchmark.ps1 -Nexus .\build\Release\NEXUS.exe -Seconds 120
#>
param(
    [Parameter(Mandatory = $true)][string]$Nexus,
    [int]$Seconds = 120,
    [int]$WarmupSeconds = 15,
    [int]$Runs = 3,
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "docs\benchmarks")
)
$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Warning "Not elevated: Task Manager will run elevated but NEXUS will not, so results are not comparable." }
$Nexus = (Resolve-Path $Nexus).Path
$cores = [Environment]::ProcessorCount
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Stop-Target($name) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 2
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

function Measure-App($label, $exe, $procName, [switch]$Minimized) {
    Stop-Target $procName
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Start-Process -FilePath $exe | Out-Null
    $p = $null
    while ($sw.Elapsed.TotalSeconds -lt 30) {
        $p = Get-Process -Name $procName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($p) { break }
        Start-Sleep -Milliseconds 10
    }
    if (-not $p) { throw "$label did not show a window within 30 s" }
    try { $p.WaitForInputIdle(10000) | Out-Null } catch {}
    $startupMs = $sw.Elapsed.TotalMilliseconds
    if ($Minimized) { [Win]::ShowWindow($p.MainWindowHandle, 6) | Out-Null }
    Start-Sleep -Seconds $WarmupSeconds
    $samples = @()
    $p.Refresh()
    $lastCpu = $p.TotalProcessorTime.TotalMilliseconds
    $lastT = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Seconds; $i++) {
        Start-Sleep -Seconds 1
        $p.Refresh()
        $cpu = $p.TotalProcessorTime.TotalMilliseconds
        $elapsed = $lastT.Elapsed.TotalMilliseconds
        $lastT.Restart()
        $samples += [pscustomobject]@{
            Cpu = 100.0 * ($cpu - $lastCpu) / ($elapsed * $cores)
            WorkingSetMB = $p.WorkingSet64 / 1MB
            PrivateMB = $p.PrivateMemorySize64 / 1MB
        }
        $lastCpu = $cpu
    }
    Stop-Target $procName
    [pscustomobject]@{
        App = $label
        Mode = $(if ($Minimized) { "minimized" } else { "visible" })
        StartupMs = [math]::Round($startupMs, 0)
        CpuAvgPct = [math]::Round(($samples | Measure-Object Cpu -Average).Average, 3)
        CpuMaxPct = [math]::Round(($samples | Measure-Object Cpu -Maximum).Maximum, 3)
        WorkingSetAvgMB = [math]::Round(($samples | Measure-Object WorkingSetMB -Average).Average, 1)
        WorkingSetMaxMB = [math]::Round(($samples | Measure-Object WorkingSetMB -Maximum).Maximum, 1)
        PrivateEndMB = [math]::Round($samples[-1].PrivateMB, 1)
        PrivateStartMB = [math]::Round($samples[0].PrivateMB, 1)
    }
}

$results = @()
for ($r = 1; $r -le $Runs; $r++) {
    foreach ($min in @($false, $true)) {
        Write-Host "Run $r ($(if ($min) {'minimized'} else {'visible'}))..."
        $results += Measure-App "NEXUS" $Nexus "NEXUS" -Minimized:$min
        $results += Measure-App "Task Manager" "$env:windir\System32\Taskmgr.exe" "Taskmgr" -Minimized:$min
    }
}

# NEXUS internal collection timings (process enumeration cost).
$benchJson = Join-Path $OutDir "nexus-collector.json"
Start-Process -FilePath $Nexus -ArgumentList @("--bench", "`"$benchJson`"", "100") -Wait

$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$machine = [pscustomobject]@{
    Computer = $env:COMPUTERNAME; OS = "$($os.Caption) $($os.Version)"; CPU = $cpu.Name; LogicalProcessors = $cores
    MemoryGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1); Elevated = $isAdmin; Date = (Get-Date).ToString("s")
    Seconds = $Seconds; Warmup = $WarmupSeconds; Runs = $Runs
}
$summary = $results | Group-Object App, Mode | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        App = $g[0].App; Mode = $g[0].Mode
        StartupMs = [math]::Round(($g | Measure-Object StartupMs -Average).Average, 0)
        CpuAvgPct = [math]::Round(($g | Measure-Object CpuAvgPct -Average).Average, 3)
        CpuMaxPct = [math]::Round(($g | Measure-Object CpuMaxPct -Maximum).Maximum, 3)
        WorkingSetAvgMB = [math]::Round(($g | Measure-Object WorkingSetAvgMB -Average).Average, 1)
        PrivateEndMB = [math]::Round(($g | Measure-Object PrivateEndMB -Average).Average, 1)
    }
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
[pscustomobject]@{ Machine = $machine; Runs = $results; Summary = $summary } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutDir "benchmark-$stamp.json")
$md = "# NEXUS vs Task Manager benchmark ($stamp)`n`n"
$md += "Machine: $($machine.CPU), $($machine.LogicalProcessors) logical processors, $($machine.MemoryGB) GB, $($machine.OS). Elevated: $isAdmin.`n"
$md += "Method: $Runs runs per mode, $WarmupSeconds s warm-up, $Seconds one-second samples; default update speed; Processes page.`n`n"
$md += "| App | Mode | Startup (ms) | CPU avg % | CPU max % | Working set avg (MB) | Private bytes end (MB) |`n|---|---|---:|---:|---:|---:|---:|`n"
foreach ($s in $summary) { $md += "| $($s.App) | $($s.Mode) | $($s.StartupMs) | $($s.CpuAvgPct) | $($s.CpuMaxPct) | $($s.WorkingSetAvgMB) | $($s.PrivateEndMB) |`n" }
$md += "`nNEXUS collector timings: ``$(Get-Content $benchJson -Raw)```n"
Set-Content -Path (Join-Path $OutDir "benchmark-$stamp.md") -Value $md
Write-Host $md
