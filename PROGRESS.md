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

## Visual redesign (Fluent "NEXUS" look)

Implemented from the reference mockup:
- Custom title bar (logo, admin badge, search pill, caption buttons with snap-layout support via `WM_NCHITTEST`), navy gradient
  background with soft glows, glowing sidebar selection, rounded content panel.
- Page header with *Run new task*, page actions and a "•••" overflow menu; the old toolbar and status bar are gone (status → toast).
- Processes table: metric pills in the header, rounded group panels, block heat cells, leaf badge for efficiency mode,
  columns that squeeze for narrow windows without losing saved widths.
- Insights rail with tinted metric cards, system information and quick actions (NEXUS EcoQoS efficiency mode, dark mode, always on top, details).
- Accent: NEXUS blue default, Windows accent or custom swatches. Mica setting removed (custom background).

Verified only under Wine, which has no Segoe Fluent icon font (glyphs show as boxes) and seems to ignore gradient-stop alpha.
To check on Windows 11: icon glyphs, gradient transparency, snap layouts, maximized frame insets, per-monitor DPI.

## Stress testing

Tooling and Wine results are in docs/STRESS_TESTING.md (fuzzing, `--stress` mode, `nexus_churn`).
Five bugs were found and fixed (regex freeze/stack overflow, numeric UB, settings whitespace, timer
starvation, per-frame resource creation). On Windows 11 (2026-10-29, i5-14400F, `scripts\stress.ps1`,
30 minutes, 300 churned processes, 10 replaced per second): no hang or crash; handles, GDI/USER objects
and threads flat; between the 3rd and last decile private bytes went 206 → 213 MB and working set
115 → 121 MB. A slow drift; every per-process cache checked is bounded, so watch it in the 8-hour soak.

## Continuous integration

GitHub Actions (`.github/workflows/build.yml`) on every push: MSVC build on Windows with unit tests,
elevated security tests (25/25, nothing skipped), collector benchmark and a 60 s stress run; MinGW
cross build producing the installer and portable zip; Linux sanitizer tests and 5 minutes of fuzzing.
`soak.yml` (manual): long stress with process churn. A 20-minute soak showed no leak. The first MSVC
builds found real bugs (UTF-8 source handling, SDK macro clash, a missing header, link dependencies).

## Security hardening

See docs/SECURITY.md. Done: elevated helpers validate arguments and confirm risky actions in the
elevated window; Task Manager replacement only from an admin-protected install; DLL planting blocked
(delay-load + System32-only search); elevated file writes guarded against junction/symlink/hard-link
redirection (SafeWriteGuard); history folder confined to the profile when elevated; dumps restricted to
administrators; pointer-free window messages; bounded cross-process reads. To do on Windows: run
`nexus_security_tests.exe` elevated (junction, symlink and ACL checks are skipped under Wine).

## Where we left off (2026-10-28 and 29, first sessions on a real Windows 11 PC)

Tested on an i5-14400F / RTX 5060 / Windows 11 Home 26200 PC, built natively with WinLibs MinGW-w64
(no Visual Studio on that PC: `cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release`,
`cmake --build build-mingw`). Unit tests 160/160, elevated security tests 25/25, CI green on `bcec2ff`.

- **Benchmark vs Task Manager done:** see docs/BENCHMARKS.md. NEXUS wins on CPU, memory and startup;
  the working-set (< 40 MB) and enumeration (< 2 ms) targets are not met yet.
- **Smoke-test parts 1–3 done** (ticked below), with fixes for: widget-ID collisions, insights cards,
  invisible text colours, layout overlaps, frame pacing, slow re-sort, the hidden elevated-helper
  confirmation, worker starvation, boxed/fitted headers and horizontal table scrolling.
- **Early preview released 2026-10-28:** v0.9.0-alpha.1, a GitHub pre-release built by CI from a
  `v*` tag (notes in docs/RELEASE_NOTES.md).
- **Smoke-test part 4 done** on a copy of PING.EXE run from %TEMP% (System32 binaries count as
  Windows processes, which are refused Efficiency mode by design), each result confirmed outside
  NEXUS: efficiency mode on (EcoQoS + Idle) and off, Below normal priority, affinity to CPUs 0-1,
  suspend/resume. Dumps and wait chains ran through NEXUS's own code from a test program, not the
  menu (their result dialogs are not yet seen): full dump (14 MB) and minidump (51 KB) both valid
  (requested streams, all memory, unique file names); a dump of csrss.exe is refused without
  leaving a file; wait chains found mutex and critical-section deadlocks and a process wait in
  about 0.3 s each, also on a suspended process.
- **Next** (also in docs/RELEASE_CHECKLIST.md): the tester's answers (below); click-test the
  2026-10-29 fixes; settings/integration (persistence, portable, start with Windows, tray, mini
  monitor, Task Manager replacement); installer (CI builds it: test the release's setup.exe,
  including installing over a running NEXUS); DPI/light theme; the 8-hour soak.
