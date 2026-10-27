# Benchmarks

## Status

**No Windows benchmark results exist yet.** NEXUS was built and tested in a Linux environment under Wine, which does not reproduce Windows
performance characteristics:
- Direct2D runs on wined3d and llvmpipe,
- many performance counters are stubs,
- memory accounting differs.

This document does **not** claim NEXUS uses fewer resources than Task Manager. That claim needs the measurements described below, on real
hardware.

## Methodology (`scripts/benchmark.ps1`)

Run from an **elevated** PowerShell on an idle machine. Task Manager auto-elevates for administrators, so both tools then have the same privileges.

```powershell
.\scripts\benchmark.ps1 -Nexus .\dist\NEXUS-0.9.0-portable-x64\NEXUS.exe -Seconds 120 -Runs 3
```

For each application (NEXUS, `%windir%\System32\Taskmgr.exe`), each run and each mode (window visible, window minimized):

1. Close any running instance.
2. Start it and measure **startup latency**: the time until the main window exists and the process is input-idle.
3. Wait 15 s for warm-up.
4. Sample once per second for N seconds:
   - CPU: delta of `TotalProcessorTime` ÷ wall time ÷ logical processors,
   - working set,
   - private bytes.
5. Report average and maximum CPU, average working set, and private bytes at start and end. Steadily growing private bytes would indicate a leak.

It also runs `NEXUS.exe --bench`, which reports process-enumeration time (first and average) and NEXUS's own working set with no UI.

For equal conditions, both tools use their default settings: update speed Normal (1 s), the Processes page, and default columns.
NEXUS's per-process network ETW session is active when elevated. This is extra work that Task Manager also does through its own ETW consumer.

Results are written to `docs/benchmarks/benchmark-<timestamp>.md` and `.json`, together with machine details.

## Engineering targets

These are targets, not measured results.

| Metric | Target |
|---|---|
| Idle CPU (visible, Normal speed) | < 0.5 % |
| Idle memory (working set) | < 40 MB (aspirational 20–30 MB) |
| Startup to interactive | < 500 ms on SSD |
| Process enumeration (300 processes) | < 2 ms per sample |
| Memory growth over 8 hours | none (bounded buffers) |

## Indicative numbers from Wine (not representative of Windows)

From `NEXUS.exe --bench` under Wine 9 with 8 processes:
- enumeration: first call 10.5 ms, average 0.95 ms;
- working set without a UI: 33 MB.

Wine's working set includes its own emulation libraries.
