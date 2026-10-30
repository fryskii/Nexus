#pragma once
// Services, startup entries, user sessions, event log and driver inventory.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nx {

// ---- Services ----
struct ServiceInfo {
    std::wstring name, displayName, description, group, binaryPath, account;
    uint32_t state = 0, startType = 0, pid = 0, type = 0, controlsAccepted = 0;
    bool delayedAuto = false, triggerStart = false, configLoaded = false;
};
std::vector<ServiceInfo> QueryServices(bool withConfig);
const wchar_t* ServiceStateName(uint32_t state);
std::wstring ServiceStartTypeName(const ServiceInfo& s);
enum class ServiceAction { Start, Stop, Restart, Pause, Continue };
// Waits (up to ~30 s) for the transition. Returns ERROR_SUCCESS or a Win32 error code.
DWORD ControlServiceAction(const std::wstring& name, ServiceAction action, std::wstring& message);
DWORD SetServiceStartType(const std::wstring& name, uint32_t startType, bool delayed);

// ---- Startup apps ----
struct StartupItem {
    std::wstring name, command, target, publisher, description, location, sourceKind;
    bool enabled = true, machineWide = false, editable = true, running = false;
    uint64_t disabledTime = 0;      // FILETIME when disabled (from StartupApproved)
    // Where the item and its StartupApproved state live:
    HKEY approvedRoot = nullptr;
    std::wstring approvedKey, approvedValue;
};
std::vector<StartupItem> QueryStartupItems();
// Writes the StartupApproved state exactly as Task Manager does. HKLM entries need elevation.
DWORD SetStartupItemEnabled(HKEY root, const std::wstring& key, const std::wstring& value, bool enable);

// ---- Sessions ----
struct SessionInfo {
    uint32_t id = 0;
    std::wstring user, domain, station, state, client;
    uint64_t logonTime = 0;
    bool current = false;
};
std::vector<SessionInfo> QuerySessions();
DWORD DisconnectUserSession(uint32_t id);
DWORD LogoffUserSession(uint32_t id);
DWORD SendSessionMessage(uint32_t id, const std::wstring& title, const std::wstring& text);

// ---- Event log ----
struct EventEntry {
    uint64_t time = 0;  // FILETIME UTC
    uint32_t id = 0;
    uint8_t level = 0;  // 1 critical, 2 error, 3 warning, 4 info
    std::wstring channel, provider, message;
    std::vector<std::wstring> data; // EventData values in order
};
std::vector<EventEntry> QueryEvents(const wchar_t* channel, const wchar_t* xpath, size_t max, bool formatMessage);
// Calls fn with each matching event, newest first, with only the provider, ID, level and time filled in.
// Stops when fn returns false. False if the log could not be read.
bool ScanEvents(const wchar_t* channel, const wchar_t* xpath, const std::function<bool(const EventEntry&)>& fn);
const wchar_t* EventLevelName(uint8_t level);

// ---- Drivers ----
struct DriverInfo {
    std::wstring name, displayName, path, description, company, version;
    uint32_t state = 0, startType = 0;
    bool kernelFileSystem = false;
};
std::vector<DriverInfo> QueryDrivers();

} // namespace nx
