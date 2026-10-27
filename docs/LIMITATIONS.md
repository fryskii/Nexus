# Known limitations and unsupported data

NEXUS never fabricates values. When Windows does not expose something, the UI says so: *Not reported*, *Not measured*, *Requires administrator* or
*Requires kernel driver*. This page lists those cases and the exact reason.

## Data that needs a kernel driver (deliberately not provided)

NEXUS ships **no kernel driver**, because a driver adds attack surface, needs signing, and is a common cause of system instability.

| Data | Why it is unavailable |
|---|---|
| CPU package / per-core temperature | Comes from model-specific registers (IA32_THERM_STATUS, AMD SMN), which only kernel code can read |
| CPU package power, core voltage (VID) | RAPL energy MSRs and VID registers are kernel-only |
| Motherboard voltages, fan speeds | Super I/O chips are read through port I/O |
| Memory timings (CL-tRCD-tRP…) | Stored in SPD EEPROMs on the SMBus |

What NEXUS shows instead: ACPI thermal zones (firmware-defined and not necessarily the CPU die), the throttling indicator from `% Performance Limit`,
and the nominal SMBIOS voltage, each labelled as such.

## Vendor-specific data

GPU hotspot temperature, throttle reasons and memory-junction temperature need NVAPI, ADLX or Intel IGCL. GPU temperature, clocks, fan, power and
voltage come from the WDDM D3DKMT performance-data interface, which is the same source Task Manager uses. Each driver decides which fields it
fills.

## Data that needs administrator rights

| Feature | Reason |
|---|---|
| Per-process network throughput (Processes/Details/Network/App history) | Kernel-network ETW sessions require admin or *Performance Log Users*. Without them the column shows "—" with an explanation; connection counts are still shown |
| SATA SMART attributes | `SMART_RCV_DRIVE_DATA` needs a read/write handle to the physical drive |
| Full partition table | `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` needs read access; volumes are shown regardless |
| Details of other users' / protected processes | Windows denies `PROCESS_QUERY_INFORMATION` / `PROCESS_VM_READ`; inaccessible fields stay blank |
| Starting/stopping most services, machine-wide startup entries | Performed through a UAC-prompted one-off helper when needed |

## Differences from Task Manager

- **Startup impact.** Task Manager computes it from boot-performance traces through a private interface. NEXUS shows *Not measured*, plus whether each
  startup app is running now and what it uses.
- **App history.** Task Manager reads the SRUM database, which the Diagnostic Policy Service keeps locked. NEXUS measures usage while it runs
  and can persist it across restarts.
- **Disk column.** NEXUS shows *Disk I/O*: read and write bytes per second from the kernel process counters. These include cached file I/O. Task
  Manager attributes physical disk I/O using kernel-logger ETW. The column tooltip explains this.
- **Power usage.** Task Manager uses the private Energy Estimation Engine. NEXUS uses a documented heuristic, labelled as an estimate:
  `score = CPU% + 0.6·GPU% + 0.15·min(I/O MB/s, 100) + 0.2·min(network MB/s, 100)`, with bands at 0.1 / 2 / 10 / 30 and a two-minute
  exponential average for the trend.
- **"Operating system context", "Enterprise context" and "Data Execution Prevention"** Details columns are not provided. DEP, ASLR, CFG and CET
  are in the process inspector's General tab instead.
- **Packaged-app startup tasks** are listed read-only. Changing them requires the WinRT StartupTask API in the app's own package context.
- **Processor affinity** covers processor group 0 (up to 64 logical processors). Multi-group CPU sets are not editable.
- **Process environment and working directory** are read from the target's process parameters (x64 layout), which can be unavailable for
  protected processes.
- **Handle names** are resolved only for disk files and named kernel objects. Pipes and some devices are skipped because querying them can
  block indefinitely.

## Accessibility

The main window exposes a UI Automation tree: navigation items that support Select/Invoke, plus a content element. The content element's name summarizes the
current page and the selected row, and status messages are announced. Individual table cells and graph values are not exposed as separate
automation elements yet, so screen-reader users get a summary rather than cell-by-cell navigation. Dialogs (Run new task, affinity, task
dialogs) are native and fully accessible. Everything is reachable by keyboard.

## Platform

- Windows 10 1809 or later on x64. ARM64 builds are untested. Mica requires Windows 11 22H2 or later and falls back to a solid color.
- On systems without the *Segoe Fluent Icons* or *Segoe MDL2 Assets* fonts, icons render as boxes. Both fonts ship with Windows 10 and 11.

## Verification status

NEXUS was developed in a Linux environment by cross-compiling with MinGW-w64. It was exercised under Wine with a virtual display, where every page,
the process inspector, search, sorting, the mini monitor, the tray, dark mode and benchmark mode work. Wine does not implement many Windows performance
counters, GPU, storage and ETW interfaces, so those code paths compile against the real Windows SDK headers but **have not yet been run on real
Windows 11 hardware**. The MSVC build has not been compiled in this environment either. See [PROGRESS.md](../PROGRESS.md) for the validation plan.
