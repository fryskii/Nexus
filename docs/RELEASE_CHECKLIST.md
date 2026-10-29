# Checklist for the first official release (1.0)

Who: *(you)* = the project owner at the PC, *(Claude)* = can be done without anyone at the PC, *(tester)* = needs hardware
or a setup we do not have. Status details are in [PROGRESS.md](../PROGRESS.md).

## Stability

- [ ] A tester's "crashed my computer" report (Dell Latitude 7410): get his answers (whole PC or only NEXUS, shutdown
      dates) and have him try the build with the freeze fix *(tester)*
- [x] Publish 0.9.0-alpha.2 with the freeze fix (2026-10-29) *(Claude)*
- [x] Soak test (6 hours, 2026-10-29): found and fixed a leak when opening inspector windows and a deadlock when NEXUS
      closes *(Claude)*
- [ ] Repeat the soak with those fixes, ideally overnight *(Claude, PC left alone)*
- [ ] Publish 0.9.0-alpha.3 with the two fixes *(Claude, after your OK)*
- [x] Crash reports: a dump and a summary in `%LOCALAPPDATA%\NEXUS\Crashes`, mentioned at the next start *(Claude)*

## Testing on real Windows

- [ ] Click-test the 2026-10-29 fixes: service startup type, all-users startup app and Task Manager switch no longer
      freeze the window while the UAC prompt is open; the "Restart as administrator" bar; the crash-report notice *(you)*
- [ ] Settings survive a restart; portable mode; start with Windows; tray; mini monitor; Replace Task Manager on and off
      *(you + Claude)*
- [ ] Installer: install, install again while NEXUS is running, uninstall leaves no files or registry entries
      *(you + Claude)*
- [ ] Display scaling 100%, 150%, 200%; a second monitor; light theme *(you)*
- [ ] Narrator or NVDA: a quick pass through the main pages *(you)*
- [ ] Hardware we do not have: SATA drive health, signing out another user, a laptop with two GPUs *(tester)*

## Decisions

- [ ] Code signing: unsigned downloads show "Windows protected your PC". Buy a certificate, or ship unsigned and say so.
- [ ] Memory: NEXUS uses less than Task Manager but misses its own goal of a working set under 40 MB. Fix it, or change
      the goal in the docs.
- [ ] The six missing Task Manager features (docs/FEATURE_PARITY.md, "Not implemented"): which, if any, 1.0 needs.

## Release mechanics

- [ ] Version 1.0.0 in `CMakeLists.txt` **and** `res/nexus.rc` (file version), release notes, CHANGELOG entry *(Claude)*
- [x] The release workflow publishes a full release for a plain version tag (v1.0.0) and a pre-release for a labelled one
      (v0.9.0-alpha.2); a tag that does not match the version in `CMakeLists.txt` fails the build *(Claude)*
- [ ] README with screenshots from real Windows (choose what may be shown: they include process names) *(you + Claude)*
- [x] Bug-report form on GitHub *(Claude)*
- [x] CI actions on current versions (no Node.js 20 warnings) *(Claude)*
- [ ] A release candidate (1.0.0-rc.1) with testers for about a week, then 1.0.0

## Can wait until after 1.0

- Restyle the process inspector window and the mini monitor to the current design
- Ideas in PROGRESS.md: per-process Disk column (ETW), screen-reader table navigation, NVIDIA/AMD extras
