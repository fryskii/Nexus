#include "core/Format.h"
#include "core/Win.h"
#include "data/Processes.h"

#include <sddl.h>

#include <mutex>
#include <unordered_map>

namespace nx {

namespace {

// Caches are only touched from the collector thread, but a mutex keeps them safe if the
// details window ever calls AcquireProcMeta from a worker.
std::mutex g_cacheLock;
std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> g_sidCache; // sid -> (DOMAIN\user, user)
std::unordered_map<std::wstring, std::shared_ptr<FileVersionInfo>> g_versionCache;

std::shared_ptr<FileVersionInfo> VersionFor(const std::wstring& path) {
    std::wstring key = ToLower(path);
    {
        std::lock_guard<std::mutex> l(g_cacheLock);
        auto it = g_versionCache.find(key);
        if (it != g_versionCache.end()) return it->second;
    }
    auto v = std::make_shared<FileVersionInfo>();
    ReadFileVersionInfo(path, *v);
    std::lock_guard<std::mutex> l(g_cacheLock);
    if (g_versionCache.size() > 4096) g_versionCache.clear(); // bounded
    g_versionCache[key] = v;
    return v;
}

void LookupUser(PSID sid, std::wstring& full, std::wstring& shortName) {
    LPWSTR sidStr = nullptr;
    if (!ConvertSidToStringSidW(sid, &sidStr)) return;
    std::wstring key = sidStr;
    LocalFree(sidStr);
    {
        std::lock_guard<std::mutex> l(g_cacheLock);
        auto it = g_sidCache.find(key);
        if (it != g_sidCache.end()) { full = it->second.first; shortName = it->second.second; return; }
    }
    wchar_t name[256], domain[256];
    DWORD nl = 256, dl = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, name, &nl, domain, &dl, &use)) {
        shortName = name;
        full = dl ? std::wstring(domain) + L"\\" + name : std::wstring(name);
    } else {
        full = shortName = key;
    }
    std::lock_guard<std::mutex> l(g_cacheLock);
    g_sidCache[key] = {full, shortName};
}

template <typename T>
bool TokenInfo(HANDLE tok, TOKEN_INFORMATION_CLASS cls, std::vector<uint8_t>& buf, T*& out) {
    DWORD len = 0;
    GetTokenInformation(tok, cls, nullptr, 0, &len);
    if (!len || len > 65536) return false;
    buf.resize(len);
    if (!GetTokenInformation(tok, cls, buf.data(), len, &len)) return false;
    out = reinterpret_cast<T*>(buf.data());
    return true;
}

using IsWow64Process2_t = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
using GetPackageFullName_t = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);

struct DynApis {
    IsWow64Process2_t isWow64Process2 = nullptr;
    GetPackageFullName_t getPackageFullName = nullptr;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_AMD64;
};
const DynApis& Dyn() {
    static DynApis d = [] {
        DynApis x;
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        x.isWow64Process2 = reinterpret_cast<IsWow64Process2_t>(GetProcAddress(k, "IsWow64Process2"));
        x.getPackageFullName = reinterpret_cast<GetPackageFullName_t>(GetProcAddress(k, "GetPackageFullName"));
        if (x.isWow64Process2) {
            USHORT pm = 0, nm = 0;
            if (x.isWow64Process2(GetCurrentProcess(), &pm, &nm)) x.nativeMachine = nm;
        }
        return x;
    }();
    return d;
}

const std::wstring& WindowsDir() {
    static std::wstring w = [] {
        wchar_t b[MAX_PATH];
        UINT n = GetWindowsDirectoryW(b, MAX_PATH);
        return ToLower(std::wstring(b, n)) + L"\\";
    }();
    return w;
}

struct PROCESS_PROTECTION_LEVEL_INFORMATION_NX { DWORD ProtectionLevel; };

} // namespace

