#include "data/Worker.h"

#include <objbase.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
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
// Completions waiting for the UI thread. The posted message carries only an id that is looked up
// here, never a pointer: anything able to post to the window could otherwise make NEXUS call an
// address of its choosing. Unknown ids are ignored.
std::unordered_map<uint64_t, std::function<void()>> g_done;
uint64_t g_nextId = 1;

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
            uint64_t id;
            HWND hwnd;
            {
                std::lock_guard<std::mutex> l(g_lock);
                id = g_nextId++;
                g_done.emplace(id, std::move(t.done));
                hwnd = g_hwnd;
            }
            if (!hwnd || !PostMessageW(hwnd, g_msg, 0, static_cast<LPARAM>(id))) {
                std::lock_guard<std::mutex> l(g_lock);
                g_done.erase(id);
            }
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
        g_done.clear();
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
    std::function<void()> done;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_done.find(static_cast<uint64_t>(lp));
        if (it == g_done.end()) return;
        done = std::move(it->second);
        g_done.erase(it);
    }
    if (done) done();
}

} // namespace nx::worker
