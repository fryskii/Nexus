# Stress testing

NEXUS ships with three stress tools. All of them are safe to run on a normal machine: they never
save settings, never confirm a dialog, and only terminate processes they started themselves.

## 1. Core fuzzing (Linux or macOS host, clang)

`tests/fuzz/fuzz_core.cpp` is a libFuzzer harness built with AddressSanitizer and
UndefinedBehaviorSanitizer. It covers SMBIOS table parsing, the search filter language, settings
files (including a save/reload round-trip check), size parsing, number formatting and the
diagnostics rules. It also runs a differential check of the search regex engine (`SafeRegex`)
against `std::regex` on short ASCII inputs.

    sudo apt install clang libclang-rt-18-dev     # or your distribution's equivalent
    tests/fuzz/run.sh /tmp/nexus-fuzz 1800        # work directory, seconds

Any crash, sanitizer report, round-trip mismatch or regex mismatch stops the run and writes the
input as `crash-*` in the work directory. Re-run it with `/tmp/nexus-fuzz/fuzz_core crash-…`.

## 2. UI stress mode

    NEXUS.exe --stress <seconds> [csv-path] [seed]

The window is driven with random but seed-reproducible actions every 30 ms: page navigation,
resizing, search queries (including hostile regular expressions and 2,000-character input),
theme, font, scale and accent changes, compact mode, the insights panel, sidebar collapse, process
view changes, keyboard scrolling, hover, mouse wheel, process inspector windows, the mini monitor
and refresh-rate changes. Sampling runs at the fastest rate (250 ms).

Each second it records NEXUS's own private bytes, working set, handles, GDI and USER objects,
threads and CPU time. When it finishes it writes:

- `<csv>`: one row of resource counters per second;
- `<csv>.summary.txt`: frame-time percentiles (overall and per page), CPU use, the action
  counts, and for each counter the start, peak, and the medians of the second and last quarter
  of the run. A rising last-quarter median after warm-up points to a leak; a flat one does not;
- `<csv>.actions.txt`: a timestamped action log for reproducing a run.

The summary also lists time spent handling snapshots, `WM_TIMER` deliveries by timer id, and the
most frequent main-loop messages by window class, which is how the starvation below was diagnosed.

`NEXUS_STRESS_ONLY` restricts the mix, for example `NEXUS_STRESS_ONLY=resize,theme` or
`NEXUS_STRESS_ONLY=idle` (no UI actions: collection and painting only). Action names are listed in
the summary.

## 3. Process churn

`nexus_churn <alive> <replace-per-second> <seconds>` (built next to `NEXUS.exe`) keeps that many
child processes alive and replaces the oldest ones every second. Children use several executable
names (Unicode, very long, with spaces), long command lines, 1–6 threads, some memory, and one in
ten burns CPU. It copies itself to those names in its own folder.

`scripts/stress.ps1` combines the two on Windows:

    .\scripts\stress.ps1 -Nexus .\build\Release\NEXUS.exe -Churn .\build\Release\nexus_churn.exe -Seconds 1800

If a run is interrupted, end any leftover `churn-*.exe` processes from Task Manager or NEXUS.

## Results on Windows (GitHub Actions)

`build.yml` runs on every push: unit tests, the elevated security tests (25 checks, none skipped:
junction, symbolic-link and hard-link attacks are staged for real), the collector benchmark and a
60-second stress run. `soak.yml` runs a long stress with process churn on demand.

20-minute soak, 300 churned processes, full action mix: no crash or hang; 51,107 frames (median 5.9 ms,
p95 15.6 ms, max 1.6 s once); 2,180 snapshots (median 1.7 ms); CPU 14.9% of one core under forced
redraws. Second-quarter vs last-quarter medians: private memory 102.3 → 103.3 MB (+0.9%), handles
1,492 → 1,483, GDI objects 68 → 68, USER objects 33 → 32, threads 15 → 14. No leak.

The MSVC build also exposed a bug that MinGW and Wine hid: the core library was compiled without
`/utf-8`, so temperatures read "Â°C". All targets are now compiled as UTF-8.

## Results on a Windows 11 PC

2026-10-29, Intel Core i5-14400F / NVIDIA RTX 5060 / Windows 11 26200, `scripts\stress.ps1` for 30 minutes with 300
churned processes (10 replaced per second), full action mix: no crash or hang; 74,388 frames (average 4.8 ms, p95 13.8 ms,
max 92 ms); 3,196 snapshots (median 1.8 ms); CPU 22% of one core under forced redraws. Handles, GDI/USER objects and threads
flat. Second-quarter vs last-quarter medians: private memory 197.9 → 204.2 MB (+3.1%), working set 110.7 → 113.9 MB
(+2.9%). Per tenth of the run, private memory moves within 206–218 MB while the working set rises slowly (115 → 121 MB from
the third tenth to the last). Every per-process cache is bounded; an 8-hour soak will show whether the drift levels off.

## Results under Wine

Recorded on Linux under Wine 9 with Xvfb (software rendering), 4 cores. Wine does not report
private bytes, handle or GDI/USER counts (they read 0), its Direct2D draws text as geometry, and
every window costs a large software swap chain, so absolute frame times and memory are far higher
than on Windows. Leak trends, crashes, hangs and correctness are still meaningful. Results on a
Windows 11 PC are above.

| Test | Result |
|---|---|
| Fuzzing, 30 min, ~24 million inputs | No crashes or sanitizer findings after the fixes below; no regex mismatches |
| Collection benchmark, 309 processes with churn | 35.6 ms average per sample (max 89 ms), on the background thread |
| Stress, each action group isolated, 90–120 s | Memory flat after warm-up (≤ 2% growth between the second and last quarter) |
| Stress, full mix with 150-process churn, 10 min | No crash, clean exit; exposed the timer starvation fixed below (actions stopped after 85 s) |
| Same, 30 min, after the fixes | No crash or hang, clean exit; 7,700 actions and 5,791 frames; one sampling gap over 10 s; working set swings 710–960 MB with inspector windows but the quarter medians differ by 4% (no trend); threads stable; snapshot handling 1.7 ms median |
| Idle at 1 s refresh (Processes page) | ~27% of one core under Wine, almost all of it Wine's software Direct2D; 0.06% when minimized |

Bugs found and fixed:

1. **Regex freeze/crash.** `std::regex` backtracks exponentially (`/(a+)+$/` took more than 20 s on
   31 characters) and recurses once per character, so a long command line could overflow the stack.
   Replaced with `SafeRegex`, a linear-time Thompson NFA (32,767 characters in about 1 ms).
2. **Undefined behaviour on out-of-range numbers**: `pid:1e30`, `mem>-5`, very large sizes, and
   NaN or infinite sampling intervals in the diagnostics rules.
3. **Settings lost leading and trailing whitespace** on reload.
4. **Timer starvation.** When frames took longer than the update interval, a repaint was always
   pending and Windows never delivered `WM_TIMER`, so tooltips, status messages, the periodic
   settings save and the stress deadline stopped (a 150 s run never finished). The first fix paced
   each window by its own draw time, but with the main window, three process inspectors and the
   mini monitor open the thread was still saturated. `FramePacer` now paces all windows of the UI
   thread together, so drawing uses at most about half of the thread.
5. **Per-frame resource creation**: gradient stop collections were created on every draw call,
   and the full-window background (about 40 large translucent fills) was redrawn every frame.
   Both are now cached.
