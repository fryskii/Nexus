// UI stress mode: NEXUS.exe --stress <seconds> [csv-path] [seed]
//
// Drives the real window with randomized but reproducible actions (navigation, resizing, search
// queries including hostile regular expressions, theme/font/scale/compact switches, view changes,
// scrolling, hover, inspector and mini-monitor windows) while recording NEXUS's own resource use.
// Settings are never written, and dialogs and menus are suppressed so no destructive action can
// be confirmed. At the end a CSV of samples and a summary with leak indicators are written.
#include "app/App.h"
#include "app/StressState.h"
#include "core/Format.h"
#include "core/Win.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace nx {

namespace {
constexpr UINT_PTR kTimerStressTick = 40, kTimerStressSample = 41;

DWORD OwnThreadCount() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD n = 0, self = GetCurrentProcessId();
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == self) ++n;
    CloseHandle(snap);
    return n;
}

const wchar_t* const kQueries[] = {
    L"svc", L"chrome", L"cpu>1", L"mem>50MB", L"!svchost", L"pid:4", L"pid:99999999999", L"/^s.*exe$/", L"/(a+)+$/",
    L"/(x|xx)*y/", L"re:[a-", L"/\\bhost\\b/", L"\"quoted text\"", L"user:sys cmd:-k", L"threads>10 handles<100000",
    L"mem>-5", L"io>1e400", L"/(?=x)/", L"/\\1/", L"中文", L"\U0001F600 emoji", L"   ", L"/", L"!", L"\"",
};
// Action mix: cumulative weights out of 100 and the names used in the summary and in
// NEXUS_STRESS_ONLY (comma-separated subset, or "idle" for no UI actions at all).
const struct { int upTo; const char* name; } kActions[] = {
    {14, "navigate"}, {22, "resize"}, {32, "search"}, {36, "search-clear"}, {39, "theme"}, {41, "font"}, {43, "scale"},
    {45, "accent"}, {48, "compact"}, {50, "insights"}, {52, "nav-collapse"}, {56, "process-view"}, {70, "key"},
    {84, "hover"}, {92, "wheel"}, {94, "inspector-open"}, {96, "inspector-close"}, {98, "mini-monitor"}, {100, "refresh-rate"},
};
} // namespace

void App::EnableStress(int seconds, const std::wstring& csvPath, unsigned seed) {
    stress_ = std::make_unique<StressState>();
    stress_->seconds = std::clamp(seconds, 5, 7 * 24 * 3600);
    stress_->csvPath = csvPath;
    stress_->rng.seed(seed);
    wchar_t only[512];
    DWORD n = GetEnvironmentVariableW(L"NEXUS_STRESS_ONLY", only, 512);
    if (n > 0 && n < 512) stress_->only = L"," + std::wstring(only, n) + L",";
}

void App::StartStress() {
    if (!stress_) return;
    stress_->start = GetTickCount64();
    sampler_->SetIntervalMs(250); // fastest supported sampling, to stress the collector too
    SetTimer(hwnd_, kTimerStressTick, 30, nullptr);
    SetTimer(hwnd_, kTimerStressSample, 1000, nullptr);
    StressSampleNow();
}

void App::RecordPaintTime(double ms) {
    if (!stress_ || stress_->finished) return;
    // Belt and braces: end the run on time even if timers were starved (posted messages are
    // delivered before WM_PAINT and WM_TIMER).
    if (GetTickCount64() - stress_->start >= static_cast<uint64_t>(stress_->seconds) * 1000)
        PostMessageW(hwnd_, WM_TIMER, kTimerStressTick, 0);
    stress_->paintMs.push_back(static_cast<float>(ms));
    stress_->paintMsByPage[static_cast<int>(current_)].push_back(static_cast<float>(ms));
}

void App::RecordSnapshotTime(double ms) {
    if (stress_ && !stress_->finished) stress_->snapshotMs.push_back(static_cast<float>(ms));
}

