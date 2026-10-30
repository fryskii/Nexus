# Changelog

Test builds are on the [Releases page](https://github.com/fryskii/Nexus/releases).

## Unreleased

Changed
- The crash, unexpected-shutdown and hardware-error checks read each event log once, newest first, and stop at the edge of
  their time window instead of reading whole logs (the System log four times over). They start a minute after launch
  rather than straight away, or as soon as System diagnostics is opened or a report is exported (which now waits for
  them). On a test PC, Windows read 4.1 MB from its log files for them instead of 17.8 MB.

Fixed
- On a slow disk, the crash, shutdown and error counts and the Events and Crashes lists could stop short: when the event
  log took more than 2 seconds to return the next events, NEXUS took that as the end.

## 0.9.0-alpha.3 (2026-10-29)

Fixed
- Memory grew by a few KB every time a process inspector window was opened and closed (about 10 MB an hour in a 6-hour
  stress run): each window created a Direct2D factory of its own. All windows now share one.
- Closing NEXUS could leave its process running without a window: the background sampler asked NEXUS's own window for its
  title while the window thread waited for the sampler to stop, so each waited for the other.

## 0.9.0-alpha.2 (2026-10-29)

Fixed
- The window stopped responding about a minute after start on PCs with a very busy disk: the periodic save of settings and
  history waited for the disk on the window's thread. It now writes in the background.
- The window froze while a UAC prompt was open after changing a service's startup type, turning an all-users startup app on
  or off, or switching the Task Manager replacement.
- Disabling a service asked for confirmation twice.
- The installer did not wait for a running NEXUS to close before replacing it, and the uninstaller could leave NEXUS.exe
  behind. Both now ask it to close and end it after 3 seconds.

Added
- Crash reports: if NEXUS crashes, it saves a small dump and a summary in `%LOCALAPPDATA%\NEXUS\Crashes` and mentions them
  at the next start.
- The "Unexpected shutdowns" finding lists when each shutdown happened.
- Without administrator rights, a bar under the process list explains why the Network column shows "—" and offers to
  restart as administrator.
- Test builds show their label (for example 0.9.0-alpha.2) in Settings › About and in the file names.
- A bug-report form on GitHub.

Changed
- Wait-chain analysis names the program a process is waiting for, tells deadlocked threads from threads waiting on them,
  and says when a process is suspended.
- Event-log queries (System diagnostics, crash and shutdown counts) run on threads of their own, so a slow disk no longer
  holds up other pages.

## 0.9.0-alpha.1 (2026-10-28)

First public test build.
