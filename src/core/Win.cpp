#include "core/Win.h"

#include <aclapi.h>
#include <sddl.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cwchar>

namespace nx {

bool RegKey::GetString(const wchar_t* name, std::wstring& out) const {
    if (!k_) return false;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return false;
    if (size > 1 << 20) return false;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(k_, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS) return false;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    out = type == REG_EXPAND_SZ ? ExpandEnv(buf) : buf;
    return true;
}

bool RegKey::GetDword(const wchar_t* name, DWORD& out) const {
    if (!k_) return false;
    DWORD type = 0, size = sizeof(DWORD);
    return RegQueryValueExW(k_, name, nullptr, &type, reinterpret_cast<BYTE*>(&out), &size) == ERROR_SUCCESS && type == REG_DWORD;
}

bool RegKey::GetQword(const wchar_t* name, uint64_t& out) const {
    if (!k_) return false;
    DWORD type = 0, size = sizeof(uint64_t);
    return RegQueryValueExW(k_, name, nullptr, &type, reinterpret_cast<BYTE*>(&out), &size) == ERROR_SUCCESS && type == REG_QWORD;
}

bool RegKey::GetBinary(const wchar_t* name, std::vector<uint8_t>& out) const {
    if (!k_) return false;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size > 1 << 20) return false;
    out.resize(size);
    return RegQueryValueExW(k_, name, nullptr, &type, out.data(), &size) == ERROR_SUCCESS;
}

std::vector<std::wstring> RegKey::SubKeys() const {
    std::vector<std::wstring> out;
    if (!k_) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        LONG r = RegEnumKeyExW(k_, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS) out.emplace_back(name, len);
    }
    return out;
}

std::vector<std::wstring> RegKey::ValueNames() const {
    std::vector<std::wstring> out;
    if (!k_) return out;
    wchar_t name[16384];
    for (DWORD i = 0;; ++i) {
        DWORD len = 16384;
        LONG r = RegEnumValueW(k_, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS) out.emplace_back(name, len);
    }
    return out;
}

std::wstring LastErrorMessage(DWORD err) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                             reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = n && buf ? std::wstring(buf, n) : L"Error " + std::to_wstring(err);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    out.resize(n - 1);
    return out;
}

std::wstring GetModuleDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    return DirOf(p);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}


namespace {
bool IsTrustedSid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid)) return true;
    // NT SERVICE\TrustedInstaller
    PSID ti = nullptr;
    bool trusted = false;
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &ti)) {
        trusted = EqualSid(sid, ti) != FALSE;
        LocalFree(ti);
    }
    return trusted;
}

// Checks owner and DACL of one file-system object; `dangerous` are the rights no untrusted SID may hold.
bool CheckObject(const std::wstring& path, ACCESS_MASK dangerous, std::wstring* why) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl,
                              nullptr, &sd) != ERROR_SUCCESS) {
        if (why) *why = L"Cannot read the permissions of " + path;
        return false;
    }
    bool ok = true;
    if (!IsTrustedSid(owner)) {
        ok = false;
        if (why) *why = path + L" is owned by a non-administrator account";
    } else if (!dacl) {
        ok = false; // NULL DACL: everyone has full access
        if (why) *why = path + L" has no access restrictions";
    } else {
        GENERIC_MAPPING map{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
            ACE_HEADER* ace = nullptr;
            if (!GetAce(dacl, i, reinterpret_cast<void**>(&ace))) { ok = false; break; }
            if (ace->AceFlags & INHERIT_ONLY_ACE) continue; // applies to children only
            if (ace->AceType == ACCESS_DENIED_ACE_TYPE || ace->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE) continue;
            if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; } // unexpected ACE type: be conservative
            auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
            ACCESS_MASK mask = allowed->Mask;
            MapGenericMask(&mask, &map);
            if ((mask & dangerous) && !IsTrustedSid(reinterpret_cast<PSID>(&allowed->SidStart))) ok = false;
        }
        if (!ok && why && why->empty()) *why = path + L" can be modified by non-administrator accounts";
    }
    LocalFree(sd);
    return ok;
}
} // namespace

