// Windows tests for code that talks to NEXUS's own windows from other threads. Runs without elevation.
//
// Hang on exit (fixed 2026-10-29): when NEXUS closed, its window thread waited for the sampler
// thread to stop while the sampler read the window's title with GetWindowText, which sends
// WM_GETTEXT to that same waiting thread, so both waited forever. The test recreates that: a
// visible window whose thread does not read messages while the process collector runs on another
// thread. The collector must finish; it must also still read the title when the thread is responsive.
#include "data/Processes.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace nx;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) ++g_passed;                                                      \
        else { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

constexpr wchar_t kTitle[] = L"NEXUS window test";

struct Job {
    Snapshot snap;
};

DWORD WINAPI Collect(void* p) {
    auto* job = static_cast<Job*>(p);
    ProcessCollector pc;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    pc.Collect(job->snap, 1.0, demand::All, si.dwNumberOfProcessors);
    return 0;
}

std::wstring OwnTitle(const Snapshot& snap) {
    for (const auto& s : snap.procs)
        if (s.pid == GetCurrentProcessId() && s.windowTitle) return *s.windowTitle;
    return L"<none>";
}

// Waits for the thread, optionally reading this thread's messages meanwhile.
bool Finishes(HANDLE thread, DWORD ms, bool pump) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        DWORD left = static_cast<DWORD>(end - GetTickCount());
        if (static_cast<LONG>(left) <= 0) return WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
        if (!pump) return WaitForSingleObject(thread, left) == WAIT_OBJECT_0;
        DWORD w = MsgWaitForMultipleObjects(1, &thread, FALSE, left, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) return true;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
}

} // namespace

int wmain() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NEXUS.WindowTest";
    RegisterClassW(&wc);
    // Visible (the collector only looks at visible top-level windows that are not tool windows) but
    // off screen and inactive.
    HWND w = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, kTitle, WS_POPUP, -32000, -32000, 100, 100, nullptr, nullptr,
                             wc.hInstance, nullptr);
    CHECK(w != nullptr);
    if (!w) return 1;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);

    {
        // Responsive window thread: the title is read.
        Job job;
        HANDLE t = CreateThread(nullptr, 0, Collect, &job, 0, nullptr);
        bool done = Finishes(t, 10000, true);
        CHECK(done);
        if (done) CHECK(OwnTitle(job.snap) == kTitle);
        if (!done) TerminateThread(t, 1);
        CloseHandle(t);
    }
    {
        // Window thread blocked (as when it waits for the sampler to stop): the collector still finishes.
        Job job;
        HANDLE t = CreateThread(nullptr, 0, Collect, &job, 0, nullptr);
        bool done = Finishes(t, 10000, false);
        CHECK(done);
        if (!done) {
            std::printf("FAIL: the collector waited for this window's blocked thread (the hang on exit)\n");
            // Let it finish so the process can exit normally.
            Finishes(t, 10000, true);
        }
        CloseHandle(t);
    }
    DestroyWindow(w);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
