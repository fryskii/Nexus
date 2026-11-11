# NEXUS 0.9.0-alpha.5

A test build of NEXUS, a Task Manager replacement for Windows 11, for testers and contributors. Expect rough edges, and
please report problems with the **Bug report** form under Issues.

## Downloads

- `NEXUS-0.9.0-alpha.5-setup-x64.exe`: installer
- `NEXUS-0.9.0-alpha.5-portable-x64.zip`: no install; unzip anywhere and run `NEXUS.exe`
- `NEXUS-0.9.0-alpha.5-symbols-x64.zip`: only for reading crash reports (developers); not needed to run NEXUS
- `SHA256SUMS.txt`: checksums

The files are not code-signed yet, so Windows SmartScreen warns ("Windows protected your PC"). Choose
**More info → Run anyway** if you trust this source.

## What changed since alpha.4

- **A Simple style.** Settings → Appearance → Style → *Simple* draws NEXUS plain and flat and laid out like Windows Task
  Manager: a solid background, no glows or gradients, no insights panel, one plain process list with tighter rows and
  Task Manager's yellow usage cells, and the Segoe UI font. It is off by default; the layered *NEXUS* look stays the
  default.
- **Your own background color.** Settings → Appearance → Background color has presets and a "+" button for any color, in
  either style. NEXUS switches to light or dark text, whichever is easier to read on that color. Choosing a theme
  returns to the default background.

Both are new and were checked with screenshots only (see "Not tested yet"). If text is hard to read on a color, or
something looks out of place in the Simple style, a screenshot in a bug report helps most.

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

- The Simple style and background colors by hand: only screenshots of Processes, Performance, Overview and Settings
  were checked (dark, light and two custom backgrounds)
- Settings persistence, portable mode, start with Windows, tray, mini monitor
- Replacing Task Manager (leave this off for now)
- The installer's upgrade and uninstall paths
- Display scaling above 100%, light theme, multi-monitor
- Multi-user sessions, SATA drive health, screen readers

Some data (the per-process Network column, for example) needs NEXUS to run as administrator. See PROGRESS.md for the
full status and docs/LIMITATIONS.md for known limitations.
