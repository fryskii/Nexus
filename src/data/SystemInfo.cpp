#include "core/Format.h"
#include "core/Win.h"
#include "data/SystemData.h"

#include <algorithm>

namespace nx {

std::vector<DriverInfo> QueryDrivers() {
    std::vector<DriverInfo> out;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return out;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<uint8_t> buf(needed + 16384);
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL, buf.data(), static_cast<DWORD>(buf.size()), &needed, &count,
                              &resume, nullptr)) {
        auto* e = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        wchar_t sysRoot[MAX_PATH];
        GetWindowsDirectoryW(sysRoot, MAX_PATH);
        for (DWORD i = 0; i < count; ++i) {
            DriverInfo d;
            d.name = e[i].lpServiceName;
            d.displayName = e[i].lpDisplayName;
            d.state = e[i].ServiceStatusProcess.dwCurrentState;
            d.kernelFileSystem = e[i].ServiceStatusProcess.dwServiceType == SERVICE_FILE_SYSTEM_DRIVER;
            SC_HANDLE s = OpenServiceW(scm, d.name.c_str(), SERVICE_QUERY_CONFIG);
            if (s) {
                DWORD n = 0;
                QueryServiceConfigW(s, nullptr, 0, &n);
                if (n && n < 65536) {
                    std::vector<uint8_t> cb(n);
                    auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cb.data());
                    if (QueryServiceConfigW(s, c, n, &n)) {
                        d.startType = c->dwStartType;
                        std::wstring p = c->lpBinaryPathName ? c->lpBinaryPathName : L"";
                        // Normalise "\SystemRoot\..." and "System32\..." forms.
                        if (p.rfind(L"\\SystemRoot\\", 0) == 0) p = std::wstring(sysRoot) + p.substr(11);
                        else if (_wcsnicmp(p.c_str(), L"System32\\", 9) == 0) p = std::wstring(sysRoot) + L"\\" + p;
                        else if (p.rfind(L"\\??\\", 0) == 0) p = p.substr(4);
                        d.path = p;
                    }
                }
                CloseServiceHandle(s);
            }
            if (d.state == SERVICE_RUNNING && !d.path.empty()) {
                FileVersionInfo v;
                if (ReadFileVersionInfo(d.path, v)) {
                    d.description = v.description;
                    d.company = v.company;
                    d.version = v.version;
                }
            }
            out.push_back(std::move(d));
        }
    }
    CloseServiceHandle(scm);
    std::sort(out.begin(), out.end(), [](const DriverInfo& a, const DriverInfo& b) {
        if ((a.state == SERVICE_RUNNING) != (b.state == SERVICE_RUNNING)) return a.state == SERVICE_RUNNING;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

} // namespace nx