bool IsProtectedFromNonAdmins(const std::wstring& pathIn, std::wstring* why) {
    // Resolve junctions and symbolic links so the real location is checked.
    HANDLE h = CreateFileW(pathIn.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = L"Cannot open " + pathIn;
        return false;
    }
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= std::size(buf)) {
        if (why) *why = L"Cannot resolve " + pathIn;
        return false;
    }
    std::wstring path(buf, n);
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        if (why) *why = L"NEXUS is on a network share";
        return false;
    }
    if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
    const ACCESS_MASK modify = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
    const ACCESS_MASK replaceChild = WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD | DELETE;
    if (!CheckObject(path, modify, why)) return false;
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 2) { if (why) *why = L"Unexpected path " + path; return false; }
    std::wstring dir = path.substr(0, slash);
    if (!CheckObject(dir, modify, why)) return false; // no planting or replacing files next to NEXUS
    // Parents: creating new entries is harmless (C:\ allows it), deleting or renaming existing ones is not.
    for (;;) {
        size_t s = dir.find_last_of(L'\\');
        if (s == std::wstring::npos) break;
        dir = dir.substr(0, s);
        std::wstring check = dir.size() == 2 ? dir + L"\\" : dir; // "C:" -> "C:\"
        if (!CheckObject(check, replaceChild, why)) return false;
        if (dir.size() <= 2) break;
    }
    return true;
}

bool IsElevated() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    HANDLE tok = nullptr;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    bool r = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) && GetTokenInformation(tok, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
    if (tok) CloseHandle(tok);
    cached = r ? 1 : 0;
    return r;
}

uint32_t WindowsBuild() {
    static uint32_t build = [] {
        OSVERSIONINFOEXW vi{sizeof vi};
        return nt::Get().GetVersion && nt::Get().GetVersion(&vi) == 0 ? static_cast<uint32_t>(vi.dwBuildNumber) : 0u;
    }();
    return build;
}

bool IsRunningUnderWine() {
    static bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    return wine;
}

bool IsWindowsDarkMode() {
    RegKey k;
    DWORD v = 1;
    if (k.Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize")) k.GetDword(L"AppsUseLightTheme", v);
    return v == 0;
}

DWORD GetWindowsAccentColor() {
    RegKey k;
    DWORD v = 0;
    if (k.Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM") && k.GetDword(L"AccentColor", v)) return v & 0x00FFFFFF; // AABBGGRR
    return 0;
}

uint64_t FileTimeToU64(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToU64(ft);
}

uint64_t QpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

double QpcToSeconds(uint64_t ticks) {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return static_cast<double>(ticks) / static_cast<double>(f.QuadPart);
}

bool ReadFileVersionInfo(const std::wstring& path, FileVersionInfo& out) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL | FILE_VER_GET_LOCALISED, path.c_str(), &dummy);
    if (!size || size > 4 * 1024 * 1024) return false;
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL | FILE_VER_GET_LOCALISED, path.c_str(), 0, size, buf.data())) return false;
    struct LangCp { WORD lang, cp; };
    LangCp* tr = nullptr;
    UINT trLen = 0;
    std::vector<std::wstring> prefixes;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) && tr) {
        for (UINT i = 0; i < trLen / sizeof(LangCp) && i < 4; ++i) {
            wchar_t p[64];
            swprintf(p, 64, L"\\StringFileInfo\\%04x%04x\\", tr[i].lang, tr[i].cp);
            prefixes.push_back(p);
        }
    }
    prefixes.push_back(L"\\StringFileInfo\\040904B0\\");
    prefixes.push_back(L"\\StringFileInfo\\040904E4\\");
    auto get = [&](const wchar_t* key) -> std::wstring {
        for (auto& pre : prefixes) {
            wchar_t* val = nullptr;
            UINT len = 0;
            std::wstring q = pre + key;
            if (VerQueryValueW(buf.data(), q.c_str(), reinterpret_cast<void**>(&val), &len) && val && len > 1)
                return std::wstring(val, wcsnlen(val, len));
        }
        return {};
    };
    out.description = get(L"FileDescription");
    out.company = get(L"CompanyName");
    out.product = get(L"ProductName");
    out.copyright = get(L"LegalCopyright");
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) && ffi && ffiLen >= sizeof(VS_FIXEDFILEINFO)) {
        wchar_t v[64];
        swprintf(v, 64, L"%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                 LOWORD(ffi->dwFileVersionLS));
        out.version = v;
    }
    return true;
}

