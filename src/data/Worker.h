#pragma once
// Small background task queue. Work runs on a low-priority thread; the optional completion runs on
// the UI thread (delivered through a posted window message), so UI state is never touched off-thread.
#include <windows.h>

#include <functional>

namespace nx::worker {

void Init(HWND uiWindow, UINT message, int threads = 2);
void Shutdown();
// `work` runs in the background, then `done` runs on the UI thread.
void Run(std::function<void()> work, std::function<void()> done = {});
// Like Run, but on a thread of its own: for work that mostly waits (an elevated helper waiting on a
// UAC prompt) and would otherwise hold one of the few queue threads and stall everything behind it.
void RunLong(std::function<void()> work, std::function<void()> done = {});
// Called by the UI window procedure when it receives the configured message.
void DispatchCompletion(LPARAM lp);

} // namespace nx::worker
