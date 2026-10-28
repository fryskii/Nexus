// Process-churn load generator for stress-testing NEXUS (Windows; also runs under Wine).
//
//   nexus_churn <alive> <replace-per-second> <seconds>
//
// Keeps <alive> child processes running and replaces <replace-per-second> of them every second
// (oldest first). Children run under several executable names (including Unicode and very long
// ones), get long command lines, start a few threads, allocate some memory, and some of them burn
// CPU, so every column in NEXUS changes constantly while rows appear and disappear.
// Only processes started by this program are terminated.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

DWORD WINAPI ChildThread(void* p) {
    bool burn = p != nullptr;
    volatile unsigned x = 0;
    for (;;) {
        if (burn) {
            DWORD until = GetTickCount() + 30;
            while (GetTickCount() < until) x = x * 1664525u + 1013904223u;
            Sleep(70);
        } else {
            Sleep(500);
        }
    }
    return 0;
}

int ChildMain(int threads, bool burn, int megabytes) {
    std::vector<char> mem(static_cast<size_t>(megabytes) * 1024 * 1024, 1); // touched, so it counts as private
    for (int i = 0; i < threads; ++i) CloseHandle(CreateThread(nullptr, 0, ChildThread, burn && i == 0 ? &mem : nullptr, 0, nullptr));
    for (;;) Sleep(1000);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 5 && std::wcscmp(argv[1], L"--child") == 0)
        return ChildMain(_wtoi(argv[2]), _wtoi(argv[3]) != 0, _wtoi(argv[4]));
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: nexus_churn <alive> <replace-per-second> <seconds>\n");
        return 2;
    }
    int alive = _wtoi(argv[1]), rate = _wtoi(argv[2]), seconds = _wtoi(argv[3]);

    // Copies of this executable under varied names, so NEXUS groups and sorts many distinct apps.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = ExeDir();
    std::vector<std::wstring> exes;
    const wchar_t* names[] = {L"churn-a.exe", L"churn-b.exe", L"churn-ünïcødé-日本.exe",
                              L"churn-a-very-long-executable-name-that-keeps-going-and-going-to-test-truncation.exe",
                              L"churn space name.exe", L"svchost-lookalike.exe"};
    for (const wchar_t* n : names) {
        std::wstring p = dir + n;
        if (CopyFileW(self, p.c_str(), FALSE) || GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) exes.push_back(p);
    }
    if (exes.empty()) exes.push_back(self);

    std::mt19937 rng(42);
    std::deque<PROCESS_INFORMATION> kids;
    unsigned long long started = 0, failed = 0;
    auto spawn = [&] {
        const std::wstring& exe = exes[rng() % exes.size()];
        int threads = 1 + rng() % 6;
        bool burn = rng() % 10 == 0;
        int mb = rng() % 8;
        std::wstring cmd = L"\"" + exe + L"\" --child " + std::to_wstring(threads) + L" " + (burn ? L"1" : L"0") + L" " + std::to_wstring(mb);
        if (rng() % 8 == 0) cmd += L" --padding=" + std::wstring(1000 + rng() % 7000, L'x'); // long command lines
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            pi.hThread = nullptr;
            kids.push_back(pi);
            ++started;
        } else {
            ++failed;
        }
    };
    auto killOldest = [&] {
        if (kids.empty()) return;
        PROCESS_INFORMATION pi = kids.front();
        kids.pop_front();
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
    };

    while (static_cast<int>(kids.size()) < alive) spawn();
    DWORD end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    while (static_cast<long>(end - GetTickCount()) > 0) {
        for (int i = 0; i < rate; ++i) {
            killOldest();
            spawn();
        }
        Sleep(1000);
    }
    while (!kids.empty()) killOldest();
    std::wprintf(L"started %llu processes, %llu failed\n", started, failed);
    return 0;
}
