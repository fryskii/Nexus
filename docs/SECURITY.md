# Security

NEXUS runs elevated in two situations: when you choose *Restart as administrator*, and when it
replaces Task Manager on an administrator account (Windows can start the replacement elevated
without a prompt). While elevated it must not give programs running as you any way to gain
administrator rights. This document lists the attack surface, the protections and how they are
tested.

## Threat model

The attacker is a program already running as you, without administrator rights. It can edit your
files and registry keys, start NEXUS.exe with any arguments, and post messages to NEXUS windows of
the same privilege level. It wants code or file writes at administrator level. Kernel attacks and
attackers who are already administrator are out of scope.

## Protections

| Surface | Risk | Protection |
|---|---|---|
| Task Manager replacement (IFEO `Debugger`) | Point Task Manager at another program, or swap NEXUS.exe, to run code elevated on Ctrl+Shift+Esc | The helper only registers its own executable, and only if `IsProtectedFromNonAdmins` confirms that the file, its folder and every parent folder are owned by and only changeable by Administrators, SYSTEM or TrustedInstaller (junctions resolved first). Install with the installer (Program Files). |
| Elevated helper commands (`--svc`, `--svc-starttype`, `--startup-set`, `--taskmgr-replace`) | Get a NEXUS-branded UAC prompt approved for an attacker-chosen action | Arguments are untrusted: only existing services and existing machine-wide startup entries, no paths or registry locations from the command line, fixed value sets. Stopping, pausing, restarting or disabling a service and re-enabling a startup entry are confirmed in the elevated window, which lower-privilege programs cannot click (UIPI); that confirmation is shown topmost and in the foreground, so it cannot be left waiting unseen behind NEXUS. Arguments are built with `QuoteArg`. |
| DLL loading | Plant a DLL (e.g. `version.dll`) next to NEXUS.exe | Non-KnownDLL imports are delay-loaded and resolved from System32 only (`SetDefaultDllDirectories`); the PE load config also sets `DependentLoadFlags` to System32. |
| File writes while elevated (settings, history, exports, dumps) | Replace a profile folder with a junction or symlink, or plant a link in place of a file, to redirect an administrator's write into a system folder | `SafeWriteGuard` (src/core/SafeFile.h): no network or device paths; no junction or symbolic link anywhere on the path; every folder held open without delete sharing during the write; files are never truncated in place (new file + rename); links and hard-linked files are refused, checked on the opened handle; the opened file must resolve into the locked folder. |
| Settings file (user-editable) | Point the history folder at a system folder | Elevated, the history folder must be inside your profile; otherwise the default is used. |
| Process dumps | A dump of a privileged process readable by every program running as you | Elevated, dumps get a protected ACL: Administrators and SYSTEM only. Default dump names are reduced to plain file names. |
| Data read from other processes (command lines, environment, current directory, handles) | Crafted structures crash or corrupt an elevated NEXUS | Lengths from other processes are bounded and rounded to whole characters; handle snapshots are bounded by the buffer; names of pipes are never queried (they can block). |
| Window messages | Post a message that makes NEXUS call an attacker-chosen address | No message carries a pointer: worker completions are ids looked up in NEXUS's own table. NEXUS never relaxes UIPI. |
| Search box | A regular expression that freezes or crashes NEXUS | `SafeRegex`: linear-time NFA, no recursion over the text, size limits. Fuzzed. |
| Process names | Malware named like a Windows component | SYSTEM labels for name-only processes require session 0; the Windows-folder check uses the full path. |
| Reports | Script in process names when the HTML report is opened | All text HTML-escaped. |
| Network | Remote access | NEXUS opens no listening sockets and makes no network connections of its own (ping/DNS only when you run them). |

## Tests

- `nexus_tests`: unit tests including argument quoting round-trips through `CommandLineToArgvW`.
- `nexus_security_tests` (Windows, **run elevated**): stages the redirection attacks for real (directory
  junction, hard link, symbolic link) against a stand-in protected file and checks that writes are
  refused and the file is unchanged; checks the dump ACL, UNC/device paths and the profile check.
  Checks that the platform cannot stage are reported as *skipped*, never as passed.
- `tests/fuzz`: libFuzzer + sanitizers over the parsers, including the regex engine.

Results on Windows: **25 passed, 0 failed, 0 skipped**, both on every CI run (GitHub Actions
`windows-2022`, elevated) and on a real Windows 11 PC (2026-10-28, run from an elevated prompt).
To run them yourself:

    # elevated PowerShell, from the build folder
    .\nexus_security_tests.exe

Under Wine, junctions, real symbolic links and stored file ACLs are not available, so those three
checks are skipped there.

Also verified under Wine: a planted `version.dll` is not loaded; the helpers refuse a foreign
Task Manager path, arbitrary HKLM keys, unknown services and invalid values; a history folder set to
`C:\Windows\...` is ignored; settings still save normally.

## Known limitations

- NEXUS is not code-signed yet, so the UAC prompt shows "Unknown publisher".
- Not yet independently reviewed. Tested on Windows by CI and on one real Windows 11 PC.
- Changing a service's start type, toggling a startup entry and the Task Manager replacement wait for
  the elevated helper on the UI thread, so the NEXUS window does not respond until the UAC prompt and
  the confirmation are answered (up to 5 minutes). Stopping, starting and restarting services wait on a
  separate thread.
- Folders that NEXUS creates (`SHCreateDirectoryEx`) are created before the guard runs; at most an
  empty folder could be created through a junction, and only inside the profile or NEXUS's own folders.

## Reporting

Please report security problems privately to the repository owner rather than in a public issue.
