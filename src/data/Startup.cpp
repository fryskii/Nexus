#include "core/Format.h"
#include "core/Win.h"
#include "data/SystemData.h"

#include <shlobj.h>
#include <shlwapi.h>

namespace nx {

namespace {

constexpr wchar_t kApprovedBase[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";

// Extracts the executable from a command line such as  "C:\a b\x.exe" -arg  or  C:\a b\x.exe -arg
std::wstring ExecutableFromCommand(const std::wstring& cmdIn) {
    std::wstring cmd = Trim(ExpandEnv(cmdIn));
    if (cmd.empty()) return {};
    if (cmd[0] == L'"') {
        size_t q = cmd.find(L'"', 1);
        return q == std::wstring::npos ? cmd.substr(1) : cmd.substr(1, q - 1);
    }
    // Unquoted: grow the candidate one space-separated token at a time until a file exists.
    size_t pos = 0;
    while (true) {
        size_t sp = cmd.find(L' ', pos);
        std::wstring cand = cmd.substr(0, sp);
        if (FileExists(cand)) return cand;
        if (FileExists(cand + L".exe")) return cand + L".exe";
        if (sp == std::wstring::npos) break;
        pos = sp + 1;
    }
    size_t sp = cmd.find(L' ');
    std::wstring first = cmd.substr(0, sp);
    // Bare names like "rundll32.exe" resolve via the search path.
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, first.c_str(), L".exe", MAX_PATH, found, nullptr)) return found;
    return first;
}

void ReadApproved(HKEY root, const std::wstring& sub, const std::wstring& value, StartupItem& it) {
    it.approvedRoot = root;
    it.approvedKey = std::wstring(kApprovedBase) + sub;
    it.approvedValue = value;
    RegKey k;
    std::vector<uint8_t> b;
    if (k.Open(root, it.approvedKey.c_str()) && k.GetBinary(value.c_str(), b) && b.size() >= 1) {
        it.enabled = (b[0] & 1) == 0; // 0x02/0x06 enabled, 0x03/0x07 disabled
        if (!it.enabled && b.size() >= 12) {
            FILETIME ft;
            memcpy(&ft, b.data() + 4, sizeof ft);
            it.disabledTime = FileTimeToU64(ft);
        }
    }
}

void Describe(StartupItem& it) {
    FileVersionInfo v;
    if (!it.target.empty() && ReadFileVersionInfo(it.target, v)) {
        it.publisher = v.company;
        it.description = v.description;
    }
}

void AddRunKey(std::vector<StartupItem>& out, HKEY root, const wchar_t* path, REGSAM view, const wchar_t* approvedSub, const wchar_t* locName) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &h) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384];
        DWORD nl = 16384, type = 0, dl = 0;
        LONG r = RegEnumValueW(h, i, name, &nl, nullptr, &type, nullptr, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || dl > 65536) continue;
        std::wstring data(dl / sizeof(wchar_t) + 1, L'\0');
        nl = 16384;
        if (RegEnumValueW(h, i, name, &nl, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &dl) != ERROR_SUCCESS) continue;
        data.resize(wcsnlen(data.c_str(), data.size()));
        StartupItem it;
        it.name = name;
        it.command = data;
        it.target = ExecutableFromCommand(data);
        it.location = locName;
        it.sourceKind = L"Registry";
        it.machineWide = root == HKEY_LOCAL_MACHINE;
        ReadApproved(root, approvedSub, name, it);
        Describe(it);
        out.push_back(std::move(it));
    }
    RegCloseKey(h);
}

std::wstring ResolveShortcut(const std::wstring& lnk, std::wstring& args) {
    std::wstring target;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl)))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
                wchar_t p[MAX_PATH * 2] = {};
                if (SUCCEEDED(sl->GetPath(p, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) target = ExpandEnv(p);
                wchar_t a[2048] = {};
                if (SUCCEEDED(sl->GetArguments(a, 2048))) args = a;
            }
            pf->Release();
        }
        sl->Release();
    }
    return target;
}

