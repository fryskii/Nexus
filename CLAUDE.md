# NEXUS: notes for Claude

NEXUS is a native Windows 11 Task Manager replacement: C++20, Win32, Direct2D/DirectWrite, CMake.
Start with README.md, then docs/ (USER_GUIDE, ARCHITECTURE, SECURITY, STRESS_TESTING, BENCHMARKS) and
PROGRESS.md (state and open work). The owner is not a full-time developer: explain results in plain
language and show screenshots of UI changes.

## Layout

- `src/core/`: platform-neutral logic (formatting, settings, SMBIOS, filter language, `SafeRegex`,
  diagnostics rules) in the `nexus_core` library, plus Windows helpers (`Win.cpp`, `SafeFile.cpp`).
- `src/data/`: collectors (processes, CPU/memory/disk/network/GPU, services, startup, event log),
  the background `Sampler` and the `worker` queue.
- `src/ui/`: `Gfx` (Direct2D), immediate-mode widgets (`Ui`), `Table`, `Theme`, `FramePacer`,
  `pages/*Page.cpp` (one per sidebar page), the process inspector window.
- `src/app/`: `App` (window, message loop), `Chrome.cpp` (title bar, sidebar, header, insights rail),
  `Commands.cpp`, `Stress.cpp`, `LoadConfig.cpp`; `src/main.cpp` (entry point, elevated helpers).
- `tests/`: `tests.cpp` (unit), `security_tests.cpp` (Windows, elevated), `fuzz/`, `stress/churn.cpp`.
- `res/`: icon, manifest, embedded Inter font (OFL); `installer/nexus.nsi`; `scripts/`.

## Build and test on Windows

From "Developer PowerShell for VS 2022" (needs Visual Studio 2022 with Desktop development with C++):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\nexus_tests.exe              # unit tests
.\build\Release\nexus_security_tests.exe     # run from an ELEVATED prompt; must say 0 failed, 0 skipped
.\build\Release\NEXUS.exe                    # the app
.\build\Release\NEXUS.exe --stress 60 "$env:TEMP\s.csv" 1   # UI stress; writes s.csv.summary.txt
.\scripts\stress.ps1 -Nexus .\build\Release\NEXUS.exe -Churn .\build\Release\nexus_churn.exe -Seconds 600
.\scripts\benchmark.ps1 -Nexus .\build\Release\NEXUS.exe     # NEXUS vs Task Manager (elevated, idle PC)
```

`.\scripts\build.ps1 -Package` also builds the portable zip and installer (needs NSIS 3).
CI (`.github/workflows/build.yml`) runs MSVC build + unit + security tests + bench + 60 s stress on
Windows, the MinGW cross build, and Linux sanitizer tests + fuzzing on every push; `soak.yml` is a
manual long stress. Keep CI green; MSVC builds with `/W4`, so avoid new warnings.

## Rules from the project brief

- Never fabricate sensor values: show "—" or "Collecting…" when Windows does not report something.
- No kernel drivers; undocumented APIs never as a critical dependency (fall back to documented ones).
- No telemetry, ads or network connections of NEXUS's own; no mandatory online dependencies.
- Confirm destructive actions; refuse to end/suspend critical Windows processes.
- Do not claim NEXUS is faster or lighter than Task Manager without benchmark results that show it.
- Keep CPU and memory use low (targets: idle < 0.5% CPU, < 40 MB RAM, startup < 500 ms).

## Security rules (see docs/SECURITY.md)

NEXUS can run elevated (restart as admin, or as the Task Manager replacement). Keep these invariants:
- Elevated helpers in `main.cpp` (`--svc`, `--svc-starttype`, `--startup-set`, `--taskmgr-replace`)
  treat arguments as untrusted: validate against what exists, never accept paths or registry
  locations, confirm risky actions in the elevated window. Build helper command lines with `QuoteArg`.
- DLLs: `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`; non-KnownDLL imports are delay-loaded
  (`NEXUS_DELAYLOAD` in CMakeLists.txt, functions listed in `cmake/delayload/<dll>.def`). Using a new
  function from those DLLs means adding it to the .def; a new non-system DLL goes into the list.
  `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` for runtime loads.
- File writes go through `WriteFileAtomic` / `SafeWriteGuard` (no in-place truncation, no writes
  through links when elevated). Elevated, settings-chosen folders must stay inside the user profile.
- Window messages never carry pointers (worker completions use ids).
- Lengths read from other processes are untrusted: bound them and use whole characters.

## UI conventions

- Immediate-mode: pages draw every frame in `Draw(Ui&, Rect)`; request frames with `app.Invalidate()`
  (paced by `FramePacer`; never call `InvalidateRect` directly on NEXUS windows).
- Colours come from `Theme` tokens (dark and light); text uses the bundled Inter font via `Gfx`.
- Icons that must not depend on fonts are vector-drawn in `Gfx` (`DrawIcon`, `Chevron`, `Leaf`, ...).
- Screenshots taken under Wine (earlier work) lack icon fonts, anti-aliasing and true glows; judge the
  look only on real Windows.

## Open work (details in PROGRESS.md)

1. Check the look on real hardware: icons, glows/transparency, 100–200% scaling, multi-monitor.
2. Run `scripts\benchmark.ps1` on a real PC and record results in docs/BENCHMARKS.md.
3. Restyle the process inspector window and the mini monitor to the current design.
4. Code signing (self-signed for personal use, or a CA certificate).
5. Task Manager replacement on an admin account is OK once `nexus_security_tests.exe` passes elevated
   on this PC and NEXUS is installed in Program Files.
