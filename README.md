# NEXUS

A native Windows 11 task manager and system monitor written in C++20 on Win32, Direct2D and DirectWrite.
It aims to cover everything Task Manager does, show much more detail, and use few resources.

- **No runtime dependencies**: one statically linked `NEXUS.exe` (about 4.3 MB, including the Inter font), no .NET, no Electron, no drivers, no services.
- **Least privilege**: runs as a standard user. It elevates only when you ask, either by restarting as administrator or through a one-off
  elevated helper for single actions such as starting a service or toggling a machine-wide startup entry.
- **No telemetry**: NEXUS makes no network connections of its own.

## Sections

| Section | Highlights |
|---|---|
| Overview | Customizable widget dashboard: add, remove, reorder and resize widgets; compact or detailed mode |
| Processes | Task Manager–style grouping (Apps / Background / Windows), plus tree and flat views; heat-map columns; header totals; power-usage estimate and trend; status (efficiency mode, suspended, not responding); highlights for new and exited processes; a filter language (`pid:4`, `cpu>5`, `mem>1GB`, `/regex/`, `!negate`) |
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
| Settings | Theme, font (bundled Inter or Segoe UI), accent, scale, compact mode, update speed, history length, units, tray, start with Windows, replace Task Manager, sidebar customization, history recording, shortcuts |

Also included:
- a per-process inspector window with tabs for threads, modules, memory map, handles, environment, token and privileges, network connections and performance graphs;
- a floating mini monitor;
- a notification-area icon that shows live CPU and memory;
- optional performance recording to CSV, with CSV and JSON export.

## Download / install

- **Installer**: `NEXUS-<version>-setup-x64.exe` adds Start-menu and optional desktop shortcuts, an uninstaller, and optionally sets NEXUS as the Task Manager replacement.
- **Portable**: unzip `NEXUS-<version>-portable-x64.zip` and run `NEXUS.exe`. The `portable` file next to it keeps settings in that folder.

Requires Windows 10 1809 or later on x64. It is designed for Windows 11.

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
- [Development status and continuation plan](PROGRESS.md)

## License

MIT, see [LICENSE](LICENSE).