- **Fixed 2026-10-29, not yet clicked through in the UI:** a bar under the process list explains
  the Network column's "—" without admin and offers Restart as administrator (× hides it,
  `processes.netHint`); changing a service start type, toggling an all-users startup app and the
  Task Manager replacement switch wait for the elevated helper on a thread of their own instead of
  freezing the window; disabling a service asks once instead of twice; wait-chain reports name the
  process a process wait is for, separate threads in a deadlock from threads waiting on one, and say
  when the process is suspended (`DescribeWaitChains` in ProcessDetails.cpp, checked against a
  deliberately deadlocked test app).
- **Tester report (Dell Latitude 7410, Windows 11 25H2): "crashed my computer 3 times".** His video
  shows NEXUS's window going "Not Responding" about 60 s after start while the disk is 100% busy:
  the 60-second save (`kTimerSave` → `SaveSettings` → `WriteFileAtomic` with `FlushFileBuffers`,
  plus `recorder_.Flush()`) runs on the UI thread and waits for the disk. The "3" probably comes from
  the System diagnostics card (3 Kernel-Power 41 events in 30 days, likely before NEXUS was
  installed). Fixed the same day: the 60-second save (settings, app-history file, recorded history;
  `HistoryRecorder::Add` no longer writes either) is prepared on the UI thread and written by
  `App::SaveInBackground` on a thread of its own, and closing waits for it through `DiskWriteLock()`;
  the card lists when the shutdowns happened. Not yet seen on a slow disk.
  Asked the tester: whole-PC freeze or only NEXUS, disk active time with NEXUS closed, dates of the
  Kernel-Power 41 events. The Crashes tab query took 25+ s on that disk; event-log queries (Crashes
  and Events tabs, crash and shutdown counts) now run on `worker::RunLong` threads, so a slow disk no
  longer holds up the two shared workers.
- **Also on 2026-10-29:** crash reports (`src/app/CrashReport.cpp`: unhandled exceptions,
  std::terminate and C runtime errors write a minidump and a summary to %LOCALAPPDATA%\NEXUS\Crashes
  from a thread of their own, newest 10 kept, mentioned at the next start; tested with real
  access-violation, stack-overflow, uncaught-exception and CRT-error crashes of a test program);
  version labels (`NEXUS_VERSION_LABEL`: CI labels tag builds from the tag and other pushes
  `dev.<commit>`, and fails a tag that does not match `CMakeLists.txt`); plain version tags make
  full releases; artifact actions on upload v7 / download v8; installer and uninstaller wait up to
  3 s for NEXUS to close, then end it (not run yet: part of the installer test); a GitHub bug-report
  form; CHANGELOG.md, docs/RELEASE_CHECKLIST.md, and README, LIMITATIONS, STRESS_TESTING and
  USER_GUIDE brought up to date.

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

- [x] Processes: grouping (apps own their child processes), tree, flat, search terms, sort, column chooser, header totals, heat map
- [x] End task / tree on a test app; refusal for csrss.exe (access-denied → restart-as-admin offer not yet seen)
- [x] Efficiency mode on/off (leaf status); priority; affinity; suspend/resume; full dump and minidump; wait chain (dumps and wait chain without the menu)
- [x] Performance: CPU % matches Task Manager (±2%), speed, logical-processor graphs, memory composition, disks, adapters, GPU engines and temperature
- [x] Details: all columns populated; sort by every column
- [x] Services: start/stop/restart (non-elevated → UAC helper), start-type change
- [x] Startup: disable an HKCU entry → Task Manager shows Disabled (HKLM entry through the helper: confirm)
- [ ] Users: per-user totals ✓; disconnect and sign off in a second session (not tested: single-user PC)
- [x] Network: per-process traffic when elevated, connections, history, ping, DNS
- [ ] Storage: NVMe health ✓, volumes ✓; SATA SMART not tested (no SATA drive)
- [ ] Diagnostics: events, crashes, drivers, report export ✓; findings under synthetic load not tested
- [ ] Settings persist across restarts; portable mode; start with Windows; replace Task Manager on/off; tray; mini monitor
- [ ] Installer: install, upgrade over a running instance (it now waits up to 3 s for NEXUS to close), uninstall leaves no files or registry entries (except user data unless selected)

## Ideas after parity

- ETW kernel-disk attribution for a true per-process physical "Disk" column (elevated)
- UIA grid/table providers for cell-level screen-reader navigation
- Optional vendor GPU SDKs (NVAPI/ADLX), loaded only if present, for hotspot temperature and throttle reasons
- Per-process GPU memory history in the inspector; GPU engine timeline
- Process start/stop event log with command lines (ETW process provider)
