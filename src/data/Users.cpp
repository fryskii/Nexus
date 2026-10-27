#include "core/Win.h"
#include "data/SystemData.h"

#include <wtsapi32.h>

namespace nx {

namespace {
std::wstring QueryStr(DWORD id, WTS_INFO_CLASS cls) {
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    std::wstring s;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, cls, &buf, &bytes) && buf) s = buf;
    if (buf) WTSFreeMemory(buf);
    return s;
}
const wchar_t* StateName(WTS_CONNECTSTATE_CLASS s) {
    switch (s) {
    case WTSActive: return L"Active";
    case WTSConnected: return L"Connected";
    case WTSConnectQuery: return L"Connecting";
    case WTSShadow: return L"Shadowing";
    case WTSDisconnected: return L"Disconnected";
    case WTSIdle: return L"Idle";
    case WTSListen: return L"Listening";
    case WTSReset: return L"Reset";
    case WTSDown: return L"Down";
    case WTSInit: return L"Initializing";
    default: return L"Unknown";
    }
}
} // namespace

std::vector<SessionInfo> QuerySessions() {
    std::vector<SessionInfo> out;
    PWTS_SESSION_INFOW list = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) return out;
    DWORD self = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &self);
    for (DWORD i = 0; i < count; ++i) {
        SessionInfo s;
        s.id = list[i].SessionId;
        s.station = list[i].pWinStationName ? list[i].pWinStationName : L"";
        s.state = StateName(list[i].State);
        s.user = QueryStr(s.id, WTSUserName);
        if (s.user.empty()) continue; // services session / listeners
        s.domain = QueryStr(s.id, WTSDomainName);
        s.client = QueryStr(s.id, WTSClientName);
        s.current = s.id == self;
        LPWSTR buf = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.id, WTSSessionInfo, &buf, &bytes) && buf && bytes >= sizeof(WTSINFOW)) {
            auto* info = reinterpret_cast<WTSINFOW*>(buf);
            s.logonTime = static_cast<uint64_t>(info->LogonTime.QuadPart);
        }
        if (buf) WTSFreeMemory(buf);
        out.push_back(std::move(s));
    }
    WTSFreeMemory(list);
    return out;
}

DWORD DisconnectUserSession(uint32_t id) {
    return WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, id, TRUE) ? ERROR_SUCCESS : GetLastError();
}

DWORD LogoffUserSession(uint32_t id) {
    return WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE) ? ERROR_SUCCESS : GetLastError();
}

DWORD SendSessionMessage(uint32_t id, const std::wstring& title, const std::wstring& text) {
    DWORD resp = 0;
    return WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, id, const_cast<LPWSTR>(title.c_str()), static_cast<DWORD>(title.size() * sizeof(wchar_t)),
                           const_cast<LPWSTR>(text.c_str()), static_cast<DWORD>(text.size() * sizeof(wchar_t)), MB_OK | MB_ICONINFORMATION, 0,
                           &resp, FALSE)
               ? ERROR_SUCCESS
               : GetLastError();
}

} // namespace nx
