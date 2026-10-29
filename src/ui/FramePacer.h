#pragma once
// Frame pacing for the windows of the UI thread: a new frame (of any window) starts no sooner than
// the previous frame took to draw (capped at 500 ms), so drawing uses at most about half of the
// thread however many windows are open. Windows only delivers WM_TIMER when nothing else is
// queued, and a pending repaint counts, so without this, windows whose frames are slower than their
// update rate keep a paint pending forever and starve every timer on the thread (tooltips, status,
// saving). With hardware rendering the gap is about a millisecond.
// Time spent waiting for the display inside EndDraw (vsync) is not drawing work, so it does not
// count, and frames cheaper than kFreeFrameMs get no gap at all: vsync already caps them at the
// monitor's refresh rate, and a short SetTimer gap would be rounded up to the ~15.6 ms system tick.
#include "core/Win.h"

#include <algorithm>

namespace nx {

inline double FrameClockMs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / freq;
}

// Milliseconds the current frame spent presenting (added to by Gfx::End).
inline double& PresentWaitMs() { static thread_local double ms = 0; return ms; }

class FramePacer {
public:
    static constexpr double kFreeFrameMs = 8.0;

    explicit FramePacer(UINT_PTR timerId) : timer_(timerId) {}

    // Use instead of InvalidateRect(hwnd, nullptr, FALSE).
    void Invalidate(HWND hwnd) {
        if (!hwnd) return;
        double now = FrameClockMs();
        double next = NextFrame();
        if (now >= next) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (!pending_) {
            pending_ = true;
            SetTimer(hwnd, timer_, static_cast<UINT>(next - now) + 1, nullptr);
        }
    }
    // Wraps the WM_PAINT handler; returns the frame time in milliseconds.
    template <class F> double Paint(F&& paint) {
        PresentWaitMs() = 0;
        double start = FrameClockMs();
        paint();
        double end = FrameClockMs();
        double work = std::max(0.0, end - start - PresentWaitMs());
        NextFrame() = end + (work < kFreeFrameMs ? 0.0 : std::min(work, 500.0));
        return end - start;
    }
    // Call from WM_TIMER; returns true if the timer was the pacer's.
    bool OnTimer(HWND hwnd, WPARAM id) {
        if (id != timer_) return false;
        KillTimer(hwnd, timer_);
        pending_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

private:
    static double& NextFrame() { static thread_local double next = 0; return next; } // shared by the thread's windows
    UINT_PTR timer_;
    bool pending_ = false;
};

} // namespace nx
