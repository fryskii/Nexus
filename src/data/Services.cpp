#include "core/Win.h"
#include "data/SystemData.h"

#include <shlwapi.h>

#include <algorithm>

namespace nx {

namespace {
struct ServiceTriggerInfo { DWORD cTriggers; PVOID pTriggers; PBYTE pReserved; }; // SERVICE_TRIGGER_INFO

struct Scm {
    SC_HANDLE h = nullptr;
    explicit Scm(SC_HANDLE x) : h(x) {}
    ~Scm() { if (h) CloseServiceHandle(h); }
    Scm(const Scm&) = delete;
    Scm& operator=(const Scm&) = delete;
};

void LoadConfig(SC_HANDLE scm, ServiceInfo& s) {
    Scm svc(OpenServiceW(scm, s.name.c_str(), SERVICE_QUERY_CONFIG));
    if (!svc.h) return;
    DWORD needed = 0;
    QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (needed && needed < 65536) {
        std::vector<uint8_t> b(needed);
        auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(b.data());
        if (QueryServiceConfigW(svc.h, c, needed, &needed)) {
            s.startType = c->dwStartType;
            s.binaryPath = c->lpBinaryPathName ? c->lpBinaryPathName : L"";
            s.group = c->lpLoadOrderGroup ? c->lpLoadOrderGroup : L"";
            s.account = c->lpServiceStartName ? c->lpServiceStartName : L"";
        }
    }
    // svchost groups ("-k netsvcs") are what Task Manager shows in its "Group" column.
    size_t k = s.binaryPath.find(L" -k ");
    if (k != std::wstring::npos) {
        std::wstring g = s.binaryPath.substr(k + 4);
        size_t sp = g.find(L' ');
        s.group = sp == std::wstring::npos ? g : g.substr(0, sp);
    }
    needed = 0;
    QueryServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &needed);
    if (needed && needed < 65536) {
        std::vector<uint8_t> b(needed);
        if (QueryServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, b.data(), needed, &needed)) {
            auto* d = reinterpret_cast<SERVICE_DESCRIPTIONW*>(b.data());
            if (d->lpDescription) {
                s.description = d->lpDescription;
                if (!s.description.empty() && s.description[0] == L'@') {
                    wchar_t out[1024];
                    if (SUCCEEDED(SHLoadIndirectString(s.description.c_str(), out, 1024, nullptr))) s.description = out;
                }
            }
        }
    }
    SERVICE_DELAYED_AUTO_START_INFO da{};
    if (QueryServiceConfig2W(svc.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<BYTE*>(&da), sizeof da, &needed))
        s.delayedAuto = da.fDelayedAutostart != FALSE;
    needed = 0;
    QueryServiceConfig2W(svc.h, SERVICE_CONFIG_TRIGGER_INFO, nullptr, 0, &needed);
    if (needed >= sizeof(ServiceTriggerInfo) && needed < 65536) {
        std::vector<uint8_t> b(needed);
        if (QueryServiceConfig2W(svc.h, SERVICE_CONFIG_TRIGGER_INFO, b.data(), needed, &needed))
            s.triggerStart = reinterpret_cast<ServiceTriggerInfo*>(b.data())->cTriggers > 0;
    }
    s.configLoaded = true;
}
} // namespace

std::vector<ServiceInfo> QueryServices(bool withConfig) {
    std::vector<ServiceInfo> out;
    Scm scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm.h) return out;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<uint8_t> buf(needed + 16384);
    resume = 0;
    if (!EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), static_cast<DWORD>(buf.size()), &needed,
                               &count, &resume, nullptr))
        return out;
    auto* e = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ServiceInfo s;
        s.name = e[i].lpServiceName;
        s.displayName = e[i].lpDisplayName;
        s.state = e[i].ServiceStatusProcess.dwCurrentState;
        s.pid = e[i].ServiceStatusProcess.dwProcessId;
        s.type = e[i].ServiceStatusProcess.dwServiceType;
        s.controlsAccepted = e[i].ServiceStatusProcess.dwControlsAccepted;
        if (withConfig) LoadConfig(scm.h, s);
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const ServiceInfo& a, const ServiceInfo& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

