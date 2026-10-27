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
// Called by the UI window procedure when it receives the configured message.
void DispatchCompletion(LPARAM lp);

} // namespace nx::worker
