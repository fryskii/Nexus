#include "app/MiniMonitor.h"
#include "app/App.h"
#include "ui/Graph.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace nx {

namespace {
LRESULT CALLBACK MiniProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<MiniMonitor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
constexpr float kW = 250.f, kH = 172.f;
} // namespace

MiniMonitor::~MiniMonitor() { Destroy(); }

void MiniMonitor::Destroy() {
    if (hwnd_) {
        RECT r;
        GetWindowRect(hwnd_, &r);
        app_.Config().SetInt("mini.x", r.left);
        app_.Config().SetInt("mini.y", r.top);
        ui_.reset();
        gfx_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void MiniMonitor::Create() {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MiniProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NEXUS.MiniMonitor";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForWindow(app_.Hwnd());
    int w = MulDiv(static_cast<int>(kW), dpi, 96), h = MulDiv(static_cast<int>(kH), dpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = static_cast<int>(app_.Config().GetInt("mini.x", wa.right - w - 24));
    int y = static_cast<int>(app_.Config().GetInt("mini.y", wa.top + 24));
    RECT test{x, y, x + w, y + h};
    if (!MonitorFromRect(&test, MONITOR_DEFAULTTONULL)) { x = wa.right - w - 24; y = wa.top + 24; }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"NEXUS mini monitor", WS_POPUP, x, y, w, h, nullptr, nullptr,
                            wc.hInstance, this);
    BYTE alpha = static_cast<BYTE>(std::clamp<int64_t>(app_.Config().GetInt("mini.opacity", 235), 80, 255));
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    int corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof corner);
    gfx_.Init(hwnd_);
    gfx_.SetDpi(static_cast<float>(dpi));
}

void MiniMonitor::Show(bool show) {
    if (show && !hwnd_) Create();
    if (hwnd_) ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) Update();
}

void MiniMonitor::OnThemeChanged() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

void MiniMonitor::Update() { if (Visible()) InvalidateRect(hwnd_, nullptr, FALSE); }

void MiniMonitor::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (gfx_.Begin()) {
        const Theme& t = app_.Th();
        if (!ui_) ui_ = std::make_unique<Ui>(gfx_, t, in_);
        Ui& ui = *ui_;
        ui.BeginFrame();
        gfx_.Clear(t.window);
        gfx_.StrokeRound({0.5f, 0.5f, gfx_.Width() - 1, gfx_.Height() - 1}, 8.f, t.cardBorder);
        auto snap = app_.Snap();
        auto& h = app_.Hist();
        float y = 10;
        auto row = [&](const wchar_t* label, const std::wstring& value, const RingBuffer<float>& data, float maxV, Color c) {
            gfx_.Text(label, {12, y, 60, 26}, Font::Small, t.textSecondary);
            gfx_.Text(value, {64, y, 92, 26}, Font::BodySemi, t.text);
            if (data.Size() > 1) GraphView::Spark(gfx_, {160, y + 4, 78, 18}, data, maxV, c);
            y += 30;
        };
        if (snap) {
            row(L"CPU", FormatPercent(snap->cpu.total), h.cpu, 100, t.cpu);
            float mem = snap->mem.total ? 100.f * snap->mem.InUse() / snap->mem.total : 0.f;
            row(L"Memory", FormatPercent(mem) + L"  " + FormatBytes(snap->mem.InUse()), h.memPct, 100, t.memory);
            float g = 0;
            for (auto& gp : snap->gpus) g = std::max(g, gp.util);
            row(L"GPU", snap->gpuCollected ? FormatPercent(g) : L"—", h.gpuMax, 100, t.gpu);
            double rd = 0, wr = 0;
            for (auto& d : snap->disks) { rd += d.readBps; wr += d.writeBps; }
            float dmax = std::max(1.f, std::max(h.diskTotalRead.Max(), h.diskTotalWrite.Max()));
            row(L"Disk", FormatRateBytes(rd + wr), h.diskTotalRead, dmax, t.disk);
            double s = 0, r = 0;
            for (auto& n : snap->nics) { s += n.sendBps; r += n.recvBps; }
            float nmax = std::max(1.f, std::max(h.netTotalSend.Max(), h.netTotalRecv.Max()));
            row(L"Network", L"↑" + FormatNetRate(s, app_.NetUnits()) + L" ↓" + FormatNetRate(r, app_.NetUnits()), h.netTotalRecv, nmax, t.network);
        } else {
            gfx_.Text(L"Collecting…", {12, 10, 200, 26}, Font::Body, t.textSecondary);
        }
        ui.EndFrame();
        gfx_.End();
    }
    EndPaint(hwnd_, &ps);
}

LRESULT MiniMonitor::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: gfx_.Resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_DPICHANGED: {
        gfx_.SetDpi(HIWORD(w));
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, m, w, l);
        return r == HTCLIENT ? HTCAPTION : r; // drag anywhere
    }
    case WM_NCLBUTTONDBLCLK: app_.ShowFromTray(); return 0;
    case WM_NCRBUTTONUP: case WM_CONTEXTMENU: {
        int alpha = static_cast<int>(app_.Config().GetInt("mini.opacity", 235));
        UINT c = app_.ShowMenu({{1, L"Open NEXUS"}, {2, L"Opacity 100%", alpha >= 255, true, false, true}, {3, L"Opacity 90%", alpha == 230, true, false, true},
                                {4, L"Opacity 75%", alpha == 191, true, false, true}, MenuItem::Sep(), {5, L"Close mini monitor"}});
        int a = c == 2 ? 255 : c == 3 ? 230 : c == 4 ? 191 : -1;
        if (a > 0) { app_.Config().SetInt("mini.opacity", a); SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(a), LWA_ALPHA); }
        if (c == 1) app_.ShowFromTray();
        if (c == 5) app_.ToggleMiniMonitor();
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(h, &r);
        app_.Config().SetInt("mini.x", r.left);
        app_.Config().SetInt("mini.y", r.top);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace nx
