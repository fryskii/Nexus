# Feature parity with Windows 11 Task Manager

Target: Task Manager in Windows 11 23H2/24H2 (the redesigned WinUI version with the navigation pane, efficiency mode and the Settings page).

> Audit note: this checklist was compiled from Task Manager's documented and observed behaviour. It was **not** produced by inspecting an
> installed copy, because the development environment had no Windows installation. Please re-verify it against the installed build (see PROGRESS.md).

Legend: ✅ implemented · 🟡 implemented with a documented difference · ❌ not implemented

## Global

| Task Manager feature | Status | Notes |
|---|---|---|
| Navigation pane, collapsible | ✅ | Ctrl+B; sections can also be hidden or set as the start page |
| Search box (filters processes) | ✅ | Adds PID, publisher, regex, numeric and negation terms |
| Run new task (with "administrative privileges" option, Browse) | ✅ | Native dialog with autocomplete |
| End task / Efficiency mode toolbar buttons | ✅ | |
| Light / dark / system theme | ✅ | Plus accent color, Mica, UI scale |
| Always on top | ✅ | Ctrl+T |
| Minimize on use, hide when minimized | 🟡 | "Hide when minimized" and "Close to notification area"; there is no "minimize on use" because NEXUS never launches other apps automatically |
| Real-time update speed High / Normal / Low / Paused, Refresh now | ✅ | Plus "Very low"; F5 refreshes |
| Default start page | ✅ | |
| Notification-area icon showing CPU | ✅ | Shows CPU and memory |
| Show full account name | 🟡 | The User name column shows the short name; the inspector shows DOMAIN\user |
| Group by type | ✅ | Grouped view; Tree and Flat views also available |
| Expand all / collapse all | ✅ | View options menu |
| Show history for all processes | 🟡 | App history covers every process (not only packaged apps) but only from when NEXUS started |
| Multiple-instance prevention / activate existing window | ✅ | |
| Keyboard navigation, context menus | ✅ | |
| Ctrl+Shift+Esc opens it | 🟡 | Optional: "Replace Task Manager" (IFEO), set in Settings or the installer |

## Processes

| Feature | Status | Notes |
|---|---|---|
| Apps / Background / Windows processes groups with app process groups | ✅ | |
| Name, Status, Publisher, PID, Process name, Type, Command line | ✅ | |
| CPU, Memory, Disk, Network, GPU, GPU engine columns with heat map | 🟡 | Disk = all process I/O (see LIMITATIONS); Network needs administrator |
| Power usage, Power usage trend | 🟡 | NEXUS heuristic estimate, labelled as such |
| Header totals (% CPU, Memory, Disk, Network, GPU) | ✅ | |
| Status: Efficiency mode, Suspended, Not responding | ✅ | |
| Resource values as percentages or absolute | 🟡 | Percent mode covers Memory and Network (of link capacity); Disk I/O stays absolute because NEXUS does not attribute physical disk time per process |
| Context: Switch to, End task, End process tree, Efficiency mode | ✅ | |
| Context: Create memory dump file | ✅ | Full dump and minidump |
| Context: Go to details, Go to service(s), Open file location, Search online, Properties | ✅ | |
| Context: Provide feedback | ❌ | Not applicable |
| Restart (Windows Explorer) | ✅ | |
| Efficiency mode blocked for Windows processes | ✅ | Same rule as Task Manager |

## Performance

