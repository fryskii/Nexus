# Development status and continuation plan

## Current state (v0.9.0)

**Done and verified in this environment** (Linux, MinGW-w64 cross-compiler, Wine 9 with a virtual display):

- **Build.** The whole application cross-compiles warning-free into a single static `NEXUS.exe`.
- **Tests.** Core unit tests pass natively (with AddressSanitizer and UBSan) and under Wine.
- **Pages.** All 13 sections render and update live: Overview, Processes, Performance, App history, Startup apps, Users, Details, Services,
  Hardware, Network, Storage, System diagnostics, Settings. Light and dark themes work.
- **Interactions exercised:** search and filter language, column sorting with tooltips, the process inspector (General, Threads, Modules,
  Memory, Environment), the mini monitor, the tray icon, benchmark mode.
- **Packaging.** The native x64 NSIS installer and the portable ZIP are produced by `scripts/package.sh`. A silent install and uninstall under Wine
  created and removed the files, the Start-menu shortcut and the Add/Remove Programs entry cleanly. Settings persist across a graceful close.
- **Process actions added after the first audit:** Explorer restart, wait-chain analysis, Resource Monitor shortcut, percent mode.

**Implemented, but not runnable under Wine** (code compiles against the real Windows SDK headers; needs validation on Windows 11):

- PDH processor, disk and GPU counters
- D3DKMT GPU telemetry
- the ETW kernel-network session
- NVMe health log and ATA SMART
- Mica
- UI Automation with a real screen reader
- service control and the elevated helper verbs
- StartupApproved writes and the Task Manager (IFEO) replacement
- efficiency mode, dumps, affinity, wait chains

**Not yet done:**

- The MSVC build has not been compiled; `scripts/build.ps1` and CI are untested.
- Benchmarks against Task Manager on real hardware (see docs/BENCHMARKS.md).
- The feature-parity audit against an installed Task Manager build.
- Remaining parity gaps listed in docs/FEATURE_PARITY.md.

## Validation plan (next session on Windows 11)

1. Build with MSVC: `scripts\build.ps1`. Fix any compiler differences; MinGW was the only compiler used so far.
2. Run the smoke-test checklist below, both non-elevated and elevated.
3. Run `scripts\benchmark.ps1` (3 runs, 120 s). Commit the results under `docs/benchmarks/` and update docs/BENCHMARKS.md with the numbers, good
   or bad.
4. Run an 8-hour soak test with performance recording enabled. Check that private bytes stay flat and that handle counts stay stable.
5. Compare every row of docs/FEATURE_PARITY.md against the installed Task Manager, and correct the audit.
6. Test with Narrator and NVDA. Then extend UI Automation to expose table rows (`IGridProvider`) if summaries prove insufficient.
7. Check DPI behaviour at 100%, 150% and 200%, mixed-DPI multi-monitor setups, and moving the window between monitors.

### Smoke-test checklist

- [ ] Processes: grouping (apps own their child processes), tree, flat, search terms, sort, column chooser, header totals, heat map
- [ ] End task / tree on a test app; refusal for csrss.exe; access-denied → restart-as-admin offer
- [ ] Efficiency mode on/off (leaf status, Task Manager agrees); priority; affinity; suspend/resume; full dump and minidump; wait chain
- [ ] Performance: CPU % matches Task Manager (±2%), speed, logical-processor graphs, memory composition, disks, adapters, GPU engines and temperature
- [ ] Details: all columns populated; sort by every column
- [ ] Services: start/stop/restart (non-elevated → UAC helper), start-type change
- [ ] Startup: disable an HKCU entry → Task Manager shows Disabled; an HKLM entry through the helper
- [ ] Users: per-user totals; disconnect and sign off in a second session
- [ ] Network: per-process traffic when elevated, connections, history, ping, DNS
- [ ] Storage: NVMe health and SATA SMART (elevated), partitions, volumes
- [ ] Diagnostics: findings appear under synthetic load (CPU burner, a leaking test app); events, crashes, drivers, report export
- [ ] Settings persist across restarts; portable mode; start with Windows; replace Task Manager on/off; tray; mini monitor
- [ ] Installer: install, upgrade over a running instance, uninstall leaves no files or registry entries (except user data unless selected)

## Ideas after parity

- ETW kernel-disk attribution for a true per-process physical "Disk" column (elevated)
- UIA grid/table providers for cell-level screen-reader navigation
- Optional vendor GPU SDKs (NVAPI/ADLX), loaded only if present, for hotspot temperature and throttle reasons
- Per-process GPU memory history in the inspector; GPU engine timeline
- Process start/stop event log with command lines (ETW process provider)