std::shared_ptr<ProcMeta> AcquireProcMeta(uint32_t pid, const std::wstring& name, uint32_t session, HANDLE h) {
    auto m = std::make_shared<ProcMeta>();
    m->name = name;
    if (pid == 0) {
        m->user = L"NT AUTHORITY\\SYSTEM";
        m->userShort = L"SYSTEM";
        m->description = L"Percentage of time the processor is idle";
        m->inWindowsDir = true;
        m->integrity = 4;
        return m;
    }
    if (pid == 4) {
        wchar_t b[MAX_PATH];
        UINT n = GetSystemDirectoryW(b, MAX_PATH);
        m->path = std::wstring(b, n) + L"\\ntoskrnl.exe";
        m->user = L"NT AUTHORITY\\SYSTEM";
        m->userShort = L"SYSTEM";
        m->description = L"NT Kernel & System";
        m->company = L"Microsoft Corporation";
        m->inWindowsDir = true;
        m->integrity = 4;
        m->isCritical = true;
        m->machine = Dyn().nativeMachine;
        return m;
    }
    if (!h) {
        m->accessDenied = true;
        // Minimal processes (Registry, Memory Compression, Secure System) have no image file.
        if (name == L"Registry" || name == L"Memory Compression" || name == L"Secure System" || name == L"vmmem") {
            m->user = L"NT AUTHORITY\\SYSTEM";
            m->userShort = L"SYSTEM";
            m->inWindowsDir = true;
        }
        return m;
    }

    wchar_t pathBuf[32768];
    DWORD len = 32768;
    if (QueryFullProcessImageNameW(h, 0, pathBuf, &len)) m->path.assign(pathBuf, len);
    m->inWindowsDir = !m->path.empty() && ToLower(m->path).rfind(WindowsDir(), 0) == 0;

    // Command line (Windows 8.1+; requires only PROCESS_QUERY_LIMITED_INFORMATION).
    if (auto q = nt::Get().QueryInformationProcess) {
        std::vector<uint8_t> buf(4096);
        ULONG ret = 0;
        nt::NTSTATUS st = q(h, nt::ProcessCommandLineInformation, buf.data(), static_cast<ULONG>(buf.size()), &ret);
        if ((st == nt::STATUS_INFO_LENGTH_MISMATCH || st == nt::STATUS_BUFFER_OVERFLOW || st == nt::STATUS_BUFFER_TOO_SMALL) && ret > buf.size() &&
            ret < 1 << 20) {
            buf.resize(ret);
            st = q(h, nt::ProcessCommandLineInformation, buf.data(), static_cast<ULONG>(buf.size()), &ret);
        }
        if (nt::NT_OK(st)) {
            auto* us = reinterpret_cast<nt::UNICODE_STRING*>(buf.data());
            auto* base = reinterpret_cast<uint8_t*>(us->Buffer);
            if (us->Buffer && base >= buf.data() && base + us->Length <= buf.data() + buf.size())
                m->commandLine.assign(us->Buffer, us->Length / sizeof(wchar_t));
        }
    }

    HANDLE tok = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        std::vector<uint8_t> b;
        TOKEN_USER* tu = nullptr;
        if (TokenInfo(tok, TokenUser, b, tu)) LookupUser(tu->User.Sid, m->user, m->userShort);
        TOKEN_MANDATORY_LABEL* ml = nullptr;
        if (TokenInfo(tok, TokenIntegrityLevel, b, ml)) {
            PSID sid = ml->Label.Sid;
            DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
            m->integrity = rid >= 0x5000 ? 5 : rid >= 0x4000 ? 4 : rid >= 0x3000 ? 3 : rid >= 0x2000 ? 2 : rid >= 0x1000 ? 1 : 0;
        }
        TOKEN_ELEVATION el{};
        DWORD rl = 0;
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &rl)) m->elevated = el.TokenIsElevated != 0;
        DWORD virt = 0;
        if (GetTokenInformation(tok, TokenVirtualizationEnabled, &virt, sizeof virt, &rl)) m->uacVirtualization = virt != 0;
        CloseHandle(tok);
    } else if (session == 0) {
        m->user = L"NT AUTHORITY\\SYSTEM"; // protected service processes deny token access
        m->userShort = L"SYSTEM";
    }
    m->isService = session == 0;

    if (Dyn().isWow64Process2) {
        USHORT pm = 0, nm = 0;
        if (Dyn().isWow64Process2(h, &pm, &nm)) m->machine = pm == IMAGE_FILE_MACHINE_UNKNOWN ? nm : pm;
    } else {
        BOOL wow = FALSE;
        IsWow64Process(h, &wow);
        m->machine = wow ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    }

    PROCESS_PROTECTION_LEVEL_INFORMATION_NX pl{0xFFFFFFFE};
    if (GetProcessInformation(h, static_cast<PROCESS_INFORMATION_CLASS>(7) /*ProcessProtectionLevelInfo*/, &pl, sizeof pl))
        m->isProtected = pl.ProtectionLevel != 0xFFFFFFFE;
    using IsProcessCritical_t = BOOL(WINAPI*)(HANDLE, PBOOL);
    static auto isCritical = reinterpret_cast<IsProcessCritical_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical"));
    BOOL crit = FALSE;
    if (isCritical && isCritical(h, &crit)) m->isCritical = crit != FALSE;

    if (Dyn().getPackageFullName) {
        UINT32 n = 0;
        if (Dyn().getPackageFullName(h, &n, nullptr) == ERROR_INSUFFICIENT_BUFFER && n < 1024) {
            std::wstring pkg(n, L'\0');
            if (Dyn().getPackageFullName(h, &n, pkg.data()) == ERROR_SUCCESS) {
                pkg.resize(wcsnlen(pkg.c_str(), pkg.size()));
                m->packageName = pkg;
            }
        }
    }

    if (!m->path.empty()) {
        auto v = VersionFor(m->path);
        m->description = v->description;
        m->company = v->company;
        m->fileVersion = v->version;
    }
    return m;
}

const wchar_t* IntegrityName(int level) {
    switch (level) {
    case 0: return L"Untrusted";
    case 1: return L"Low";
    case 2: return L"Medium";
    case 3: return L"High";
    case 4: return L"System";
    case 5: return L"Protected";
    default: return L"";
    }
}

const wchar_t* PriorityClassName(uint32_t cls) {
    switch (cls) {
    case IDLE_PRIORITY_CLASS: return L"Low";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
    case NORMAL_PRIORITY_CLASS: return L"Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
    case HIGH_PRIORITY_CLASS: return L"High";
    case REALTIME_PRIORITY_CLASS: return L"Realtime";
    default: return L"";
    }
}

const wchar_t* MachineName(uint16_t m) {
    switch (m) {
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_I386: return L"x86";
    case 0xAA64: return L"ARM64";
    case IMAGE_FILE_MACHINE_ARMNT: return L"ARM";
    default: return L"";
    }
}

} // namespace nx