void AddFolder(std::vector<StartupItem>& out, REFKNOWNFOLDERID id, HKEY approvedRoot, const wchar_t* locName) {
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) return;
    std::wstring d = dir;
    CoTaskMemFree(dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        std::wstring fn = fd.cFileName;
        if (_wcsicmp(fn.c_str(), L"desktop.ini") == 0) continue;
        StartupItem it;
        std::wstring full = d + L"\\" + fn;
        it.name = fn;
        size_t dot = it.name.rfind(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : ToLower(it.name.substr(dot));
        if (ext == L".lnk") {
            it.name.resize(dot);
            std::wstring args;
            it.target = ResolveShortcut(full, args);
            it.command = L"\"" + it.target + L"\"" + (args.empty() ? L"" : L" " + args);
        } else {
            it.target = full;
            it.command = full;
        }
        it.location = locName;
        it.sourceKind = L"Startup folder";
        it.machineWide = approvedRoot == HKEY_LOCAL_MACHINE;
        ReadApproved(approvedRoot, L"StartupFolder", fn, it);
        Describe(it);
        out.push_back(std::move(it));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Packaged-app startup tasks (read-only: their state is managed by the app package API).
void AddPackagedTasks(std::vector<StartupItem>& out) {
    RegKey root;
    const wchar_t* base = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\SystemAppData";
    if (!root.Open(HKEY_CURRENT_USER, base)) return;
    for (auto& pkg : root.SubKeys()) {
        RegKey pk;
        if (!pk.Open(HKEY_CURRENT_USER, (std::wstring(base) + L"\\" + pkg).c_str())) continue;
        for (auto& task : pk.SubKeys()) {
            RegKey tk;
            DWORD state = 0;
            if (!tk.Open(HKEY_CURRENT_USER, (std::wstring(base) + L"\\" + pkg + L"\\" + task).c_str()) || !tk.GetDword(L"State", state)) continue;
            StartupItem it;
            size_t us = pkg.find(L'_');
            it.name = us == std::wstring::npos ? pkg : pkg.substr(0, us);
            size_t dot = it.name.find(L'.');
            if (dot != std::wstring::npos && dot + 1 < it.name.size()) it.name = it.name.substr(dot + 1);
            it.command = pkg + L"!" + task;
            it.location = L"Packaged app";
            it.sourceKind = L"Packaged app";
            it.enabled = state == 2 || state == 4; // Enabled / EnabledByPolicy
            it.editable = false;
            out.push_back(std::move(it));
        }
    }
}

} // namespace

std::vector<StartupItem> QueryStartupItems() {
    std::vector<StartupItem> out;
    const wchar_t* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    AddRunKey(out, HKEY_CURRENT_USER, run, 0, L"Run", L"HKCU\\...\\Run");
    AddRunKey(out, HKEY_LOCAL_MACHINE, run, KEY_WOW64_64KEY, L"Run", L"HKLM\\...\\Run");
    AddRunKey(out, HKEY_LOCAL_MACHINE, run, KEY_WOW64_32KEY, L"Run32", L"HKLM\\...\\Run (32-bit)");
    AddFolder(out, FOLDERID_Startup, HKEY_CURRENT_USER, L"Startup folder (user)");
    AddFolder(out, FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE, L"Startup folder (all users)");
    AddPackagedTasks(out);
    return out;
}

DWORD SetStartupItemEnabled(HKEY root, const std::wstring& key, const std::wstring& value, bool enable) {
    HKEY h = nullptr;
    LONG r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return static_cast<DWORD>(r);
    uint8_t data[12] = {};
    data[0] = enable ? 0x02 : 0x03;
    if (!enable) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        memcpy(data + 4, &ft, sizeof ft);
    }
    r = RegSetValueExW(h, value.c_str(), 0, REG_BINARY, data, sizeof data);
    RegCloseKey(h);
    return static_cast<DWORD>(r);
}

} // namespace nx
