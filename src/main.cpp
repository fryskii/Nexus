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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace nx;

namespace {

// Privileged helper actions. These run in a short-lived elevated NEXUS process started with
// the UAC prompt, so the main UI never needs to run elevated for occasional admin tasks.
int RunHelper(const std::vector<std::wstring>& a) {
    if (a.size() >= 3 && a[0] == L"--svc") {
        static const std::pair<const wchar_t*, ServiceAction> verbs[] = {{L"start", ServiceAction::Start}, {L"stop", ServiceAction::Stop},
                                                                        {L"restart", ServiceAction::Restart}, {L"pause", ServiceAction::Pause},
                                                                        {L"continue", ServiceAction::Continue}};
        for (auto& [v, act] : verbs)
            if (a[1] == v) {
                std::wstring msg;
                return static_cast<int>(ControlServiceAction(a[2], act, msg));
            }
        return ERROR_INVALID_PARAMETER;
    }
    if (a.size() >= 4 && a[0] == L"--svc-starttype") {
        return static_cast<int>(SetServiceStartType(a[1], static_cast<uint32_t>(_wtoi(a[2].c_str())), a[3] == L"1"));
    }
    if (a.size() >= 4 && a[0] == L"--startup-set") {
        // --startup-set <key under HKLM> <value name> <0|1>
        return static_cast<int>(SetStartupItemEnabled(HKEY_LOCAL_MACHINE, a[1], a[2], a[3] == L"1"));
    }
    if (a.size() >= 3 && a[0] == L"--kill") {
        std::wstring why;
        return static_cast<int>(TerminateProcessId(static_cast<uint32_t>(_wtoi(a[1].c_str())), _wcstoui64(a[2].c_str(), nullptr, 10), why));
    }
    if (a.size() >= 3 && a[0] == L"--taskmgr-replace") {
        // Registers / removes NEXUS as the Task Manager replacement (Image File Execution Options "Debugger").
        HKEY k = nullptr;
        LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe", 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &k, nullptr);
        if (r != ERROR_SUCCESS) return static_cast<int>(r);
        if (a[1] == L"1") {
            std::wstring v = L"\"" + a[2] + L"\"";
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

    // Harden DLL search order: only System32 and the application directory.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
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
