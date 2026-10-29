# NEXUS 0.9.0-alpha.3

A test build of NEXUS, a Task Manager replacement for Windows 11, for testers and contributors. Expect rough edges, and
please report problems with the **Bug report** form under Issues.

## Downloads

- `NEXUS-0.9.0-alpha.3-setup-x64.exe`: installer
- `NEXUS-0.9.0-alpha.3-portable-x64.zip`: no install; unzip anywhere and run `NEXUS.exe`
- `SHA256SUMS.txt`: checksums for both

The files are not code-signed yet, so Windows SmartScreen warns ("Windows protected your PC"). Choose
**More info → Run anyway** if you trust this source.

## What changed since alpha.2

- **Closing NEXUS could leave it running in the background** without a window, so it stayed in the process list until it
  was ended. Fixed.
- **Memory grew a little every time a process details window was opened and closed** (about 10 MB an hour in a 6-hour
  stress test). Fixed; those windows also open faster and use less memory.

Earlier changes are in CHANGELOG.md.

## Reporting problems

If NEXUS crashes, it saves a report in `%LOCALAPPDATA%\NEXUS\Crashes` and says so at the next start. Please attach the
newest `.txt` and `.dmp` files to your bug report. If the whole PC froze or restarted, a screenshot from Reliability
Monitor (Start, type "reliability") for that time helps.

## Tested on a real Windows 11 PC

- Unit tests and elevated security tests (25 of 25)
- Processes, Performance, Details, Services, Startup apps, Network, Storage and System diagnostics
- Process actions: end task and end tree, efficiency mode, priority, affinity, suspend and resume, full dump and minidump,
  wait-chain analysis
- A benchmark against Task Manager (docs/BENCHMARKS.md), a 30-minute stress run with 300 short-lived processes and a
  6-hour soak test

## Not tested yet

- Settings persistence, portable mode, start with Windows, tray, mini monitor
- Replacing Task Manager (leave this off for now)
- The installer's upgrade and uninstall paths
- Display scaling above 100%, light theme, multi-monitor
- Multi-user sessions, SATA drive health, screen readers

Some data (the per-process Network column, for example) needs NEXUS to run as administrator. See PROGRESS.md for the
full status and docs/LIMITATIONS.md for known limitations.
