#include "data/Worker.h"

#include <objbase.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace nx::worker {

namespace {
struct Task {
    std::function<void()> work, done;
};
std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Task> g_queue;
std::vector<HANDLE> g_threads;
bool g_quit = false;
HWND g_hwnd = nullptr;
UINT g_msg = 0;

DWORD WINAPI Loop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] { return g_quit || !g_queue.empty(); });
            if (g_quit) break;
            t = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (t.work) t.work();
        if (t.done) {
            auto* d = new std::function<void()>(std::move(t.done));
            if (!g_hwnd || !PostMessageW(g_hwnd, g_msg, 0, reinterpret_cast<LPARAM>(d))) delete d;
        }
    }
    CoUninitialize();
    return 0;
}
} // namespace

void Init(HWND hwnd, UINT msg, int threads) {
    g_hwnd = hwnd;
    g_msg = msg;
    g_quit = false;
    for (int i = 0; i < threads; ++i)
        if (HANDLE h = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr)) g_threads.push_back(h);
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_quit = true;
        g_queue.clear();
    }
    g_cv.notify_all();
    // Tasks may be blocked in slow system calls (e.g. a hung handle query). Don't hang exit on them.
    if (!g_threads.empty()) WaitForMultipleObjects(static_cast<DWORD>(g_threads.size()), g_threads.data(), TRUE, 1500);
    for (HANDLE h : g_threads) CloseHandle(h);
    g_threads.clear();
    g_hwnd = nullptr;
}

void Run(std::function<void()> work, std::function<void()> done) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_quit) return;
        g_queue.push_back({std::move(work), std::move(done)});
    }
    g_cv.notify_one();
}

void DispatchCompletion(LPARAM lp) {
    auto* d = reinterpret_cast<std::function<void()>*>(lp);
    if (!d) return;
    (*d)();
    delete d;
}

} // namespace nx::worker