| Feature | Status | Notes |
|---|---|---|
| CPU: utilization graph, overall / logical processors, show kernel times | ✅ | |
| CPU: utilization, speed, processes, threads, handles, up time | ✅ | Plus context switches/s, interrupts/s, queue length, throttling |
| CPU: base speed, sockets, cores, logical processors, virtualization, L1/L2/L3 cache | ✅ | Plus P-core and E-core counts |
| Memory: usage graph, composition bar | ✅ | |
| Memory: in use (compressed), available, committed, cached, paged / non-paged pool | ✅ | |
| Memory: speed, slots used, form factor, hardware reserved | ✅ | From SMBIOS |
| Disk: active time, transfer-rate graphs, response time, read/write speed | ✅ | |
| Disk: capacity, formatted, system disk, page file, type | ✅ | |
| Ethernet / Wi-Fi: throughput graph, send/receive, adapter name, SSID, connection type, IPv4/IPv6, DNS name | ✅ | |
| GPU: 4 selectable engine graphs, dedicated/shared memory graphs | ✅ | |
| GPU: utilization, memory, driver version/date, DirectX version, physical location, temperature | 🟡 | DirectX version is derived from the WDDM version; the feature level is not probed (avoids loading the user-mode driver) |
| Graph summary view, Copy, Change graph to | ✅ | |
| Open Resource Monitor | ✅ | Performance context menu |

## App history

| Feature | Status | Notes |
|---|---|---|
| CPU time, network, metered network, tile updates per app | 🟡 | CPU, GPU, I/O and network time collected by NEXUS while running; SRUM history (before NEXUS started) is unavailable; metered network and tile updates are not tracked |
| Delete usage history | ✅ | |

## Startup apps

| Feature | Status | Notes |
|---|---|---|
| Name, Publisher, Status | ✅ | |
| Startup impact | 🟡 | Shows "Not measured" (private boot-trace data); "Running now" and current CPU/memory are shown instead |
| Enable / Disable | ✅ | Writes StartupApproved exactly as Task Manager does; HKLM entries through a UAC helper |
| Disabled time, Command line, Startup type columns | ✅ | Location column instead of startup type |
| Open file location, Search online, Properties | ✅ | |
| Packaged app startup tasks | 🟡 | Listed read-only |

## Users

| Feature | Status | Notes |
|---|---|---|
| Users with per-user CPU, Memory, Disk, Network, GPU, expandable to processes | ✅ | |
| Status, ID, Session, Client name columns | ✅ | |
| Disconnect, Sign off, Manage user accounts | ✅ | |
| Send message | 🟡 | Implemented in the data layer (`SendSessionMessage`); no UI yet |

## Details

| Feature | Status | Notes |
|---|---|---|
| All columns (Name … Shared GPU memory) | 🟡 | All except Operating system context, Enterprise context, DEP and "Memory (active private working set)", which is shown as private working set |
| End task, End process tree, Set priority, Set affinity, Analyze wait chain, UAC virtualization, Create dump | 🟡 | Analyze wait chain uses the Wait Chain Traversal API; toggling UAC virtualization is not implemented (the state is shown) |
| Efficiency mode, Open file location, Search online, Properties, Go to service(s) | ✅ | |

## Services

| Feature | Status | Notes |
|---|---|---|
| Name, PID, Description, Status, Group | ✅ | Plus start type, log-on account, path, full description |
| Start, Stop, Restart, Open Services, Search online, Go to details | ✅ | Plus Pause/Resume and changing the start type |

## Beyond Task Manager

- **Process inspector**: threads, modules, memory map, handles, environment, token, network, graphs.
- **Signature verification**: embedded and catalog signatures.
- **Hardware page**: per-core clocks, caches, instruction sets, GPU telemetry, PCIe link, HAGS, memory modules, board and firmware, sensor matrix.
- **Storage**: NVMe/ATA health, temperature, partitions, volumes.
- **Network**: endpoints, connection history, adapter errors, ping and DNS.
- **Diagnostics**: rules engine with evidence, investigations, drivers, events, crashes and bugchecks, HTML report.
- **Monitoring tools**: customizable dashboard, mini monitor, CSV/JSON history export and recording, suspend/resume, process highlights, filter language.

## Not implemented (gaps)

1. Percent mode for the Disk column (Memory and Network are supported)
2. Toggling UAC virtualization per process
3. Send message UI (Users)
4. Feedback hub entry
5. Startup impact measurement (needs private boot-trace data)
6. App history before NEXUS started (SRUM is locked)
