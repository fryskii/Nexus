// NEXUS entry point: single-instance handling, privileged helper actions and benchmark mode.
#include "app/App.h"
#include "core/Format.h"
#include "core/Win.h"
#include "data/ProcessOps.h"
#include "data/Processes.h"
#include "data/Sampler.h"
#include "data/SystemData.h"

#include <commctrl.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace nx;

namespace {

// Privileged helper actions. These run in a short-lived elevated NEXUS process started with the
// UAC prompt, so the main UI never needs to run elevated for occasional admin tasks.
//
// Any program running as the user can start NEXUS.exe with these arguments and trigger a UAC
// prompt that shows NEXUS's name, so every helper treats its arguments as untrusted: it only
// acts on objects that exist and are of the expected kind, never takes paths or registry
// locations from the command line, and asks for confirmation in its own elevated window
// (which lower-privilege programs cannot click or fake) before anything that could weaken the
// system or re-enable software.
bool HelperConfirm(const std::wstring& title, const std::wstring& text, const std::wstring& okText) {
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.pszWindowTitle = L"NEXUS (administrator)";
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = text.c_str();
    tc.pszMainIcon = TD_SHIELD_ICON;
    TASKDIALOG_BUTTON buttons[] = {{IDOK, okText.c_str()}, {IDCANCEL, L"Cancel"}};
    tc.pButtons = buttons;
    tc.cButtons = 2;
    tc.nDefaultButton = IDCANCEL;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    // No owner window (it belongs to the non-elevated NEXUS), so bring it to the front ourselves.
    tc.pfCallback = [](HWND h, UINT n, WPARAM, LPARAM, LONG_PTR) -> HRESULT {
        if (n == TDN_CREATED) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(h);
        }
        return S_OK;
    };
    int button = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, nullptr)))
        return MessageBoxW(nullptr, (title + L"\n\n" + text).c_str(), L"NEXUS (administrator)", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) == IDOK;
    return button == IDOK;
}

// Display name of an existing service, or empty if there is no such service.
std::wstring ServiceDisplayName(const std::wstring& name) {
    if (name.empty() || name.size() > 256) return {};
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return {};
    std::wstring display;
    if (SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG)) {
        wchar_t buf[512];
        DWORD n = 512;
        display = GetServiceDisplayNameW(scm, name.c_str(), buf, &n) ? std::wstring(buf) : name;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return display;
}

// Machine-wide startup entries that Task Manager can enable or disable: the 64- and 32-bit HKLM Run
// keys and the all-users Startup folder. Returns the command or file the entry starts, or empty if
// `location`/`value` do not name an existing entry.
std::wstring MachineStartupEntry(const std::wstring& location, const std::wstring& value) {
    if (value.empty() || value.size() > 16383) return {};
    if (location == L"Run" || location == L"Run32") {
        std::wstring cmd;
        HKEY h = nullptr;
        REGSAM view = location == L"Run" ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) return {};
        DWORD type = 0, bytes = 0;
        if (RegQueryValueExW(h, value.c_str(), nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes <= 65536) {
            cmd.resize(bytes / sizeof(wchar_t) + 1);
            if (RegQueryValueExW(h, value.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(cmd.data()), &bytes) == ERROR_SUCCESS)
                cmd.resize(wcsnlen(cmd.c_str(), cmd.size()));
            else
                cmd.clear();
        }
        RegCloseKey(h);
        return cmd;
    }
    if (location == L"StartupFolder") {
        if (value.find_first_of(L"\\/:") != std::wstring::npos || value == L"." || value == L"..") return {};
        PWSTR dir = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonStartup, 0, nullptr, &dir))) path = std::wstring(dir) + L"\\" + value;
        CoTaskMemFree(dir);
        DWORD attr = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY) ? path : std::wstring();
    }
    return {};
}

