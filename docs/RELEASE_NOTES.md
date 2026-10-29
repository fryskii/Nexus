# NEXUS early preview

A very early build of NEXUS, a Task Manager replacement for Windows 11, published so testers and
contributors can try it. Expect rough edges; please report problems as GitHub issues.

## Downloads

- `NEXUS-<version>-setup-x64.exe`: installer
- `NEXUS-<version>-portable-x64.zip`: no install; unzip anywhere and run `NEXUS.exe`
- `SHA256SUMS.txt`: checksums for both

The files are not code-signed yet, so Windows SmartScreen will warn ("Windows protected your PC").
Choose **More info → Run anyway** if you trust this source.

## What has been tested on a real Windows 11 PC

- Unit tests 160/160 and elevated security tests 25/25
- Processes, Performance, Details, Services, Startup and Network pages
- Process actions: end task / end tree, efficiency mode on/off, priority, affinity, suspend/resume
- Benchmark against Task Manager: see docs/BENCHMARKS.md

## Not tested yet

- Memory dumps, wait-chain analysis
- Settings persistence, portable mode, start with Windows, tray, mini monitor
- Replacing Task Manager (leave this off for now)
- The installer's upgrade and uninstall paths
- Display scaling above 100%, light theme, multi-monitor
- Multi-user sessions, SATA SMART

Some data (the per-process Network column, for example) needs NEXUS to run as administrator.
See PROGRESS.md for the full status and docs/LIMITATIONS.md for known limitations.
