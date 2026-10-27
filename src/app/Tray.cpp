#include "app/Tray.h"
#include "app/App.h"
#include "core/Format.h"

#include <shellapi.h>

#include <algorithm>

namespace nx {

namespace {
constexpr UINT kTrayId = 1;
}

Tray::~Tray() {
    Hide();
}

HICON Tray::MakeIcon(float cpu, float mem) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    const Theme& t = app_.Th();
    auto pack = [](Color c, float a) {
        uint32_t A = static_cast<uint32_t>(a * 255), R = static_cast<uint32_t>(c.r * a * 255), G = static_cast<uint32_t>(c.g * a * 255),
                 B = static_cast<uint32_t>(c.b * a * 255);
        return (A << 24) | (R << 16) | (G << 8) | B;
    };
    // Two vertical meters: CPU (wide) and memory (narrow), with a subtle frame.
    bool dark = true; // taskbar is dark by default on Windows 11; check system theme
    {
        DWORD v = 0;
        RegKey k;
        if (k.Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize")) k.GetDword(L"SystemUsesLightTheme", v);
        dark = v == 0;
    }
    Color frame = dark ? Color{1, 1, 1, 1} : Color{0, 0, 0, 1};
    int cpuW = sz * 10 / 16, gap = std::max(1, sz / 16), memX = cpuW + gap;
    int cpuH = static_cast<int>(std::clamp(cpu, 0.f, 100.f) / 100.f * (sz - 2) + 0.5f);
    int memH = static_cast<int>(std::clamp(mem, 0.f, 100.f) / 100.f * (sz - 2) + 0.5f);
    Color cpuC = cpu >= 90 ? t.critical : t.cpu;
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            uint32_t v = 0;
            bool border = (x == 0 || x == cpuW - 1 || y == 0 || y == sz - 1) && x < cpuW;
            bool mborder = (x == memX || x == sz - 1 || y == 0 || y == sz - 1) && x >= memX;
            if (border || mborder) v = pack(frame, 0.55f);
            else if (x > 0 && x < cpuW - 1 && y >= sz - 1 - cpuH) v = pack(cpuC, 1.f);
            else if (x > memX && x < sz - 1 && y >= sz - 1 - memH) v = pack(t.memory, 1.f);
            else if (x < cpuW || x >= memX) v = pack(frame, 0.08f);
            px[y * sz + x] = v;
        }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void Tray::Show() {
    if (visible_) return;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = app_.Hwnd();
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_NX_TRAY;
    icon_ = MakeIcon(0, 0);
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"NEXUS");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        visible_ = true;
    }
}

void Tray::Hide() {
    if (!visible_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = app_.Hwnd();
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    visible_ = false;
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    lastCpuBucket_ = lastMemBucket_ = -1;
}

void Tray::Update(const Snapshot& s) {
    if (!visible_) return;
    float mem = s.mem.total ? 100.f * s.mem.InUse() / s.mem.total : 0.f;
    int cb = static_cast<int>(s.cpu.total / 100.f * 16), mb = static_cast<int>(mem / 100.f * 16);
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = app_.Hwnd();
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    double net = 0, disk = 0;
    for (auto& n : s.nics) net += n.sendBps + n.recvBps;
    for (auto& d : s.disks) disk = std::max<double>(disk, d.activePct);
    std::wstring tip = L"NEXUS\nCPU " + FormatPercent(s.cpu.total) + L"   Memory " + FormatPercent(mem) + L"\nDisk " + FormatPercent(disk) + L"   Network " +
                       FormatNetRate(net, app_.NetUnits());
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (cb != lastCpuBucket_ || mb != lastMemBucket_) {
        HICON ni = MakeIcon(s.cpu.total, mem);
        if (ni) {
            nid.uFlags |= NIF_ICON;
            nid.hIcon = ni;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            if (icon_) DestroyIcon(icon_);
            icon_ = ni;
            lastCpuBucket_ = cb;
            lastMemBucket_ = mb;
            return;
        }
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::OnMessage(WPARAM, LPARAM lp) {
    UINT ev = LOWORD(lp);
    if (ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONDBLCLK) {
        app_.ShowFromTray();
    } else if (ev == WM_CONTEXTMENU) {
        SetForegroundWindow(app_.Hwnd());
        bool top = app_.Config().GetBool("window.topmost", false);
        UINT c = app_.ShowMenu({{1, L"Open NEXUS"}, {2, L"Mini monitor", app_.MiniMonitorVisible()}, {3, L"Always on top", top}, MenuItem::Sep(),
                                {4, L"Run new task..."}, MenuItem::Sep(), {5, L"Exit"}});
        switch (c) {
        case 1: app_.ShowFromTray(); break;
        case 2: app_.ToggleMiniMonitor(); break;
        case 3: app_.SetTopmost(!top); break;
        case 4: app_.RunNewTaskDialog(); break;
        case 5: app_.ExitApp(); break;
        }
        PostMessageW(app_.Hwnd(), WM_NULL, 0, 0);
    }
}

void Tray::NotifyAlerts(const std::vector<DiagAlert>& alerts) {
    if (!visible_) return;
    uint64_t now = GetTickCount64();
    for (auto& a : alerts) {
        if (a.severity < DiagAlert::Warning) continue;
        auto it = notified_.find(a.title);
        if (it != notified_.end() && now - it->second < 3600000) continue; // at most once per hour per finding
        notified_[a.title] = now;
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = app_.Hwnd();
        nid.uID = kTrayId;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = a.severity == DiagAlert::Critical ? NIIF_ERROR : NIIF_WARNING;
        wcsncpy_s(nid.szInfoTitle, a.title.c_str(), _TRUNCATE);
        std::wstring body = a.evidence.empty() ? a.reasoning : a.evidence.front();
        wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        break; // one balloon per evaluation
    }
}

} // namespace nx
