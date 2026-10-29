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
starvation, per-frame resource creation). Still to do on Windows 11: `scripts\stress.ps1` for 30+ minutes
with 300 churned processes; check private bytes, handles and GDI/USER objects (Wine reports 0).

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

## Where we left off (2026-10-28, first session on a real Windows 11 PC)

Tested on an i5-14400F / RTX 5060 / Windows 11 Home 26200 PC, built natively with WinLibs MinGW-w64
(no Visual Studio on that PC: `cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release`,
`cmake --build build-mingw`). Unit tests 160/160, elevated security tests 25/25, CI green on `bcec2ff`.

- **Benchmark vs Task Manager done:** see docs/BENCHMARKS.md. NEXUS wins on CPU, memory and startup;
  the working-set (< 40 MB) and enumeration (< 2 ms) targets are not met yet.
- **Smoke-test parts 1–3 done** (ticked below), with fixes for: widget-ID collisions, insights cards,
  invisible text colours, layout overlaps, frame pacing, slow re-sort, the hidden elevated-helper
  confirmation, worker starvation, boxed/fitted headers and horizontal table scrolling.
- **Part 4 started 2026-10-28, then paused for an early preview release** (v0.9.0-alpha.1, a
  GitHub pre-release built by CI from a `v*` tag; notes in docs/RELEASE_NOTES.md). Checked on a
  copy of PING.EXE run from %TEMP% (System32 binaries count as Windows processes, which are refused
  Efficiency mode by design), each result confirmed outside NEXUS: efficiency mode on (EcoQoS +
  Idle) and off (EcoQoS explicitly off, Normal), Below normal priority, affinity to CPUs 0-1,
  suspend (all threads suspended) and resume.
- **Next:** finish part 4 (full dump and minidump, wait chain on a suspended process); then
  settings/integration (persistence, portable, start with Windows, tray, mini monitor, Task Manager
  replacement); installer (needs NSIS); DPI/light theme.
- **Ideas noted:** without admin the Network column only shows "—" (reason in its tooltip); a
  visible "run as administrator" hint would help. Changing a service start type and toggling
  startup apps still wait for the elevated helper on the UI thread (window freezes until answered).

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
- [ ] Efficiency mode on/off ✓ (leaf status ✓); priority ✓; affinity ✓; suspend/resume ✓; full dump and minidump; wait chain
- [x] Performance: CPU % matches Task Manager (±2%), speed, logical-processor graphs, memory composition, disks, adapters, GPU engines and temperature
- [x] Details: all columns populated; sort by every column
- [x] Services: start/stop/restart (non-elevated → UAC helper), start-type change
- [x] Startup: disable an HKCU entry → Task Manager shows Disabled (HKLM entry through the helper: confirm)
- [ ] Users: per-user totals ✓; disconnect and sign off in a second session (not tested: single-user PC)
- [x] Network: per-process traffic when elevated, connections, history, ping, DNS
- [ ] Storage: NVMe health ✓, volumes ✓; SATA SMART not tested (no SATA drive)
- [ ] Diagnostics: events, crashes, drivers, report export ✓; findings under synthetic load not tested
- [ ] Settings persist across restarts; portable mode; start with Windows; replace Task Manager on/off; tray; mini monitor
- [ ] Installer: install, upgrade over a running instance, uninstall leaves no files or registry entries (except user data unless selected)

## Ideas after parity

- ETW kernel-disk attribution for a true per-process physical "Disk" column (elevated)
- UIA grid/table providers for cell-level screen-reader navigation
- Optional vendor GPU SDKs (NVAPI/ADLX), loaded only if present, for hotspot temperature and throttle reasons
- Per-process GPU memory history in the inspector; GPU engine timeline
- Process start/stop event log with command lines (ETW process provider)
