# NEXUS

[![build](https://github.com/fryskii/Nexus/actions/workflows/build.yml/badge.svg)](https://github.com/fryskii/Nexus/actions/workflows/build.yml)

A native Windows 11 task manager and system monitor written in C++20 on Win32, Direct2D and DirectWrite.
It aims to cover everything Task Manager does, show much more detail, and use few resources.

- **No runtime dependencies**: one statically linked `NEXUS.exe` (about 3.7 MB, including the Inter font), no .NET, no Electron, no drivers, no services.
- **Least privilege**: runs as a standard user. It elevates only when you ask, either by restarting as administrator or through a one-off
  elevated helper for single actions such as starting a service or toggling a machine-wide startup entry.
- **No telemetry**: NEXUS makes no network connections of its own.

## Status

Version 0.9.0, in testing ([test builds](https://github.com/fryskii/Nexus/releases)). NEXUS has been built and tested on a real Windows 11 PC (Intel Core i5-14400F, NVIDIA RTX 5060,
Windows 11 Home 26200), and every push is built and tested on Windows and Linux by CI: unit tests, elevated security tests
(25 of 25 pass, none skipped), a collector benchmark, a UI stress run and fuzzing.

On that PC, measured against Task Manager with `scripts/benchmark.ps1` (both elevated, default update speed, 3 runs of 2 minutes;
details and raw data in [docs/BENCHMARKS.md](docs/BENCHMARKS.md)):

| | NEXUS | Task Manager |
|---|---:|---:|
| CPU, window visible | 0.20% | 0.50% |
| CPU, minimized | 0.05% | 0.72% |
| Working set | 68 MB | 161 MB |
| Private memory | 78 MB | 116 MB |
| Startup | 297 ms | 332 ms |

These are results from one machine. Two of NEXUS's own targets are not met yet: working set (goal under 40 MB) and
process enumeration time (goal under 2 ms).

The smoke tests on that PC covered Processes (including the process actions: efficiency mode, priority, affinity, suspend,
dumps and wait chains), Performance, Details, Services, Startup apps, Users, Network, Storage and System diagnostics, and a
30-minute stress run. Still to test there: settings and Windows integration (start with Windows, tray, mini monitor, Task
Manager replacement), the installer, and display scaling. See [PROGRESS.md](PROGRESS.md) and the
[release checklist](docs/RELEASE_CHECKLIST.md).

## Sections

| Section | Highlights |
|---|---|
| Overview | Customizable widget dashboard: add, remove, reorder and resize widgets; compact or detailed mode |
| Processes | Task Manager–style grouping (Apps / Background / Windows), plus tree and flat views; heat-map columns; header totals; power-usage estimate and trend; status (efficiency mode, suspended, not responding); highlights for new and exited processes; a filter language (`pid:4`, `cpu>5`, `mem>1GB`, `/regex/`, `!negate`); when many columns are shown they scroll sideways with Name pinned |
| Performance | CPU (overall or per-logical-processor graphs, kernel time), Memory (composition bar), Disks, Network adapters, GPUs (selectable engines, dedicated/shared memory, temperature) |
| App history | Per-application CPU, GPU, I/O and network totals collected while NEXUS runs, optionally kept across restarts |
| Startup apps | Registry Run keys, Startup folders and packaged-app tasks; enable/disable via Task Manager's own StartupApproved mechanism; shows which entries are running now and what they use |
| Users | Sessions with per-user totals and their processes; disconnect / sign off |
| Details | All of Task Manager's "Select columns" fields, plus more |
| Services | Start / stop / restart / pause, start type, groups, descriptions |
| Hardware | CPU topology (P-cores and E-cores), caches, instruction sets, per-core clocks; GPU telemetry (temperature, clocks, fan, power, PCIe link, HAGS); memory modules from SMBIOS; board and firmware; a sensor-availability matrix |
| Network | Throughput, per-process traffic (ETW, administrator), TCP/UDP endpoints, connection history, adapter counters and errors, ping and DNS tools |
| Storage | Drive identity, NVMe health log and ATA SMART, temperature, live latency and throughput, partition layout, volumes |
| System diagnostics | Evidence-based health findings, top consumers per resource, Secure Boot / TPM / VBS, drivers, event-log errors, crashes and bugchecks, HTML report export |
| Settings | Theme, font (bundled Inter or Segoe UI), accent, scale, compact mode, update speed (0.25 s to 4 s), history length, units, tray, start with Windows, replace Task Manager, sidebar customization, history recording, shortcuts |

Also included:
- a per-process inspector window with tabs for threads, modules, memory map, handles, environment, token and privileges, network connections and performance graphs;
- a floating mini monitor;
- a notification-area icon that shows live CPU and memory;
- optional performance recording to CSV, with CSV and JSON export.

## Download / install

Download the latest build from the [Releases page](https://github.com/fryskii/Nexus/releases). Test builds are not
code-signed yet, so Windows SmartScreen warns before the first start (*More info › Run anyway*).

- **Installer**: `NEXUS-<version>-setup-x64.exe` adds Start-menu and optional desktop shortcuts, an uninstaller, and optionally sets NEXUS as the Task Manager replacement.
- **Portable**: unzip `NEXUS-<version>-portable-x64.zip` and run `NEXUS.exe`. The `portable` file next to it keeps settings in that folder.

Requires Windows 10 1809 or later on x64. It is designed for Windows 11.

## Reporting problems

Use the **Bug report** form under Issues. If NEXUS crashed, it saved a report in `%LOCALAPPDATA%\NEXUS\Crashes` and says so
the next time it starts: attach the newest `.txt` and `.dmp` files. If the whole PC froze or restarted, *Reliability
Monitor* (Start, type "reliability") shows what Windows recorded at that time.

## Third-party

- [Inter](https://rsms.me/inter/) typeface, © The Inter Project Authors, SIL Open Font License 1.1 (`res/fonts/Inter-OFL.txt`).

## Documentation

- [User guide](docs/USER_GUIDE.md)
- [Building from source](docs/BUILDING.md)
- [Stress testing](docs/STRESS_TESTING.md)
- [Security](docs/SECURITY.md)
- [Feature parity with Task Manager](docs/FEATURE_PARITY.md)
- [Known limitations and unsupported data](docs/LIMITATIONS.md)
- [Benchmark methodology and results](docs/BENCHMARKS.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Changelog](CHANGELOG.md)
- [Release checklist](docs/RELEASE_CHECKLIST.md)
- [Development status and continuation plan](PROGRESS.md)

## License

MIT, see [LICENSE](LICENSE).