void App::CountStressMessage(const MSG& msg) {
    if (stress_->finished) return;
    wchar_t cls[64] = L"(thread)";
    if (msg.hwnd) GetClassNameW(msg.hwnd, cls, 64);
    ++stress_->messages[{msg.message, cls}];
}

bool App::OnStressTimer(UINT_PTR id) {
    if (!stress_ || stress_->finished) return false;
    ++stress_->timerFires[id];
    if (id == kTimerStressSample) { StressSampleNow(); return true; }
    if (id != kTimerStressTick) return false;
    if (GetTickCount64() - stress_->start >= static_cast<uint64_t>(stress_->seconds) * 1000) { FinishStress(); return true; }

    auto& rng = stress_->rng;
    auto rnd = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    std::wstring detail;
    auto count = [&](const char* name) {
        ++stress_->actions[name];
        char head[64];
        std::snprintf(head, sizeof head, "%8.2f %s ", (GetTickCount64() - stress_->start) / 1000.0, name);
        stress_->log += head + ToUtf8(detail.substr(0, 80)) + "\n";
    };
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int r = rnd(100);
    if (!stress_->only.empty()) {
        // Restricted mix: re-draw until an allowed action comes up ("idle" allows none).
        auto allowed = [&](int v) {
            for (auto& a : kActions)
                if (v < a.upTo) return stress_->only.find(L"," + std::wstring(a.name, a.name + strlen(a.name)) + L",") != std::wstring::npos;
            return false;
        };
        int tries = 0;
        while (!allowed(r) && ++tries < 1000) r = rnd(100);
        if (tries >= 1000) { Invalidate(); UpdateWindow(hwnd_); return true; }
    }
    if (r < 14) {
        PageId to = static_cast<PageId>(rnd(static_cast<int>(PageId::Count)));
        detail = kNavEntries[static_cast<int>(to)].name;
        Navigate(to);
        count("navigate");
    } else if (r < 22) {
        int w = 640 + rnd(1100), h = 480 + rnd(700);
        SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        detail = std::to_wstring(w) + L"x" + std::to_wstring(h);
        count("resize");
    } else if (r < 32) {
        std::wstring q = kQueries[rnd(static_cast<int>(std::size(kQueries)))];
        if (rnd(10) == 0) q = std::wstring(500 + rnd(1500), static_cast<wchar_t>(L'a' + rnd(26))); // very long input
        SetWindowTextW(search_, q.c_str());
        detail = q;
        count("search");
    } else if (r < 36) {
        SetWindowTextW(search_, L"");
        count("search-clear");
    } else if (r < 39) {
        settings_.SetString("ui.theme", settings_.GetString("ui.theme", L"system") == L"dark" ? L"light" : L"dark");
        ApplyTheme();
        count("theme");
    } else if (r < 41) {
        bool inter = settings_.GetString("ui.font", L"inter") != L"segoe";
        settings_.SetString("ui.font", inter ? L"segoe" : L"inter");
        Gfx::SetUiFont(inter ? L"segoe" : L"inter");
        ApplyTheme();
        count("font");
    } else if (r < 43) {
        static const double scales[] = {0.75, 1.0, 1.25, 1.5, 2.0};
        double sc = scales[rnd(5)];
        settings_.SetDouble("ui.scale", sc);
        ApplyTheme();
        detail = std::to_wstring(sc);
        count("scale");
    } else if (r < 45) {
        static const wchar_t* accents[] = {L"system", L"windows", L"8764B8", L"E74856", L"10893E"};
        settings_.SetString("ui.accent", accents[rnd(5)]);
        ApplyTheme();
        count("accent");
    } else if (r < 48) {
        SetCompact(!compact_);
        count("compact");
    } else if (r < 50) {
        ToggleInsights();
        count("insights");
    } else if (r < 52) {
        navCollapsed_ = !navCollapsed_;
        count("nav-collapse");
    } else if (r < 56) {
        if (Page* p = GetPage(PageId::Processes)) p->OnPageMenu(501 + rnd(3));
        count("process-view");
    } else if (r < 70) {
        static const UINT keys[] = {VK_DOWN, VK_UP, VK_NEXT, VK_PRIOR, VK_HOME, VK_END};
        pages_[static_cast<int>(current_)]->OnKey(keys[rnd(6)], false, false);
        count("key");
    } else if (r < 84) {
        LPARAM pt = MAKELPARAM(rnd(std::max<LONG>(1, rc.right)), rnd(std::max<LONG>(1, rc.bottom)));
        SendMessageW(hwnd_, WM_MOUSEMOVE, 0, pt);
        count("hover");
    } else if (r < 92) {
        POINT pt{rnd(std::max<LONG>(1, rc.right)), rnd(std::max<LONG>(1, rc.bottom))};
        ClientToScreen(hwnd_, &pt);
        SendMessageW(hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, (rnd(2) ? 1 : -1) * WHEEL_DELTA * (1 + rnd(3))), MAKELPARAM(pt.x, pt.y));
        count("wheel");
    } else if (r < 94) {
        if (snap_ && !snap_->procs.empty() && stress_->inspectors.size() < 3) {
            OpenProcessWindow(snap_->procs[rnd(static_cast<int>(snap_->procs.size()))].pid);
            count("inspector-open");
        }
    } else if (r < 96) {
        // Close inspector windows (they are top-level windows of this thread).
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, L"NEXUS.ProcessWindow", nullptr)) != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId()) { SendMessageW(w, WM_CLOSE, 0, 0); w = nullptr; count("inspector-close"); }
        }
    } else if (r < 98) {
        ToggleMiniMonitor();
        count("mini-monitor");
    } else {
        static const uint32_t rates[] = {250, 500, 1000};
        sampler_->SetIntervalMs(rates[rnd(3)]);
        count("refresh-rate");
    }
    // Count open inspectors so the cap above holds.
    stress_->inspectors.clear();
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"NEXUS.ProcessWindow", nullptr)) != nullptr;) stress_->inspectors.push_back(w);
    Invalidate();
    UpdateWindow(hwnd_);
    return true;
}