const wchar_t* ServiceStateName(uint32_t s) {
    switch (s) {
    case SERVICE_STOPPED: return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_CONTINUE_PENDING: return L"Continuing";
    case SERVICE_PAUSE_PENDING: return L"Pausing";
    case SERVICE_PAUSED: return L"Paused";
    default: return L"Unknown";
    }
}

std::wstring ServiceStartTypeName(const ServiceInfo& s) {
    if (!s.configLoaded) return L"";
    std::wstring t;
    switch (s.startType) {
    case SERVICE_BOOT_START: t = L"Boot"; break;
    case SERVICE_SYSTEM_START: t = L"System"; break;
    case SERVICE_AUTO_START: t = s.delayedAuto ? L"Automatic (Delayed Start)" : L"Automatic"; break;
    case SERVICE_DEMAND_START: t = L"Manual"; break;
    case SERVICE_DISABLED: t = L"Disabled"; break;
    default: t = L"Unknown";
    }
    if (s.triggerStart && s.startType != SERVICE_DISABLED) t += L" (Trigger Start)";
    return t;
}

namespace {
DWORD WaitFor(SC_HANDLE svc, DWORD target, std::wstring& msg) {
    SERVICE_STATUS_PROCESS sp{};
    DWORD needed = 0;
    ULONGLONG start = GetTickCount64();
    while (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof sp, &needed)) {
        if (sp.dwCurrentState == target) return ERROR_SUCCESS;
        if (sp.dwCurrentState == SERVICE_STOPPED && target != SERVICE_STOPPED) {
            msg = L"The service stopped unexpectedly (exit code " + std::to_wstring(sp.dwWin32ExitCode) + L").";
            return sp.dwWin32ExitCode ? sp.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
        }
        if (GetTickCount64() - start > 30000) {
            msg = L"Timed out waiting for the service to change state.";
            return ERROR_SERVICE_REQUEST_TIMEOUT;
        }
        Sleep(200);
    }
    return GetLastError();
}
} // namespace

DWORD ControlServiceAction(const std::wstring& name, ServiceAction action, std::wstring& msg) {
    Scm scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return GetLastError();
    Scm svc(OpenServiceW(scm.h, name.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_QUERY_STATUS));
    if (!svc.h) return GetLastError();
    SERVICE_STATUS st{};
    auto stop = [&]() -> DWORD {
        if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &st)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_NOT_ACTIVE) return e;
            return ERROR_SUCCESS;
        }
        return WaitFor(svc.h, SERVICE_STOPPED, msg);
    };
    auto start = [&]() -> DWORD {
        if (!StartServiceW(svc.h, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING) return e;
            return ERROR_SUCCESS;
        }
        return WaitFor(svc.h, SERVICE_RUNNING, msg);
    };
    switch (action) {
    case ServiceAction::Start: return start();
    case ServiceAction::Stop: return stop();
    case ServiceAction::Restart: {
        DWORD e = stop();
        return e ? e : start();
    }
    case ServiceAction::Pause:
        if (!ControlService(svc.h, SERVICE_CONTROL_PAUSE, &st)) return GetLastError();
        return WaitFor(svc.h, SERVICE_PAUSED, msg);
    case ServiceAction::Continue:
        if (!ControlService(svc.h, SERVICE_CONTROL_CONTINUE, &st)) return GetLastError();
        return WaitFor(svc.h, SERVICE_RUNNING, msg);
    }
    return ERROR_INVALID_PARAMETER;
}

DWORD SetServiceStartType(const std::wstring& name, uint32_t startType, bool delayed) {
    Scm scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return GetLastError();
    Scm svc(OpenServiceW(scm.h, name.c_str(), SERVICE_CHANGE_CONFIG));
    if (!svc.h) return GetLastError();
    if (!ChangeServiceConfigW(svc.h, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
        return GetLastError();
    if (startType == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO da{delayed ? TRUE : FALSE};
        ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &da);
    }
    return ERROR_SUCCESS;
}

} // namespace nx
