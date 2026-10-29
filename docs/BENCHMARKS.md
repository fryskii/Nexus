# Benchmarks

## Status

**First comparison with Task Manager on a real PC (2026-10-28):** `scripts/benchmark.ps1`, elevated, 3 runs per mode,
15 s warm-up, 120 one-second samples, both tools at their default update speed on the Processes page. Intel Core
i5-14400F (16 logical processors), 15.8 GB, Windows 11 Home 26200, NVIDIA RTX 5060. NEXUS 0.9.0 built with MinGW-w64
(GCC 16.2, WinLibs), commit `8796f99`. Raw data: [`benchmarks/benchmark-20261028-213841.json`](benchmarks/benchmark-20261028-213841.json).

| App | Mode | Startup (ms) | CPU avg % | CPU max % | Working set avg (MB) | Private bytes end (MB) |
|---|---|---:|---:|---:|---:|---:|
| NEXUS | visible | 297 | 0.201 | 0.576 | 67.6 | 78.2 |
| Task Manager | visible | 332 | 0.504 | 1.06 | 161.1 | 115.8 |
| NEXUS | minimized | 297 | 0.047 | 0.577 | 66.3 | 75.2 |
| Task Manager | minimized | 305 | 0.722 | 1.917 | 160.1 | 115.1 |

CPU % is of the whole machine (all 16 logical processors). On this PC, in this test, NEXUS used about 40% of Task
Manager's CPU when visible and under 10% when minimized, about 42% of its working set and 66% of its private bytes,
and started slightly faster. The three runs agreed closely (NEXUS visible CPU 0.19–0.21%; one Task Manager minimized run
averaged 1.26%, the others 0.44–0.46%). Private bytes stayed flat within each 2-minute run.

Against the engineering targets below: idle CPU (< 0.5%) and startup (< 500 ms) are met; working set (67 MB vs < 40 MB)
and process enumeration (7.95 ms average for 217 processes elevated, vs < 2 ms for 300) are not.

**First Windows numbers (GitHub Actions `windows-2022` virtual machine, not a desktop PC):**

| Measurement | Result |
|---|---|
| Collection (`NEXUS.exe --bench`, 138 processes) | 1.1 ms per sample (max 1.6 ms); collector alone 8.4 MB working set, 2.4 MB private |
| UI under stress (`--stress`, about 33 forced redraws per second) | median frame 5.9 ms, p95 15.6 ms; 10–15% of one core |
| 20-minute soak with 300 churned processes | private memory 102 → 103 MB (second vs last quarter), handles 1,492 → 1,483, GDI 68 → 68: no leak |

These come from a server VM without a GPU and are not a comparison with Task Manager. The comparison
below (`scripts/benchmark.ps1`) still has to be run on a real PC.

**Earlier status: no Windows benchmark results existed.** NEXUS was built and tested in a Linux environment under Wine, which does not reproduce Windows
performance characteristics:
- Direct2D runs on wined3d and llvmpipe,
- many performance counters are stubs,
- memory accounting differs.

Claims about NEXUS versus Task Manager should cite the real-PC results above (one machine so far), not the Wine or CI numbers.

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
