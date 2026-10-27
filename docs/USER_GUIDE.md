# NEXUS user guide

## Getting started

Start `NEXUS.exe` from the Start menu or the portable folder. NEXUS runs as a standard user. Some information and actions need administrator rights:
per-process network traffic, SMART data, the full partition table, other users' processes and stopping protected services.
For these, use the shield button in the title area to restart as administrator. Single privileged actions, such as starting a service or enabling a
machine-wide startup app, show a UAC prompt for that action only.

The sidebar switches between sections (Ctrl+1 … Ctrl+9). Right-click a sidebar item to hide it or make it the start page.
Ctrl+B collapses the sidebar.

## Processes

- **Grouped** (default) works like Task Manager: *Apps* are processes with a visible window, together with the processes they started. *Background
  processes* and *Windows processes* follow, and multiple instances of the same program are grouped. **Tree** shows the parent/child
  hierarchy. **Flat** lists every process.
- Column headers show system totals. Click a header to sort, drag its edge to resize, drag it to reorder, and right-click it to choose columns.
- Cell shading (heat map) shows relative load. Turn it off from the "…" menu.
- **Search** (Ctrl+F) supports plain text (name, description, publisher, PID) and these terms:
  - `pid:1234`, `ppid:4`, `name:`, `path:`, `pub:`, `user:`, `cmd:`
  - comparisons: `cpu>5`, `mem>500MB`, `gpu>10`, `io>1MB`, `threads>100`, `handles>5000`
  - regular expressions: `/chrom(e|ium)/` or `re:pattern`
  - negation: `!svchost`. All terms must match.
- Actions are in the toolbar, the context menu (right-click or Shift+F10) and on keys:
  - End task (Del) and end process tree
  - Efficiency mode
  - Suspend / resume
  - Set priority and affinity
  - Create a full dump or minidump
  - Go to details or services
  - Open file location, search online, copy fields, Properties
  - Inspect (Enter)
- Destructive actions ask for confirmation. NEXUS refuses to end or suspend critical Windows processes, because Windows would stop with a bugcheck.
- *Power usage* is a NEXUS estimate based on CPU, GPU, I/O and network activity. It is not a wattage.

## Process inspector

Double-click a process, or choose *Inspect*, to open a window with these tabs:
- **General**: image, signature, command line, working directory, parent, user, integrity, mitigations.
- **Performance**: history graphs.
- **Threads**: start module and wait reason.
- **Modules**.
- **Memory**: virtual memory map.
- **Handles**: with names where Windows allows.
- **Environment**.
- **Security**: token groups and privileges.
- **Network**: connections.

F5 refreshes the current tab, Ctrl+Tab switches tabs and Esc closes the window.

## Performance

Select a device on the left. Right-click the detail area to switch between overall and per-logical-processor CPU graphs, show kernel time,
use the summary view, copy the statistics (Ctrl+C) or export history. On graphs:
- hover to read values,
- Ctrl+wheel to zoom,
- drag to pan back in time,
- double-click to reset.

On the GPU view, click an engine name to choose which engine each graph shows.

## Other sections

- **App history** totals resource usage per application while NEXUS runs. Enable *Keep across restarts* to persist it.
- **Startup apps**: Enable/Disable (or Space) uses the same mechanism as Task Manager, so the two tools agree. Packaged-app tasks are listed
  read-only.
- **Users**: expand a user to see their processes. Disconnect and sign off ask for confirmation.
- **Details**: the full column set. Use *Select columns* or right-click a header.
- **Services**: control services and their start type. *Disabled* asks for confirmation.
- **Hardware**: the *Sensors* tab lists every sensor and whether it is available, unsupported, needs a driver or needs administrator rights.
- **Network**:
  - *Apps* shows per-process throughput (administrator required).
  - *Connections* lists TCP/UDP endpoints; enable host-name resolution if you want it.
  - *History* lists connections seen while NEXUS runs.
  - *Adapters* shows counters and errors.
  - *Tools* has ping and DNS lookup.
- **Storage**: drive health from the NVMe health log or ATA SMART, temperature, partitions and volumes.
- **System diagnostics**:
  - *Health* lists findings with the measured evidence and the reasoning behind them.
  - *Investigate* lists the top consumers per resource.
  - *System* shows Secure Boot, TPM, VBS and other platform details.
  - *Drivers*, *Events* and *Crashes* list driver inventory, event-log errors and crash history.
  - Export an HTML report with Ctrl+E.

## Mini monitor and tray

Ctrl+M toggles a small always-on-top monitor. Drag it anywhere, double-click it to open NEXUS, and right-click it for opacity options. The notification-area icon
shows CPU (wide bar) and memory (narrow bar). Click it to open NEXUS and right-click it for a menu.

## Settings

All preferences are in Settings and are stored in `%APPDATA%\NEXUS\nexus.ini`, or next to the exe in portable mode. History recording to disk is
**off** by default. When it is on, it writes daily CSV files to `%LOCALAPPDATA%\NEXUS\History`, at most once a minute, and deletes them after the retention period.

**Replace Task Manager** makes Ctrl+Shift+Esc open NEXUS through the documented Image File Execution Options setting. Turn it off in Settings or
uninstall NEXUS to restore Task Manager.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+1…9 / Ctrl+0 / Ctrl+, | Sections / Overview / Settings |
| Ctrl+F, F3, Esc | Search / clear search |
| Del, Enter, Shift+F10 | End task, inspect, context menu |
| ← → | Collapse / expand group |
| Ctrl+N | Run new task |
| F5, Ctrl+P | Refresh now, pause/resume |
| Ctrl+T, Ctrl+M | Always on top, mini monitor |
| Ctrl+E | Export diagnostic report |
| Ctrl + / Ctrl - | UI scale |
| Ctrl+B | Collapse sidebar |

## Command line

```
NEXUS.exe [--minimized] [--page N]
NEXUS.exe --bench [out.json] [iterations]   # headless collection benchmark
NEXUS.exe --version
```

## Uninstall

Use *Settings › Apps* or the Start-menu uninstaller. The uninstaller removes the Task Manager replacement, but only if it points at NEXUS, and the per-user
autostart entry. It can optionally remove your settings and recordings.
