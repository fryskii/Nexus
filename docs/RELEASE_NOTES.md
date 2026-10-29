# NEXUS 0.9.0-alpha.2

A test build of NEXUS, a Task Manager replacement for Windows 11, for testers and contributors. Expect rough edges, and
please report problems with the **Bug report** form under Issues.

## Downloads

- `NEXUS-0.9.0-alpha.2-setup-x64.exe`: installer
- `NEXUS-0.9.0-alpha.2-portable-x64.zip`: no install; unzip anywhere and run `NEXUS.exe`
- `SHA256SUMS.txt`: checksums for both

The files are not code-signed yet, so Windows SmartScreen warns ("Windows protected your PC"). Choose
**More info → Run anyway** if you trust this source.

## What changed since alpha.1

- **No more "Not Responding" on a busy disk.** NEXUS saved its settings every minute on the window's thread and waited for
  the disk; on a laptop whose disk was fully busy, the window froze. Saving now happens in the background.
- The window no longer freezes while a UAC prompt is open after changing a service's startup type, turning an all-users
  startup app on or off, or switching the Task Manager replacement. Disabling a service asks once instead of twice.
- **Crash reports.** If NEXUS crashes, it saves a report in `%LOCALAPPDATA%\NEXUS\Crashes` and says so at the next start.
  Please attach the newest `.txt` and `.dmp` files to your bug report.
- The "Unexpected shutdowns" finding in System diagnostics lists when each shutdown happened. These come from the Windows
  event log; NEXUS only reports them.
- Without administrator rights, a bar under the process list explains why the Network column shows "—" and offers to
  restart as administrator.
- Wait-chain analysis names the program a process is waiting for and tells deadlocked threads from threads waiting on them.
- The installer waits for a running NEXUS to close before replacing it.
- Settings › About and the file names show the test-build label (0.9.0-alpha.2).

The full list is in CHANGELOG.md.

## Tested on a real Windows 11 PC

- Unit tests and elevated security tests (25 of 25)
- Processes, Performance, Details, Services, Startup apps, Network, Storage and System diagnostics
- Process actions: end task and end tree, efficiency mode, priority, affinity, suspend and resume, full dump and minidump,
  wait-chain analysis
- A benchmark against Task Manager (docs/BENCHMARKS.md) and a 30-minute stress run with 300 short-lived processes

## Not tested yet

- Settings persistence, portable mode, start with Windows, tray, mini monitor
- Replacing Task Manager (leave this off for now)
- The installer's upgrade and uninstall paths
- Display scaling above 100%, light theme, multi-monitor
- Multi-user sessions, SATA drive health, screen readers

Some data (the per-process Network column, for example) needs NEXUS to run as administrator. See PROGRESS.md for the
full status and docs/LIMITATIONS.md for known limitations.