void App::StressSampleNow() {
    StressSample s;
    s.t = (GetTickCount64() - stress_->start) / 1000.0;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        s.privateBytes = pmc.PrivateUsage;
        s.workingSet = pmc.WorkingSetSize;
    }
    GetProcessHandleCount(GetCurrentProcess(), &s.handles);
    s.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    s.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    s.threads = OwnThreadCount();
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto ft = [](const FILETIME& f) { return ((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        s.cpuSec = ft(k) + ft(u);
    }
    s.frames = stress_->paintMs.size();
    stress_->samples.push_back(s);
}

void App::FinishStress() {
    auto& st = *stress_;
    st.finished = true;
    KillTimer(hwnd_, kTimerStressTick);
    KillTimer(hwnd_, kTimerStressSample);
    StressSampleNow();

    std::wstring csv = st.csvPath;
    if (csv.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        csv = std::wstring(tmp) + L"nexus-stress.csv";
    }
    std::string out = "seconds,private_bytes,working_set,handles,gdi_objects,user_objects,threads,cpu_seconds,frames\n";
    char line[256];
    for (auto& s : st.samples) {
        std::snprintf(line, sizeof line, "%.1f,%llu,%llu,%lu,%lu,%lu,%lu,%.3f,%llu\n", s.t, static_cast<unsigned long long>(s.privateBytes),
                      static_cast<unsigned long long>(s.workingSet), s.handles, s.gdi, s.user, s.threads, s.cpuSec,
                      static_cast<unsigned long long>(s.frames));
        out += line;
    }
    std::wstring err;
    WriteFileAtomic(csv, out, err);

    // Summary. Growth compares the median of the second quarter (after warm-up) with the last quarter.
    std::string sum;
    auto add = [&](const char* fmt, auto... a) { std::snprintf(line, sizeof line, fmt, a...); sum += line; };
    std::vector<float> sorted = st.paintMs;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0.0 : static_cast<double>(sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]); };
    double total = 0;
    for (float f : sorted) total += f;
    const auto& first = st.samples.front();
    const auto& last = st.samples.back();
    add("NEXUS stress run: %d s, seed-reproducible actions\n", st.seconds);
    add("frames: %zu  paint ms avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", sorted.size(), sorted.empty() ? 0.0 : total / sorted.size(),
        pct(0.5), pct(0.95), pct(0.99), sorted.empty() ? 0.0 : static_cast<double>(sorted.back()));
    add("cpu: %.1f s over %.0f s wall (%.1f%% of one core)\n", last.cpuSec - first.cpuSec, last.t, last.t > 0 ? 100.0 * (last.cpuSec - first.cpuSec) / last.t : 0.0);
    size_t n = st.samples.size();
    auto median = [&](size_t from, size_t to, auto field) {
        std::vector<double> v;
        for (size_t k = from; k < to && k < n; ++k) v.push_back(static_cast<double>(field(st.samples[k])));
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    auto report = [&](const char* name, auto field, double scale, const char* unit) {
        double peak = 0;
        for (auto& s : st.samples) peak = std::max(peak, static_cast<double>(field(s)));
        double q2 = median(n / 4, n / 2, field), q4 = median(3 * n / 4, n, field);
        add("%-13s start %10.1f  peak %10.1f  2nd-quarter median %10.1f  last-quarter median %10.1f %s  (%+.1f%%)\n", name,
            field(first) / scale, peak / scale, q2 / scale, q4 / scale, unit, q2 > 0 ? 100.0 * (q4 - q2) / q2 : 0.0);
    };
    report("private", [](const StressSample& s) { return s.privateBytes; }, 1048576.0, "MB");
    report("working set", [](const StressSample& s) { return s.workingSet; }, 1048576.0, "MB");
    report("handles", [](const StressSample& s) { return s.handles; }, 1.0, "");
    report("GDI objects", [](const StressSample& s) { return s.gdi; }, 1.0, "");
    report("USER objects", [](const StressSample& s) { return s.user; }, 1.0, "");
    report("threads", [](const StressSample& s) { return s.threads; }, 1.0, "");
    {
        std::vector<float> sn = st.snapshotMs;
        std::sort(sn.begin(), sn.end());
        if (!sn.empty())
            add("snapshots handled: %zu  ms p50 %.2f  p95 %.2f  max %.2f\n", sn.size(), static_cast<double>(sn[sn.size() / 2]),
                static_cast<double>(sn[sn.size() * 95 / 100]), static_cast<double>(sn.back()));
        sum += "timer deliveries:";
        for (auto& [id, cnt] : st.timerFires) add(" %u=%llu", static_cast<unsigned>(id), static_cast<unsigned long long>(cnt));
        sum += "\n";
    }
    {
        std::vector<std::pair<uint64_t, std::pair<UINT, std::wstring>>> top;
        for (auto& [k, v] : st.messages) top.push_back({v, k});
        std::sort(top.rbegin(), top.rend());
        sum += "main-loop messages (count, id, window class):\n";
        for (size_t k = 0; k < top.size() && k < 12; ++k)
            add("  %8llu  0x%04x  %ls\n", static_cast<unsigned long long>(top[k].first), top[k].second.first, top[k].second.second.c_str());
    }
    sum += "paint by page (frames, median ms, max ms):\n";
    for (auto& [page, v] : st.paintMsByPage) {
        std::vector<float> s2 = v;
        std::sort(s2.begin(), s2.end());
        add("  %-20ls %6zu %9.2f %9.2f\n", kNavEntries[page].name, s2.size(), static_cast<double>(s2[s2.size() / 2]), static_cast<double>(s2.back()));
    }
    sum += "actions:";
    for (auto& [k, v] : st.actions) add(" %s=%llu", k.c_str(), static_cast<unsigned long long>(v));
    sum += "\n";
    WriteFileAtomic(csv + L".summary.txt", sum, err);
    WriteFileAtomic(csv + L".actions.txt", st.log, err);
    ExitApp();
}

} // namespace nx
