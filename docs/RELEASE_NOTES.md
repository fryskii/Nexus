# NEXUS 0.9.0-alpha.4

A test build of NEXUS, a Task Manager replacement for Windows 11, for testers and contributors. Expect rough edges, and
please report problems with the **Bug report** form under Issues.

## Downloads

- `NEXUS-0.9.0-alpha.4-setup-x64.exe`: installer
- `NEXUS-0.9.0-alpha.4-portable-x64.zip`: no install; unzip anywhere and run `NEXUS.exe`
- `NEXUS-0.9.0-alpha.4-symbols-x64.zip`: only for reading crash reports (developers); not needed to run NEXUS
- `SHA256SUMS.txt`: checksums

The files are not code-signed yet, so Windows SmartScreen warns ("Windows protected your PC"). Choose
**More info → Run anyway** if you trust this source.

## What changed since alpha.3

- **Less disk activity at startup.** NEXUS's check for recent crashes, unexpected shutdowns and hardware errors read
  whole Windows event logs as soon as it started. It now reads only the time window it needs, a minute after launch (or
  when System diagnostics is opened): on a test PC about 4 MB instead of 18 MB. On a slow disk the counts and the Events
  and Crashes lists could also stop short; fixed.
- **If NEXUS stops responding**, starting it again (for example with Ctrl+Shift+Esc when it replaces Task Manager) now
  offers to end it and start a new one, or to open Windows Task Manager. Before, nothing happened.
- **Better crash and hang reports.** A crash report now includes where in NEXUS the crash happened (the call stack), and
  when a stuck NEXUS is ended this way, a report of where it was stuck is saved first.

Earlier changes are in CHANGELOG.md.

## Reporting problems

If NEXUS crashes, or a stuck NEXUS is ended as above, it saves a report in `%LOCALAPPDATA%\NEXUS\Crashes` and says so at
the next start. Please attach the newest `.txt` and `.dmp` files to your bug report. If the whole PC froze or restarted, a screenshot from Reliability
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