void OpenFileLocation(const std::wstring& path) {
    if (path.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    } else {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void SearchOnline(const std::wstring& term) {
    // Percent-encode the query (UTF-8).
    std::wstring url = L"https://www.bing.com/search?q=";
    int n = WideCharToMultiByte(CP_UTF8, 0, term.c_str(), static_cast<int>(term.size()), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, term.c_str(), static_cast<int>(term.size()), u.data(), n, nullptr, nullptr);
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') url.push_back(c);
        else { url.push_back(L'%'); url.push_back(hex[c >> 4]); url.push_back(hex[c & 15]); }
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        void* p = GlobalLock(h);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

bool ShellRun(HWND owner, const std::wstring& file, const std::wstring& args, bool elevated, int show) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = elevated ? L"runas" : nullptr;
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = show;
    return ShellExecuteExW(&sei) != FALSE;
}

DWORD RunSelfElevatedAndWait(HWND owner, const std::wstring& args, DWORD timeoutMs) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    // Not SW_HIDE: the helper has no main window, so Windows would apply the show state to its
    // first window, the confirmation dialog, and the user would wait on a dialog they cannot see.
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return GetLastError();
    if (!sei.hProcess) return ERROR_GEN_FAILURE;
    AllowSetForegroundWindow(GetProcessId(sei.hProcess)); // let its confirmation come to the front
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(sei.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

std::wstring NtPathToDos(const std::wstring& nt) {
    static std::vector<std::pair<std::wstring, std::wstring>> map;
    static DWORD lastBuild = 0;
    DWORD now = GetTickCount();
    if (map.empty() || now - lastBuild > 30000) {
        map.clear();
        wchar_t drives[512];
        DWORD n = GetLogicalDriveStringsW(511, drives);
        for (wchar_t* d = drives; n && *d; d += wcslen(d) + 1) {
            wchar_t dev[3] = {d[0], L':', 0};
            wchar_t target[MAX_PATH];
            if (QueryDosDeviceW(dev, target, MAX_PATH)) map.emplace_back(target, dev);
        }
        lastBuild = now;
    }
    for (auto& [devPath, letter] : map) {
        if (nt.size() > devPath.size() && _wcsnicmp(nt.c_str(), devPath.c_str(), devPath.size()) == 0 && nt[devPath.size()] == L'\\')
            return letter + nt.substr(devPath.size());
    }
    if (nt.rfind(L"\\??\\", 0) == 0) return nt.substr(4);
    if (nt.rfind(L"\\SystemRoot\\", 0) == 0) return ExpandEnv(L"%SystemRoot%") + nt.substr(11);
    return nt;
}

} // namespace nx

namespace nt {
const Api& Get() {
    static Api api = [] {
        Api a;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            a.QuerySystemInformation = reinterpret_cast<NtQuerySystemInformation_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
            a.QueryInformationProcess = reinterpret_cast<NtQueryInformationProcess_t>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
            a.QueryObject = reinterpret_cast<NtQueryObject_t>(GetProcAddress(ntdll, "NtQueryObject"));
            a.SuspendProcess = reinterpret_cast<NtSuspendProcess_t>(GetProcAddress(ntdll, "NtSuspendProcess"));
            a.ResumeProcess = reinterpret_cast<NtSuspendProcess_t>(GetProcAddress(ntdll, "NtResumeProcess"));
            a.GetVersion = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"));
        }
        return a;
    }();
    return api;
}
} // namespace nt
