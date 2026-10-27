# Architecture

```
src/
  core/      Platform-neutral logic (unit-tested on any host): formatting, settings store, SMBIOS parser,
             process filter language, ring buffer, diagnostics rule engine. Win.h/.cpp hold Windows helpers
             (RAII handles, registry, NT definitions resolved at run time).
  data/      Data collection. Sampler (background thread) + collectors: Processes/ProcessMeta (NtQuerySystemInformation
             with Toolhelp fallback), Pdh (PDH queries), CpuInfo (CPUID, GetLogicalProcessorInformationEx), MemoryInfo
             (SMBIOS, firmware/TPM/Secure Boot), DiskInfo (storage IOCTLs, NVMe log, ATA SMART), GpuInfo (DXGI, D3DKMT, SetupAPI),
             NetInfo (GetAdaptersAddresses, WLAN), Connections (TCP/UDP tables + ETW network trace), NetTools (ping, DNS),
             Services, Startup, Users (WTS), EventLog (wevtapi), ProcessDetails/ProcessActions, History (CSV/JSON), Worker.
  ui/        Rendering and widgets: Gfx (Direct2D/DirectWrite), Theme, Widgets (immediate-mode UI), Table (virtualized),
             Graph, Grid, IconCache (WIC), Accessibility (UI Automation), ProcessWindow (inspector), pages/*.
  app/       App (window, navigation, histories, diagnostics scheduling, static-info cache), Commands (actions and native
             dialogs), Tray, MiniMonitor.
  main.cpp   Entry point: single instance, elevated helper verbs, --bench.
```

## Threads

| Thread | Work |
|---|---|
| UI thread | Win32 message loop, painting, input; owns all UI state |
| Sampler | One immutable `Snapshot` per interval: PDH collect, process enumeration, network counters, GPU counters/telemetry; posts `WM_NX_SNAPSHOT` |
| Worker pool (2, below-normal priority) | On-demand queries (static hardware info, services, startup, events, process inspector tabs, signatures, icons, pings); completions are posted back to the UI thread |
| ETW consumer | `ProcessTrace` for the kernel-network session (only when elevated and enabled) |

Snapshots are `shared_ptr<const Snapshot>`. The UI never blocks on collection, and collection never touches UI state.

## Keeping overhead low

- **Demand flags.** Each page declares which expensive collectors it needs: per-process GPU counters, disk counters, window classification, GPU
  telemetry. The sampler skips everything else. When the window is hidden and no mini monitor is open, the interval is multiplied by four (at least 4 s).
- **One system call** (`NtQuerySystemInformation(SystemProcessInformation)`) returns every process with its threads, memory and I/O counters.
- **Metadata cached per process instance** (PID plus creation time): path, command line, user, version info, architecture and protection are read once.
  Version info and SID lookups have bounded caches.
- **Paint only on change.** Frames are drawn when a snapshot arrives or input changes the hovered widget. Hover tracking compares hit-test
  results instead of redrawing on every mouse move. There are no animations or timers while idle.
- **Virtualized tables** format only visible rows. Ring buffers are fixed-size and do not allocate after warm-up.
- **Lazy static info.** Hardware, disks, GPUs and adapters are queried on first use, on a worker thread. UI Automation and WLAN libraries are loaded
  only when needed.
- **Segment heap** (manifest) and a static CRT. No .NET, no WebView, no COM servers kept alive.

## Immediate-mode UI

Widgets are functions called during paint (`ui.Button(id, rect, label)`). Hover, press and active state come from the previous frame's hit regions,
so overlapping elements resolve correctly. Mouse button events are processed synchronously in their own frame
(`RedrawWindow(RDW_UPDATENOW)`), so clicks are never merged. Actions that open menus or dialogs are deferred until after `WM_PAINT`
(`ui.Defer`). Text input, dialogs and menus are native Win32 controls, so IME, clipboard and accessibility work as users expect.

## Security model

- The manifest requests `asInvoker`. Privileged single actions re-launch `NEXUS.exe` with a helper verb through UAC: `--svc`,
  `--svc-starttype`, `--startup-set`, `--taskmgr-replace`. The helper performs exactly one operation and exits with a Win32 error code.
- `SetDefaultDllDirectories(System32 | app dir)` hardens the DLL search order. `HeapEnableTerminationOnCorruption` is enabled.
- Termination guards:
  - processes are verified by PID plus creation time (no PID-reuse kills);
  - NEXUS refuses to end or suspend critical processes (`IsProcessCritical` or a known critical image);
  - the user confirms destructive actions.
- Signature checks never hit the network (`WTD_CACHE_ONLY_URL_RETRIEVAL`, no revocation).
- Firmware tables and NT structures are parsed defensively with bounds checks. The SMBIOS parser is fuzz-tested in the unit tests.
- Settings are written atomically (temp file plus `MoveFileEx`) under the user's profile with default ACLs.