int RunHelper(const std::vector<std::wstring>& a) {
    if (a.empty() || a[0].rfind(L"--", 0) != 0) return -1;
    if (a[0] == L"--svc") {
        // --svc <start|stop|restart|pause|continue> <service name>
        static const std::pair<const wchar_t*, ServiceAction> verbs[] = {{L"start", ServiceAction::Start}, {L"stop", ServiceAction::Stop},
                                                                        {L"restart", ServiceAction::Restart}, {L"pause", ServiceAction::Pause},
                                                                        {L"continue", ServiceAction::Continue}};
        if (a.size() != 3) return ERROR_INVALID_PARAMETER;
        std::wstring display = ServiceDisplayName(a[2]);
        if (display.empty()) return ERROR_SERVICE_DOES_NOT_EXIST;
        for (auto& [v, act] : verbs) {
            if (a[1] != v) continue;
            if (act == ServiceAction::Stop || act == ServiceAction::Restart || act == ServiceAction::Pause) {
                std::wstring what = act == ServiceAction::Stop ? L"Stop" : act == ServiceAction::Restart ? L"Restart" : L"Pause";
                if (!HelperConfirm(what + L" the service \"" + display + L"\"?",
                                   L"Service name: " + a[2] + L"\n\nPrograms or Windows features that depend on this service may stop working. "
                                   L"If you did not ask NEXUS to do this, choose Cancel.",
                                   what + L" service"))
                    return ERROR_CANCELLED;
            }
            std::wstring msg;
            return static_cast<int>(ControlServiceAction(a[2], act, msg));
        }
        return ERROR_INVALID_PARAMETER;
    }
    if (a[0] == L"--svc-starttype") {
        // --svc-starttype <service name> <2 automatic | 3 manual | 4 disabled> <delayed 0|1>
        if (a.size() != 4) return ERROR_INVALID_PARAMETER;
        std::wstring display = ServiceDisplayName(a[1]);
        if (display.empty()) return ERROR_SERVICE_DOES_NOT_EXIST;
        uint32_t type = a[2] == L"2" ? SERVICE_AUTO_START : a[2] == L"3" ? SERVICE_DEMAND_START : a[2] == L"4" ? SERVICE_DISABLED : 0;
        if (!type || (a[3] != L"0" && a[3] != L"1")) return ERROR_INVALID_PARAMETER;
        if (type == SERVICE_DISABLED &&
            !HelperConfirm(L"Disable the service \"" + display + L"\"?",
                           L"Service name: " + a[1] + L"\n\nIt will no longer start, even when Windows or other programs need it. "
                           L"If you did not ask NEXUS to do this, choose Cancel.",
                           L"Disable service"))
            return ERROR_CANCELLED;
        return static_cast<int>(SetServiceStartType(a[1], type, a[3] == L"1"));
    }
    if (a[0] == L"--startup-set") {
        // --startup-set <Run|Run32|StartupFolder> <entry name> <0|1>
        if (a.size() != 4 || (a[3] != L"0" && a[3] != L"1")) return ERROR_INVALID_PARAMETER;
        std::wstring target = MachineStartupEntry(a[1], a[2]);
        if (target.empty()) return ERROR_FILE_NOT_FOUND;
        bool enable = a[3] == L"1";
        if (enable && !HelperConfirm(L"Let \"" + a[2] + L"\" start with Windows for all users?",
                                     L"It starts:\n" + target + L"\n\nIf you did not ask NEXUS to do this, choose Cancel.", L"Enable"))
            return ERROR_CANCELLED;
        return static_cast<int>(SetStartupItemEnabled(HKEY_LOCAL_MACHINE,
                                                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\" + a[1], a[2], enable));
    }
    if (a[0] == L"--taskmgr-replace") {
        // --taskmgr-replace <0|1>: registers or removes THIS NEXUS.exe as the Task Manager replacement
        // (Image File Execution Options "Debugger"). Windows can start the replacement elevated
        // without a prompt, so it is only registered from a location that only administrators can
        // change (for example, the Program Files folder used by the installer).
        if (a.size() != 2 || (a[1] != L"0" && a[1] != L"1")) return ERROR_INVALID_PARAMETER;
        HKEY k = nullptr;
        LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe", 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &k, nullptr);
        if (r != ERROR_SUCCESS) return static_cast<int>(r);
        if (a[1] == L"1") {
            wchar_t exe[MAX_PATH];
            DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring why;
            if (n == 0 || n >= MAX_PATH || !IsProtectedFromNonAdmins(exe, &why)) {
                RegCloseKey(k);
                return ERROR_INVALID_OWNER;
            }
            std::wstring v = L"\"" + std::wstring(exe) + L"\"";
            r = RegSetValueExW(k, L"Debugger", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
        } else {
            r = RegDeleteValueW(k, L"Debugger");
            if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
        }
        RegCloseKey(k);
        return static_cast<int>(r);
    }
    return -1; // not a helper invocation
}

// Headless measurement of collection cost, used by scripts/benchmark.ps1.
int RunBenchmark(const std::wstring& outPath, int iterations) {
    std::string out = "{\n";
    char b[512];
    uint64_t t0 = QpcNow();
    CpuStatic cpu = QueryCpuStatic();
    double cpuStaticMs = QpcToSeconds(QpcNow() - t0) * 1000.0;
    ProcessCollector pc;
    std::vector<double> times;
    Snapshot s;
    for (int i = 0; i < iterations; ++i) {
        Snapshot snap;
        uint64_t a = QpcNow();
        pc.Collect(snap, 1.0, demand::All, cpu.logical);
        times.push_back(QpcToSeconds(QpcNow() - a) * 1000.0);
        if (i == iterations - 1) s = std::move(snap);
        Sleep(20);
    }
    double first = times.empty() ? 0 : times.front(), sum = 0, mx = 0;
    for (size_t i = 1; i < times.size(); ++i) { sum += times[i]; mx = std::max(mx, times[i]); }
    double avg = times.size() > 1 ? sum / (times.size() - 1) : first;
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::snprintf(b, sizeof b,
                  "  \"version\": \"%s\",\n  \"processes\": %zu,\n  \"cpuStaticMs\": %.3f,\n  \"enumerateFirstMs\": %.3f,\n  \"enumerateAvgMs\": %.3f,\n"
                  "  \"enumerateMaxMs\": %.3f,\n  \"iterations\": %d,\n  \"workingSetBytes\": %zu,\n  \"privateBytes\": %zu,\n  \"handles\": %lu\n}\n",
                  NEXUS_VERSION_STR, s.procs.size(), cpuStaticMs, first, avg, mx, iterations, static_cast<size_t>(pmc.WorkingSetSize),
                  static_cast<size_t>(pmc.PrivateUsage), handles);
    out += b;
    std::wstring err;
    if (outPath.empty()) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD w = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), static_cast<DWORD>(out.size()), &w, nullptr);
        }
        return 0;
    }
    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD w = 0;
    WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &w, nullptr);
    CloseHandle(f);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);

    // DLLs load from System32 only, never from NEXUS's own folder (which may be user-writable, such as
    // Downloads). Delay-loaded imports (see CMakeLists.txt) are resolved through LoadLibrary and so
    // follow this rule too. Nothing may call into a delay-loaded DLL before this line.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    if (!args.empty()) {
        int hr = RunHelper(args);
        if (hr >= 0) return hr;
        if (args[0] == L"--bench") {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            return RunBenchmark(args.size() > 1 ? args[1] : L"", args.size() > 2 ? _wtoi(args[2].c_str()) : 50);
        }
        if (args[0] == L"--version") {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                const char v[] = "NEXUS " NEXUS_VERSION_STR "\n";
                DWORD w = 0;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), v, sizeof v - 1, &w, nullptr);
            }
            return 0;
        }
    }

    bool minimized = false;
    int page = -1;
    int stressSeconds = 0;
    unsigned stressSeed = 1;
    std::wstring stressCsv;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--minimized" || args[i] == L"/minimized") minimized = true;
        else if (args[i] == L"--stress" && i + 1 < args.size()) {
            // --stress <seconds> [csv-path] [seed]
            stressSeconds = std::max(5, _wtoi(args[++i].c_str()));
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) stressCsv = args[++i];
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) stressSeed = static_cast<unsigned>(_wtoi(args[++i].c_str()));
        }
        else if (args[i] == L"--wait-pid" && i + 1 < args.size()) {
            // Restart handoff (e.g. after elevation): wait for the previous instance to exit.
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(args[++i].c_str())));
            if (h) { WaitForSingleObject(h, 10000); CloseHandle(h); }
        } else if (args[i] == L"--page" && i + 1 < args.size()) page = _wtoi(args[++i].c_str());
    }

    // A stress run may start next to a normal instance, so it skips the single-instance check.
    HANDLE mutex = stressSeconds ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\NEXUS.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"NEXUS.MainWindow", nullptr);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_NX_ACTIVATE, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    int rc = 1;
    {
        App app;
        if (stressSeconds) app.EnableStress(stressSeconds, stressCsv, stressSeed);
        if (app.Create(nCmdShow, minimized)) {
            if (page >= 0 && page < static_cast<int>(PageId::Count)) app.Navigate(static_cast<PageId>(page));
            rc = app.Run();
        }
    }
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return rc;
}
