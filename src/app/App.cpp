#include "app/App.h"
#include "app/MiniMonitor.h"
#include "app/Tray.h"
#include "core/Win.h"
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/Accessibility.h"
#include "ui/ProcessWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define NX_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define NX_DWMWA_SYSTEMBACKDROP_TYPE 38

namespace nx {

namespace {
App* g_app = nullptr;
constexpr UINT_PTR kTimerStatus = 1, kTimerTooltip = 2, kTimerSave = 3;
constexpr wchar_t kClassName[] = L"NEXUS.MainWindow";

LRESULT CALLBACK StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return app ? app->WndProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// Dark context menus: uxtheme ordinal 135 (SetPreferredAppMode). Cosmetic only; ignored if absent.
void EnableDarkMenus(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<Flush>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (set) set(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

COLORREF ToColorRef(Color c) {
    return RGB(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255), static_cast<int>(c.b * 255));
}
Color Opaque(Color c, Color over) { return over.Mix(Color{c.r, c.g, c.b, 1}, c.a); }

struct NavEntry { PageId id; const wchar_t* name; wchar_t glyph; };
const NavEntry kNav[] = {
    {PageId::Overview, L"Overview", 0xE80F},     {PageId::Processes, L"Processes", 0xE71D}, {PageId::Performance, L"Performance", 0xE9D2},
    {PageId::AppHistory, L"App history", 0xE81C}, {PageId::Startup, L"Startup apps", 0xE7E8}, {PageId::Users, L"Users", 0xE716},
    {PageId::Details, L"Details", 0xE8FD},       {PageId::Services, L"Services", 0xEA86},    {PageId::Hardware, L"Hardware", 0xE7F4},
    {PageId::Network, L"Network", 0xE839},       {PageId::Storage, L"Storage", 0xEDA2},      {PageId::Diagnostics, L"System diagnostics", 0xE9D9},
    {PageId::Settings, L"Settings", 0xE713},
};
} // namespace

App& TheApp() { return *g_app; }

void Histories::Resize(size_t c) {
    cap = c;
    for (auto* r : {&cpu, &cpuKernel, &memPct, &commitPct, &memUsed, &pagesPerSec, &thermal, &cpuMHz, &netTotalSend, &netTotalRecv, &diskTotalRead, &diskTotalWrite, &gpuMax})
        r->Resize(c);
    for (auto& r : cores) r.Resize(c);
    for (auto& [k, d] : disks) { d.active.Resize(c); d.read.Resize(c); d.write.Resize(c); d.latency.Resize(c); }
    for (auto& [k, n] : nics) { n.send.Resize(c); n.recv.Resize(c); }
    for (auto& [k, gh] : gpus) {
        gh.util.Resize(c); gh.dedicated.Resize(c); gh.shared.Resize(c); gh.temp.Resize(c);
        for (auto& [n, e] : gh.engines) e.Resize(c);
    }
}

App::App() { g_app = this; }

App::~App() {
    g_app = nullptr;
}

// ------------------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------------------
void App::LoadSettings() {
    // Portable mode: a "portable" marker file next to the executable keeps settings beside it.
    std::wstring exeDir = GetModuleDir();
    if (FileExists(exeDir + L"\\portable") || FileExists(exeDir + L"\\portable.txt")) {
        configDir_ = exeDir;
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
            configDir_ = std::wstring(p) + L"\\NEXUS";
            CoTaskMemFree(p);
        } else {
            configDir_ = exeDir;
        }
        SHCreateDirectoryExW(nullptr, configDir_.c_str(), nullptr);
    }
    configPath_ = configDir_ + L"\\nexus.ini";
    HANDLE h = CreateFileW(configPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > 0 && sz.QuadPart < 4 * 1024 * 1024) {
            std::string data(static_cast<size_t>(sz.QuadPart), '\0');
            DWORD rd = 0;
            if (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &rd, nullptr)) {
                data.resize(rd);
                settings_.Parse(data);
            }
        }
        CloseHandle(h);
    }
}

void App::SaveSettings() {
    for (auto& p : pages_)
        if (p) p->SaveState();
    SaveWindowPlacement();
    if (!settings_.Dirty()) return;
    std::wstring err;
    if (WriteFileAtomic(configPath_, settings_.Serialize(), err)) settings_.ClearDirty();
}

void App::SaveWindowPlacement() {
    if (!hwnd_) return;
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    RECT r = wp.rcNormalPosition;
    settings_.SetInt("window.x", r.left);
    settings_.SetInt("window.y", r.top);
    settings_.SetInt("window.w", r.right - r.left);
    settings_.SetInt("window.h", r.bottom - r.top);
    settings_.SetBool("window.maximized", wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED));
}

void App::RestoreWindowPlacement(int& nCmdShow) {
    if (!settings_.Has("window.w")) return;
    RECT r{static_cast<LONG>(settings_.GetInt("window.x")), static_cast<LONG>(settings_.GetInt("window.y")), 0, 0};
    r.right = r.left + static_cast<LONG>(std::max<int64_t>(480, settings_.GetInt("window.w")));
    r.bottom = r.top + static_cast<LONG>(std::max<int64_t>(360, settings_.GetInt("window.h")));
    // Only restore if the rectangle is still on a connected monitor.
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{sizeof wp};
        wp.rcNormalPosition = r;
        wp.showCmd = settings_.GetBool("window.maximized") ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(hwnd_, &wp);
        if (wp.showCmd == SW_SHOWMAXIMIZED && nCmdShow == SW_SHOWNORMAL) nCmdShow = SW_SHOWMAXIMIZED;
    }
}

// ------------------------------------------------------------------------------------------
// Creation
// ------------------------------------------------------------------------------------------
bool App::Create(int nCmdShow, bool startMinimized) {
    elevated_ = IsElevated();
    LoadSettings();
    compact_ = settings_.GetBool("ui.compact", false);
    navCollapsed_ = settings_.GetBool("ui.nav.collapsed", false);

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    std::wstring title = elevated_ ? L"NEXUS (Administrator)" : L"NEXUS";
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            1180, 760, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (!settings_.Has("window.w")) {
        // First run: size relative to DPI and centre on the primary work area.
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = MulDiv(1180, static_cast<int>(dpi_), 96), h = MulDiv(760, static_cast<int>(dpi_), 96);
        w = std::min<int>(w, wa.right - wa.left);
        h = std::min<int>(h, wa.bottom - wa.top);
        SetWindowPos(hwnd_, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER);
    }
    RestoreWindowPlacement(nCmdShow);

    worker::Init(hwnd_, WM_NX_WORKER, 2);
    ApplyMica();
    if (!gfx_.Init(hwnd_)) {
        MessageBoxW(hwnd_, L"Direct2D could not be initialised.", L"NEXUS", MB_ICONERROR);
        return false;
    }
    ApplyTheme();
    icons_.Init(&gfx_, hwnd_, WM_NULL);
    icons_.SetPixelSize(static_cast<int>(std::lround(16 * dpi_ / 96.f)) <= 20 ? (dpi_ > 120 ? 20 : 16) : 32);

    // Search box (native edit so IME, clipboard, accessibility and undo work as users expect).
    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(100), wc.hInstance, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search processes (name, PID, publisher, /regex/, cpu>5)"));
    SetWindowTheme(search_, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    sampler_ = std::make_unique<Sampler>(hwnd_, WM_NX_SNAPSHOT);
    sampler_->SetNetTraceEnabled(settings_.GetBool("net.etw", true));
    ApplyRefreshRate();
    hist_.Resize(static_cast<size_t>(std::clamp<int64_t>(settings_.GetInt("graph.historySeconds", 60) * 1000 / std::max<int64_t>(250, settings_.GetInt("refresh.ms", 1000)), 30, 7200)));

    pages_[0] = MakeOverviewPage(*this);
    pages_[1] = MakeProcessesPage(*this);
    pages_[2] = MakePerformancePage(*this);
    pages_[3] = MakeAppHistoryPage(*this);
    pages_[4] = MakeStartupPage(*this);
    pages_[5] = MakeUsersPage(*this);
    pages_[6] = MakeDetailsPage(*this);
    pages_[7] = MakeServicesPage(*this);
    pages_[8] = MakeHardwarePage(*this);
    pages_[9] = MakeNetworkPage(*this);
    pages_[10] = MakeStoragePage(*this);
    pages_[11] = MakeDiagnosticsPage(*this);
    pages_[12] = MakeSettingsPage(*this);

    recorder_.Configure(settings_.GetBool("history.enabled", false), settings_.GetString("history.folder"),
                        static_cast<int>(settings_.GetInt("history.retentionDays", 7)), static_cast<int>(settings_.GetInt("history.everyN", 5)));

    int start = static_cast<int>(settings_.GetInt("ui.startPage", static_cast<int>(PageId::Processes)));
    if (start < 0 || start >= static_cast<int>(PageId::Count)) start = static_cast<int>(PageId::Processes);
    current_ = static_cast<PageId>(start);
    pages_[start]->OnShow();
    UpdateDemand();
    sampler_->Start();

    tray_ = std::make_unique<Tray>(*this);
    if (settings_.GetBool("tray.enabled", true)) tray_->Show();
    mini_ = std::make_unique<MiniMonitor>(*this);
    if (settings_.GetBool("mini.visible", false)) mini_->Show(true);
    SetTopmost(settings_.GetBool("window.topmost", false));
    a11y::Attach(hwnd_);

    SetTimer(hwnd_, kTimerSave, 60000, nullptr);
    if (startMinimized || settings_.GetBool("startup.minimized", false)) {
        if (settings_.GetBool("window.hideWhenMinimized", false) && tray_->Visible()) { hiddenToTray_ = true; }
        else ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
    } else {
        ShowWindow(hwnd_, nCmdShow);
    }
    UpdateWindow(hwnd_);
    return true;
}

void App::ApplyMica() {
    BOOL dark = theme_.dark;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, NX_DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    // Mica (DWMWA_SYSTEMBACKDROP_TYPE) exists from Windows 11 22H2 (build 22621).
    bool want = settings_.GetBool("ui.mica", true) && WindowsBuild() >= 22621 && !IsRunningUnderWine();
    int backdrop = want ? 2 /*DWMSBT_MAINWINDOW*/ : 1 /*DWMSBT_NONE*/;
    bool ok = SUCCEEDED(DwmSetWindowAttribute(hwnd_, NX_DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop)) && want;
    MARGINS m = ok ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    theme_.mica = ok;
    gfx_.SetTransparentBackground(ok);
}

void App::ApplyTheme() {
    std::wstring mode = settings_.GetString("ui.theme", L"system");
    ThemeMode tm = mode == L"light" ? ThemeMode::Light : mode == L"dark" ? ThemeMode::Dark : ThemeMode::System;
    std::wstring acc = settings_.GetString("ui.accent", L"system");
    uint32_t rgb = acc == L"system" ? 0 : static_cast<uint32_t>(wcstoul(acc.c_str(), nullptr, 16));
    bool micaBefore = theme_.mica;
    theme_ = Theme::Make(tm, rgb, micaBefore);
    ApplyMica();
    theme_ = Theme::Make(tm, rgb, theme_.mica);
    EnableDarkMenus(theme_.dark);
    if (search_) SetWindowTheme(search_, theme_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (searchBrush_) DeleteObject(searchBrush_);
    searchBrush_ = CreateSolidBrush(ToColorRef(theme_.surfaceAlt));
    float scale = static_cast<float>(std::clamp(settings_.GetDouble("ui.scale", 1.0), 0.75, 2.0));
    gfx_.SetDpi(dpi_ * scale);
    if (searchFont_) DeleteObject(searchFont_);
    searchFont_ = CreateFontW(-MulDiv(13, static_cast<int>(dpi_ * scale), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
    if (search_) SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont_), TRUE);
    if (mini_) mini_->OnThemeChanged();
    Invalidate();
}

void App::ApplyRefreshRate() {
    int ms = static_cast<int>(settings_.GetInt("refresh.ms", 1000));
    sampler_->SetIntervalMs(static_cast<uint32_t>(std::clamp(ms, 250, 10000)));
    sampler_->SetPaused(settings_.GetBool("refresh.paused", false));
}

void App::SetTopmost(bool on) {
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    settings_.SetBool("window.topmost", on);
}

void App::SetCompact(bool c) {
    compact_ = c;
    settings_.SetBool("ui.compact", c);
    Invalidate();
}

void App::ToggleMiniMonitor() {
    bool show = !mini_->Visible();
    mini_->Show(show);
    settings_.SetBool("mini.visible", show);
    UpdateDemand();
}

bool App::MiniMonitorVisible() const { return mini_ && mini_->Visible(); }

bool App::PageVisibleInNav(PageId id) const {
    if (id == PageId::Settings) return true;
    for (auto& h : settings_.GetList("nav.hidden"))
        if (h == kNav[static_cast<int>(id)].name) return false;
    return true;
}

void App::SetPageHidden(PageId id, bool hidden) {
    if (id == PageId::Settings) return;
    auto list = settings_.GetList("nav.hidden");
    std::wstring name = kNav[static_cast<int>(id)].name;
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
    if (hidden) list.push_back(name);
    settings_.SetList("nav.hidden", list);
    Invalidate();
}

void App::UpdateDemand() {
    // Window messages (WM_SIZE, WM_SHOWWINDOW) can arrive during creation, before these exist.
    if (!sampler_ || !pages_[static_cast<int>(current_)]) return;
    uint32_t d = demand::Thermal; // cheap; used by overview and diagnostics
    bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    if (visible) d |= pages_[static_cast<int>(current_)]->Demand();
    // Diagnostics rules need disk latency, and the tray/mini monitor need a few metrics even when hidden.
    d |= demand::Disk;
    if (mini_ && mini_->Visible()) d |= demand::GpuSystem | demand::Disk;
    sampler_->SetDemand(d);
    // When the window is not visible and nothing else needs data, slow sampling down.
    int ms = static_cast<int>(settings_.GetInt("refresh.ms", 1000));
    bool background = !visible && !(mini_ && mini_->Visible());
    sampler_->SetIntervalMs(static_cast<uint32_t>(background ? std::max(ms * 4, 4000) : std::clamp(ms, 250, 10000)));
}

// ------------------------------------------------------------------------------------------
// Navigation / status / dialogs
// ------------------------------------------------------------------------------------------
void App::Navigate(PageId id) {
    if (id == current_) return;
    pages_[static_cast<int>(current_)]->OnHide();
    current_ = id;
    pages_[static_cast<int>(id)]->OnShow();
    if (pages_[static_cast<int>(id)]->HasSearch()) {
        SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(pages_[static_cast<int>(id)]->SearchHint()));
        pages_[static_cast<int>(id)]->OnSearch(searchText_);
    }
    UpdateDemand();
    a11y::NotifyPageChanged(hwnd_, pages_[static_cast<int>(id)]->Title());
    Invalidate();
}

void App::ShowStatus(const std::wstring& text, bool error) {
    status_ = text;
    statusError_ = error;
    statusTime_ = GetTickCount64();
    SetTimer(hwnd_, kTimerStatus, 6000, nullptr);
    a11y::Announce(hwnd_, text);
    Invalidate();
}

void App::Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

UINT App::ShowMenu(const std::vector<MenuItem>& items, float x, float y) {
    std::function<HMENU(const std::vector<MenuItem>&)> build = [&](const std::vector<MenuItem>& list) {
        HMENU m = CreatePopupMenu();
        for (auto& it : list) {
            if (it.separator) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); continue; }
            UINT flags = MF_STRING | (it.enabled ? 0 : MF_GRAYED) | (it.checked ? MF_CHECKED : 0);
            if (!it.sub.empty()) AppendMenuW(m, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(build(it.sub)), it.text.c_str());
            else {
                AppendMenuW(m, flags, it.id, it.text.c_str());
                if (it.radio && it.checked) CheckMenuRadioItem(m, it.id, it.id, it.id, MF_BYCOMMAND);
            }
        }
        return m;
    };
    HMENU menu = build(items);
    POINT pt;
    if (x < 0) GetCursorPos(&pt);
    else {
        float s = gfx_.Scale();
        pt = {static_cast<LONG>(x * s), static_cast<LONG>(y * s)};
        ClientToScreen(hwnd_, &pt);
    }
    input_.lDown = input_.rDown = false;
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
    return cmd;
}

bool App::Confirm(const std::wstring& title, const std::wstring& message, const std::wstring& okText, bool danger) {
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.hwndParent = hwnd_;
    tc.pszWindowTitle = L"NEXUS";
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = message.c_str();
    tc.pszMainIcon = danger ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    TASKDIALOG_BUTTON buttons[] = {{IDOK, okText.c_str()}, {IDCANCEL, L"Cancel"}};
    tc.pButtons = buttons;
    tc.cButtons = 2;
    tc.nDefaultButton = danger ? IDCANCEL : IDOK;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    int button = 0;
    input_.lDown = input_.rDown = false;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, nullptr)))
        return MessageBoxW(hwnd_, (title + L"\n\n" + message).c_str(), L"NEXUS", MB_OKCANCEL | (danger ? MB_ICONWARNING : MB_ICONQUESTION)) == IDOK;
    return button == IDOK;
}

void App::Info(const std::wstring& title, const std::wstring& message, bool error) {
    input_.lDown = input_.rDown = false;
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.hwndParent = hwnd_;
    tc.pszWindowTitle = L"NEXUS";
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = message.c_str();
    tc.pszMainIcon = error ? TD_ERROR_ICON : TD_INFORMATION_ICON;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    if (FAILED(TaskDialogIndirect(&tc, nullptr, nullptr, nullptr)))
        MessageBoxW(hwnd_, (title + L"\n\n" + message).c_str(), L"NEXUS", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

void App::RequestElevatedRestart(const std::wstring& reason) {
    if (elevated_) return;
    if (!Confirm(L"Restart NEXUS as administrator?", reason + L"\n\nNEXUS will close and reopen with administrator rights.", L"Restart as administrator"))
        return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId()) + L" --page " + std::to_wstring(static_cast<int>(current_));
    SaveSettings();
    if (ShellRun(hwnd_, exe, args, true)) ExitApp();
}

// ------------------------------------------------------------------------------------------
// Snapshot handling
// ------------------------------------------------------------------------------------------
const ProcHist* App::FindProcHist(const ProcSample& p) const {
    auto it = procHist_.find(p.Key());
    return it == procHist_.end() ? nullptr : &it->second;
}

void App::OnSnapshot() {
    SnapshotPtr s = sampler_->Latest();
    if (!s || (snap_ && s->seq == snap_->seq)) return;
    prevSnap_ = snap_;
    snap_ = s;
    UpdateHistories(*s);
    UpdateProcHistories(*s);
    UpdateAppUsage(*s);
    HistoryPoint hp = MakeHistoryPoint(*s);
    sessionHistory_.push_back(hp);
    if (sessionHistory_.size() > 86400) sessionHistory_.erase(sessionHistory_.begin(), sessionHistory_.begin() + 3600); // bounded (~1 day at 1 s)
    recorder_.Add(hp);
    if (GetTickCount64() - lastDiag_ > 10000) EvaluateDiagnostics();
    for (auto& p : pages_) p->OnSnapshot();
    ProcessWindow::OnSnapshotAll();
    if (tray_) tray_->Update(*s);
    if (mini_ && mini_->Visible()) mini_->Update();
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) Invalidate();
}

void App::UpdateHistories(const Snapshot& s) {
    auto& h = hist_;
    size_t c = h.cap;
    h.cpu.Push(s.cpu.total);
    h.cpuKernel.Push(s.cpu.kernel);
    h.cpuMHz.Push(static_cast<float>(s.cpu.currentMHz));
    float memPct = s.mem.total ? 100.f * s.mem.InUse() / static_cast<float>(s.mem.total) : 0.f;
    h.memPct.Push(memPct);
    h.memUsed.Push(static_cast<float>(s.mem.InUse() / (1024.0 * 1024.0 * 1024.0)));
    h.commitPct.Push(s.mem.commitLimit ? 100.f * s.mem.commit / static_cast<float>(s.mem.commitLimit) : 0.f);
    h.pagesPerSec.Push(s.mem.pagesPerSec);
    float tz = -1;
    for (auto& [n, v] : s.thermalZonesC) tz = std::max(tz, static_cast<float>(v));
    h.thermal.Push(std::max(0.f, tz));
    if (h.cores.size() != s.cpu.cores.size()) h.cores.assign(s.cpu.cores.size(), RingBuffer<float>(c));
    for (size_t i = 0; i < s.cpu.cores.size(); ++i) h.cores[i].Push(s.cpu.cores[i]);
    double rd = 0, wr = 0;
    for (auto& d : s.disks) {
        auto it = h.disks.find(d.index);
        if (it == h.disks.end()) it = h.disks.emplace(d.index, DiskHist(c)).first;
        it->second.active.Push(d.activePct);
        it->second.read.Push(d.readBps);
        it->second.write.Push(d.writeBps);
        it->second.latency.Push(d.latencyMs);
        rd += d.readBps;
        wr += d.writeBps;
    }
    if (s.diskCollected) { h.diskTotalRead.Push(static_cast<float>(rd)); h.diskTotalWrite.Push(static_cast<float>(wr)); }
    double ts = 0, tr = 0;
    for (auto& n : s.nics) {
        auto it = h.nics.find(n.luid);
        if (it == h.nics.end()) it = h.nics.emplace(n.luid, NicHist(c)).first;
        it->second.send.Push(static_cast<float>(n.sendBps));
        it->second.recv.Push(static_cast<float>(n.recvBps));
        ts += n.sendBps;
        tr += n.recvBps;
    }
    h.netTotalSend.Push(static_cast<float>(ts));
    h.netTotalRecv.Push(static_cast<float>(tr));
    float gmax = 0;
    for (auto& g : s.gpus) {
        auto it = h.gpus.find(g.luid);
        if (it == h.gpus.end()) it = h.gpus.emplace(g.luid, GpuHist(c)).first;
        it->second.util.Push(g.util);
        it->second.dedicated.Push(static_cast<float>(g.dedicatedUsed / (1024.0 * 1024.0 * 1024.0)));
        it->second.shared.Push(static_cast<float>(g.sharedUsed / (1024.0 * 1024.0 * 1024.0)));
        it->second.temp.Push(static_cast<float>(std::max(0.0, g.temperatureC)));
        for (auto& e : g.engines) {
            std::wstring key = e.name + L"#" + std::to_wstring(e.index);
            auto ei = it->second.engines.find(key);
            if (ei == it->second.engines.end()) ei = it->second.engines.emplace(key, RingBuffer<float>(c)).first;
            ei->second.Push(e.util);
        }
        gmax = std::max(gmax, g.util);
    }
    if (s.gpuCollected) h.gpuMax.Push(gmax);
}

void App::UpdateProcHistories(const Snapshot& s) {
    for (auto& p : s.procs) {
        auto& ph = procHist_[p.Key()];
        if (ph.name.empty()) { ph.name = p.Name(); ph.pid = p.pid; }
        ph.lastSeq = s.seq;
        float memMB = static_cast<float>(p.privateBytes / (1024.0 * 1024.0));
        ph.cpu.Push(p.cpu);
        ph.mem.Push(memMB);
        ph.io.Push(p.ioReadRate + p.ioWriteRate);
        ph.gpu.Push(p.gpu);
        ph.net.Push(p.netSend >= 0 ? p.netSend + p.netRecv : 0.f);
        ph.cpuAcc += p.cpu;
        ph.memAcc += memMB;
        if (++ph.accN >= 10) {
            ph.cpuLong.Push(ph.cpuAcc / ph.accN);
            ph.memLong.Push(ph.memAcc / ph.accN);
            ph.cpuAcc = ph.memAcc = 0;
            ph.accN = 0;
        }
    }
    // Keep exited processes' history for a short while so the details window can still show it.
    for (auto it = procHist_.begin(); it != procHist_.end();) {
        if (s.seq - it->second.lastSeq > 30) it = procHist_.erase(it);
        else ++it;
    }
}

void App::UpdateAppUsage(const Snapshot& s) {
    for (auto& p : s.procs) {
        if (!p.meta || p.meta->path.empty() || p.pid == 0) continue;
        std::wstring key = ToLower(p.meta->path);
        auto& u = appUsage_[key];
        if (u.name.empty()) {
            u.name = p.meta->DisplayName();
            u.path = p.meta->path;
            u.publisher = p.meta->company;
            u.firstSeen = s.time;
        }
        if (p.isNew) ++u.launches;
        u.lastSeen = s.time;
        u.cpuSeconds += p.cpu / 100.0 * s.interval * std::max<size_t>(1, s.cpu.cores.size());
        u.gpuSeconds += p.gpu / 100.0 * s.interval;
        u.ioBytes += (p.ioReadRate + p.ioWriteRate) * s.interval;
        if (p.netSend >= 0) u.netBytes += (p.netSend + p.netRecv) * s.interval;
    }
}

void App::ResetAppHistory() {
    appUsage_.clear();
    Invalidate();
}

void App::RefreshDiagnosticsNow() { EvaluateDiagnostics(); Invalidate(); }

void App::EvaluateDiagnostics() {
    lastDiag_ = GetTickCount64();
    if (!snap_) return;
    EnsureEventCounts();
    DiagInput in;
    in.intervalSec = snap_->interval > 0.05 ? snap_->interval : 1.0;
    in.logicalCpus = static_cast<unsigned>(std::max<size_t>(1, snap_->cpu.cores.size()));
    std::vector<float> tmp;
    hist_.cpu.Tail(hist_.cpu.Size(), in.totalCpu);
    hist_.commitPct.Tail(hist_.commitPct.Size(), in.commitPct);
    std::vector<float> mem;
    hist_.memPct.Tail(hist_.memPct.Size(), mem);
    for (float m : mem) in.availPct.push_back(100.f - m);
    hist_.pagesPerSec.Tail(hist_.pagesPerSec.Size(), in.hardFaults);
    for (auto& p : snap_->procs) {
        auto* ph = FindProcHist(p);
        if (!ph) continue;
        DiagInput::Proc dp;
        dp.pid = p.pid;
        dp.name = p.Name();
        ph->cpu.Tail(ph->cpu.Size(), dp.cpu);
        // Memory trend uses the long series (10-sample averages), rescaled to its own interval.
        ph->memLong.Tail(ph->memLong.Size(), dp.privateMB);
        if (dp.cpu.size() < 30 && dp.privateMB.size() < 30) continue;
        in.procs.push_back(std::move(dp));
    }
    // The long memory series is sampled every 10 intervals; evaluate it separately with the right interval.
    DiagInput memIn;
    memIn.intervalSec = in.intervalSec * 10;
    memIn.logicalCpus = in.logicalCpus;
    for (auto& p : in.procs) {
        DiagInput::Proc mp;
        mp.pid = p.pid;
        mp.name = p.name;
        mp.privateMB = std::move(p.privateMB);
        memIn.procs.push_back(std::move(mp));
        p.privateMB.clear();
    }
    for (auto& [idx, d] : hist_.disks) {
        DiagInput::Disk dd;
        dd.name = L"Disk " + std::to_wstring(idx);
        d.latency.Tail(d.latency.Size(), dd.latencyMs);
        d.active.Tail(d.active.Size(), dd.activePct);
        in.disks.push_back(std::move(dd));
    }
    // Adapter error deltas since monitoring started.
    static std::map<uint64_t, std::array<uint64_t, 3>> base;
    for (auto& n : snap_->nics) {
        auto it = base.find(n.luid);
        if (it == base.end()) it = base.emplace(n.luid, std::array<uint64_t, 3>{n.inErrors + n.outErrors, n.inDiscards + n.outDiscards, n.inPackets + n.outPackets}).first;
        DiagInput::Adapter a;
        a.name = n.alias;
        a.errors = n.inErrors + n.outErrors - std::min(n.inErrors + n.outErrors, it->second[0]);
        a.discards = n.inDiscards + n.outDiscards - std::min(n.inDiscards + n.outDiscards, it->second[1]);
        a.packets = n.inPackets + n.outPackets - std::min(n.inPackets + n.outPackets, it->second[2]);
        in.adapters.push_back(a);
    }
    if (static_.disksLoaded) {
        for (auto& d : static_.disks) {
            for (auto& v : d.volumes)
                if (!v.letter.empty() && v.total) in.volumes.push_back({v.letter, v.free, v.total});
            auto hi = static_.health.find(d.index);
            if (hi != static_.health.end() && hi->second.available)
                in.storageHealth.push_back({d.model, hi->second.criticalWarning, hi->second.percentUsed, hi->second.availableSpare, hi->second.spareThreshold, hi->second.temperatureC});
        }
    } else {
        EnsureDiskInfo(true);
    }
    for (auto& [n, v] : snap_->thermalZonesC) in.maxThermalZoneC = std::max(in.maxThermalZoneC, v);
    for (auto& g : snap_->gpus) if (g.temperatureC > 0) in.gpuTempsC.push_back(g.temperatureC);
    in.appCrashes24h = events.crashes24h;
    in.unexpectedShutdowns30d = events.kernelPower30d;
    in.whea30d = events.whea30d;

    alerts_ = nx::EvaluateDiagnostics(in);
    for (auto& a : nx::EvaluateDiagnostics(memIn))
        if (a.category == L"Memory" && a.pid) alerts_.push_back(std::move(a));
    std::stable_sort(alerts_.begin(), alerts_.end(), [](const DiagAlert& a, const DiagAlert& b) { return a.severity > b.severity; });
    if (tray_ && settings_.GetBool("alerts.notify", false)) tray_->NotifyAlerts(alerts_);
}

// ------------------------------------------------------------------------------------------
// Lazy static information
// ------------------------------------------------------------------------------------------
void App::EnsureSystemInfo() {
    if (static_.sysLoaded || static_.sysLoading) return;
    static_.sysLoading = true;
    auto out = std::make_shared<std::pair<SystemStatic, MemStatic>>();
    worker::Run([out] {
        out->first = QuerySystem();
        out->second = QueryMemStatic(out->first.smbios);
    }, [this, out] {
        static_.sys = std::move(out->first);
        static_.mem = std::move(out->second);
        static_.sysLoaded = true;
        static_.sysLoading = false;
        Invalidate();
    });
}

void App::EnsureGpuInfo() {
    if (static_.gpusLoaded || static_.gpusLoading) return;
    static_.gpusLoading = true;
    auto out = std::make_shared<std::vector<GpuStatic>>();
    worker::Run([out] { *out = QueryGpus(); }, [this, out] {
        static_.gpus = std::move(*out);
        static_.gpusLoaded = true;
        static_.gpusLoading = false;
        Invalidate();
    });
}

void App::EnsureDiskInfo(bool withHealth) {
    if (static_.disksLoading) return;
    if (static_.disksLoaded && (!withHealth || !static_.health.empty())) return;
    static_.disksLoading = true;
    auto out = std::make_shared<std::pair<std::vector<DiskStatic>, std::map<int, StorageHealth>>>();
    worker::Run([out, withHealth] {
        out->first = QueryDisks();
        if (withHealth)
            for (auto& d : out->first) out->second[d.index] = QueryStorageHealth(d.index);
    }, [this, out] {
        static_.disks = std::move(out->first);
        if (!out->second.empty()) static_.health = std::move(out->second);
        static_.disksLoaded = true;
        static_.disksLoading = false;
        Invalidate();
    });
}

void App::EnsureNicInfo(bool force) {
    if (static_.nicsLoading) return;
    if (static_.nicsLoaded && !force && GetTickCount64() - static_.nicsTime < 30000) return;
    static_.nicsLoading = true;
    auto out = std::make_shared<std::vector<NicStatic>>();
    worker::Run([out] { *out = QueryNics(); }, [this, out] {
        static_.nics = std::move(*out);
        static_.nicsLoaded = true;
        static_.nicsLoading = false;
        static_.nicsTime = GetTickCount64();
        Invalidate();
    });
}

void App::EnsureEventCounts() {
    if (events.loading || (events.loaded && GetTickCount64() - lastDiag_ < 600000 && events.crashes24h >= 0)) return;
    if (events.loaded) {
        static uint64_t last = 0;
        if (GetTickCount64() - last < 600000) return; // refresh every 10 minutes
        last = GetTickCount64();
    }
    events.loading = true;
    auto out = std::make_shared<std::array<int, 4>>();
    worker::Run([out] {
        (*out)[0] = CountEvents(L"Application", L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 86400000]]]", 500);
        (*out)[1] = CountEvents(L"System", L"*[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41 and TimeCreated[timediff(@SystemTime) <= 2592000000]]]", 500);
        (*out)[2] = CountEvents(L"System", L"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and TimeCreated[timediff(@SystemTime) <= 2592000000]]]", 500);
        (*out)[3] = CountEvents(L"System", L"*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 86400000]]]", 1000);
    }, [this, out] {
        events.crashes24h = (*out)[0];
        events.kernelPower30d = (*out)[1];
        events.whea30d = (*out)[2];
        events.errors24h = (*out)[3];
        events.loaded = true;
        events.loading = false;
        Invalidate();
    });
}

// ------------------------------------------------------------------------------------------
// Painting
// ------------------------------------------------------------------------------------------
void App::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (!sampler_ || !pages_[static_cast<int>(current_)]) { EndPaint(hwnd_, &ps); return; } // still creating
    if (!gfx_.Begin()) { EndPaint(hwnd_, &ps); return; }
    if (!ui_) ui_ = std::make_unique<Ui>(gfx_, theme_, input_);
    Ui& ui = *ui_;
    ui.BeginFrame();
    const float W = gfx_.Width(), H = gfx_.Height();
    gfx_.Clear(theme_.mica ? Color{0, 0, 0, 0} : theme_.nav.a > 0 ? theme_.nav : theme_.window);

    float navW = navCollapsed_ ? 52.f : 214.f;
    Rect nav{0, 0, navW, H};
    DrawNav(ui, nav);

    Rect content{navW, 0, W - navW, H};
    gfx_.FillRound({content.x, content.y + 0, content.w + 12, content.h + 12}, 8.f, theme_.window);
    gfx_.StrokeRound({content.x, content.y, content.w + 12, content.h + 12}, 8.f, theme_.cardBorder);
    Rect status = content;
    status = Rect{content.x, H - 26, content.w, 26};
    Rect top{content.x, 0, content.w, 52};
    Rect body{content.x + 16, top.B(), content.w - 32, status.y - top.B()};
    Page* page = pages_[static_cast<int>(current_)].get();
    DrawTopBar(ui, top);
    gfx_.PushClip({content.x, top.B(), content.w, status.y - top.B()});
    page->Draw(ui, body);
    gfx_.PopClip();
    DrawStatusBar(ui, status);
    ui.EndFrame();
    gfx_.End();
    EndPaint(hwnd_, &ps);
    input_.ClearEvents();
    auto d = ui.TakeDeferred();
    for (auto& f : d) deferred_.push_back(std::move(f));
    // Actions that open menus/dialogs run outside WM_PAINT.
    if (!deferred_.empty()) PostMessageW(hwnd_, WM_NX_DEFERRED, 0, 0);
    lastHot_ = ui.HitTest(input_.mx, input_.my);
}

void App::RunDeferred() {
    auto list = std::move(deferred_);
    deferred_.clear();
    for (auto& f : list) f();
    if (!list.empty()) Invalidate();
}

void App::DrawNav(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    if (t.nav.a > 0) g.Fill(r, t.nav);
    float y = 10;
    // Hamburger / collapse toggle.
    if (ui.IconButton(Id("nav.toggle"), {r.x + 8, y, 36, 34}, 0xE700, navCollapsed_ ? L"Expand navigation" : L"Collapse navigation")) {
        navCollapsed_ = !navCollapsed_;
        settings_.SetBool("ui.nav.collapsed", navCollapsed_);
    }
    if (!navCollapsed_) g.Text(L"NEXUS", {r.x + 52, y, r.w - 60, 34}, Font::Subtitle, t.text);
    y += 46;
    float itemH = compact_ ? 32.f : 38.f;
    Rect list{r.x, y, r.w, r.h - y - 54};
    float contentH = 0;
    for (auto& e : kNav)
        if (e.id != PageId::Settings && PageVisibleInNav(e.id)) contentH += itemH + 2;
    float off = navScroll_;
    ui.Scroll(Id("nav.scroll"), list, contentH, off);
    navScroll_ = off;
    g.PushClip(list);
    float iy = list.y - navScroll_;
    int shortcut = 1;
    auto item = [&](const NavEntry& e, float yy) {
        Rect ir{r.x + 6, yy, r.w - 12, itemH};
        WidgetId id = IdMix(Id("nav.item"), static_cast<int>(e.id));
        bool hot = ui.Hit(id, ir);
        bool sel = current_ == e.id;
        if (sel) g.FillRound(ir, 5.f, t.dark ? Color{1, 1, 1, 0.07f} : Color{0, 0, 0, 0.05f});
        else if (hot) g.FillRound(ir, 5.f, t.dark ? Color{1, 1, 1, 0.045f} : Color{0, 0, 0, 0.035f});
        if (sel) g.FillRound({ir.x, ir.y + itemH / 2 - 8, 3, 16}, 1.5f, t.accent);
        g.Glyph(e.glyph, {ir.x + 8, ir.y, 24, ir.h}, Font::Icon, sel ? t.accentText : t.text);
        if (!navCollapsed_) g.Text(e.name, {ir.x + 42, ir.y, ir.w - 46, ir.h}, sel ? Font::BodySemi : Font::Body, t.text);
        else ui.Tooltip(id, e.name);
        if (ui.Clicked(id)) Navigate(e.id);
        if (ui.RightClicked(id) && e.id != PageId::Settings) {
            PageId pid = e.id;
            ui.Defer([this, pid] {
                UINT c = ShowMenu({{1, L"Hide from navigation"}, {2, L"Set as start page"}});
                if (c == 1) SetPageHidden(pid, true);
                if (c == 2) settings_.SetInt("ui.startPage", static_cast<int>(pid));
            });
        }
    };
    for (auto& e : kNav) {
        if (e.id == PageId::Settings || !PageVisibleInNav(e.id)) continue;
        item(e, iy);
        iy += itemH + 2;
        ++shortcut;
    }
    g.PopClip();
    item(kNav[static_cast<int>(PageId::Settings)], r.B() - itemH - 12);
}

void App::DrawTopBar(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    Page* page = pages_[static_cast<int>(current_)].get();
    g.Text(page->Title(), {r.x + 18, r.y + 6, 260, r.h - 6}, Font::Title, t.text);
    float right = r.R() - 12;
    auto btn = [&](const char* key, wchar_t glyph, const std::wstring& tip, bool toggled) {
        right -= 36;
        return ui.IconButton(Id(key), {right, r.y + 12, 34, 32}, glyph, tip, true, toggled);
    };
    bool paused = sampler_->Paused();
    if (btn("top.pause", paused ? 0xE768 : 0xE769, paused ? L"Resume updates (Ctrl+P)" : L"Pause updates (Ctrl+P)", paused)) {
        bool p = !paused;
        settings_.SetBool("refresh.paused", p);
        sampler_->SetPaused(p);
        ShowStatus(p ? L"Updates paused" : L"Updates resumed");
    }
    if (btn("top.refresh", 0xE72C, L"Refresh now (F5)", false)) sampler_->RefreshNow();
    bool top = settings_.GetBool("window.topmost", false);
    if (btn("top.pin", 0xE718, top ? L"Always on top: on (Ctrl+T)" : L"Always on top: off (Ctrl+T)", top)) SetTopmost(!top);
    if (btn("top.mini", 0xE8A7, L"Mini monitor (Ctrl+M)", MiniMonitorVisible())) ToggleMiniMonitor();
    right -= 8;
    float runW = 132;
    right -= runW;
    if (ui.Button(Id("top.run"), {right, r.y + 12, runW, 32}, L"Run new task", 0xE710)) ui.Defer([this] { RunNewTaskDialog(); });
    if (!elevated_) {
        right -= 40;
        if (ui.IconButton(Id("top.admin"), {right, r.y + 12, 34, 32}, 0xE7EF, L"Restart as administrator (shows all details, enables per-process network, SMART)"))
            ui.Defer([this] { RequestElevatedRestart(L"Some information and actions require administrator rights."); });
    }
    float searchX = r.x + 18 + std::min(260.f, g.Measure(page->Title(), Font::Title) + 24);
    float searchW = std::min(420.f, right - searchX - 12);
    bool showSearch = page->HasSearch() && searchW > 120;
    Rect sr{searchX, r.y + 12, searchW, 32};
    if (showSearch) {
        g.FillRound(sr, 4.f, t.surfaceAlt);
        g.StrokeRound(sr, 4.f, t.cardBorder);
        g.FillRound({sr.x + 1, sr.B() - 2, sr.w - 2, 1.5f}, 1.f, GetFocus() == search_ ? t.accent : t.textTertiary.Alpha(0.4f));
        g.Glyph(0xE721, {sr.x + 6, sr.y, 22, sr.h}, Font::IconSmall, t.textSecondary);
    }
    LayoutSearch({sr.x + 30, sr.y + 7, sr.w - 38, sr.h - 12}, showSearch);
}

void App::LayoutSearch(const Rect& r, bool visible) {
    if (!search_) return;
    if (!visible) {
        if (IsWindowVisible(search_)) ShowWindow(search_, SW_HIDE);
        return;
    }
    float s = gfx_.Scale();
    RECT want{static_cast<LONG>(r.x * s), static_cast<LONG>(r.y * s), static_cast<LONG>(r.R() * s), static_cast<LONG>(r.B() * s)};
    RECT cur;
    GetWindowRect(search_, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    if (memcmp(&cur, &want, sizeof cur) != 0 || !IsWindowVisible(search_))
        SetWindowPos(search_, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void App::DrawStatusBar(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    g.Line(r.x, r.y, r.R(), r.y, t.divider);
    std::wstring left;
    if (!status_.empty()) left = status_;
    else if (snap_) {
        left = L"Processes " + std::to_wstring(snap_->cpu.processes) + L"   CPU " + FormatPercent(snap_->cpu.total) + L"   Memory " +
               FormatPercent(snap_->mem.total ? 100.0 * snap_->mem.InUse() / snap_->mem.total : 0) + L"   Up " + FormatUptime(snap_->cpu.uptimeSec);
    }
    g.Text(left, {r.x + 18, r.y, r.w * 0.65f, r.h}, Font::Small, statusError_ && !status_.empty() ? t.critical : t.textSecondary);
    std::wstring right;
    int ms = static_cast<int>(settings_.GetInt("refresh.ms", 1000));
    if (sampler_->Paused()) right = L"Paused";
    else right = ms < 1000 ? L"Update: high (0.5 s)" : ms <= 1000 ? L"Update: normal (1 s)" : L"Update: " + std::to_wstring(ms / 1000) + L" s";
    if (snap_) {
        wchar_t b[64];
        swprintf(b, 64, L"   Sample %.1f ms", snap_->collectMs);
        right += b;
    }
    if (elevated_) right += L"   Administrator";
    WidgetId sid = Id("status.speed");
    Rect rr{r.R() - 300, r.y, 284, r.h};
    bool hot = ui.Hit(sid, rr);
    g.Text(right, rr, Font::Small, hot ? t.text : t.textTertiary, Align::Right);
    ui.Tooltip(sid, L"Click to change the update speed. 'Sample' is the time NEXUS spent collecting the last snapshot.");
    if (ui.Clicked(sid)) {
        ui.Defer([this, ms] {
            bool paused = sampler_->Paused();
            UINT c = ShowMenu({{1, L"High (0.5 s)", ms == 500 && !paused, true, false, true},
                               {2, L"Normal (1 s)", ms == 1000 && !paused, true, false, true},
                               {3, L"Low (2 s)", ms == 2000 && !paused, true, false, true},
                               {4, L"Very low (4 s)", ms == 4000 && !paused, true, false, true},
                               MenuItem::Sep(),
                               {5, L"Paused", paused}});
            int nms[] = {0, 500, 1000, 2000, 4000};
            if (c >= 1 && c <= 4) {
                settings_.SetInt("refresh.ms", nms[c]);
                settings_.SetBool("refresh.paused", false);
                ApplyRefreshRate();
                UpdateDemand();
            } else if (c == 5) {
                settings_.SetBool("refresh.paused", !paused);
                ApplyRefreshRate();
            }
        });
    }
}

// ------------------------------------------------------------------------------------------
// Input
// ------------------------------------------------------------------------------------------
void App::HandleMouse(UINT msg, WPARAM wp, LPARAM lp) {
    float s = gfx_.Scale();
    if (msg != WM_MOUSEWHEEL && msg != WM_MOUSELEAVE) {
        input_.mx = GET_X_LPARAM(lp) / s;
        input_.my = GET_Y_LPARAM(lp) / s;
    }
    input_.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    input_.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool immediate = true;
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        WidgetId hot = ui_ ? ui_->HitTest(input_.mx, input_.my) : 0;
        bool dragging = input_.lDown;
        if (hot != lastHot_) {
            lastHot_ = hot;
            input_.tooltipReady = false;
            KillTimer(hwnd_, kTimerTooltip);
            if (hot) SetTimer(hwnd_, kTimerTooltip, 550, nullptr);
            Invalidate();
        } else if (dragging) {
            Invalidate();
        } else if (hot && input_.tooltipReady) {
            Invalidate(); // tooltip follows the cursor / graph read-out
        }
        immediate = false;
        break;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        input_.mx = input_.my = -1;
        lastHot_ = 0;
        input_.tooltipReady = false;
        Invalidate();
        immediate = false;
        break;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd_);
        if (GetFocus() == search_) SetFocus(hwnd_);
        input_.lDown = true;
        input_.lPressed = true;
        input_.tooltipReady = false;
        break;
    case WM_LBUTTONDBLCLK:
        input_.lDown = true;
        input_.lPressed = true;
        input_.dblClick = true;
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        input_.lDown = false;
        input_.lReleased = true;
        break;
    case WM_RBUTTONDOWN:
        input_.rDown = true;
        input_.rPressed = true;
        break;
    case WM_RBUTTONUP:
        input_.rDown = false;
        input_.rReleased = true;
        break;
    case WM_MOUSEWHEEL:
        input_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
        input_.tooltipReady = false;
        break;
    }
    if (immediate) {
        // Process the event in its own frame so clicks are never merged or lost.
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

bool App::HandleKey(UINT vk) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && vk >= '1' && vk <= '9') {
        int idx = vk - '1';
        int n = 0;
        for (auto& e : kNav) {
            if (e.id == PageId::Settings || !PageVisibleInNav(e.id)) continue;
            if (n++ == idx) { Navigate(e.id); return true; }
        }
        return true;
    }
    if (ctrl && vk == '0') { Navigate(PageId::Overview); return true; }
    if (ctrl && vk == VK_OEM_COMMA) { Navigate(PageId::Settings); return true; }
    if ((ctrl && vk == 'F') || vk == VK_F3) {
        if (pages_[static_cast<int>(current_)]->HasSearch()) { SetFocus(search_); SendMessageW(search_, EM_SETSEL, 0, -1); }
        return true;
    }
    if (vk == VK_F5) { sampler_->RefreshNow(); return true; }
    if (ctrl && vk == 'P') {
        bool p = !sampler_->Paused();
        settings_.SetBool("refresh.paused", p);
        sampler_->SetPaused(p);
        ShowStatus(p ? L"Updates paused" : L"Updates resumed");
        return true;
    }
    if (ctrl && vk == 'T') { SetTopmost(!settings_.GetBool("window.topmost", false)); Invalidate(); return true; }
    if (ctrl && vk == 'M') { ToggleMiniMonitor(); Invalidate(); return true; }
    if (ctrl && vk == 'N') { RunNewTaskDialog(); return true; }
    if (ctrl && vk == 'E') { ExportReport(); return true; }
    if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) {
        double sc = settings_.GetDouble("ui.scale", 1.0) + ((vk == VK_OEM_PLUS || vk == VK_ADD) ? 0.1 : -0.1);
        settings_.SetDouble("ui.scale", std::clamp(sc, 0.75, 2.0));
        ApplyTheme();
        ShowStatus(L"UI scale " + FormatPercent(std::clamp(sc, 0.75, 2.0) * 100));
        return true;
    }
    if (ctrl && vk == 'B') { navCollapsed_ = !navCollapsed_; settings_.SetBool("ui.nav.collapsed", navCollapsed_); Invalidate(); return true; }
    if (vk == VK_ESCAPE && !searchText_.empty()) { SetWindowTextW(search_, L""); return true; }
    if (pages_[static_cast<int>(current_)]->OnKey(vk, ctrl, shift)) { Invalidate(); return true; }
    return false;
}

void App::OnDpiChanged(UINT dpi, const RECT* r) {
    dpi_ = static_cast<float>(dpi);
    float scale = static_cast<float>(std::clamp(settings_.GetDouble("ui.scale", 1.0), 0.75, 2.0));
    gfx_.SetDpi(dpi_ * scale);
    icons_.SetPixelSize(dpi_ > 120 ? (dpi_ > 168 ? 32 : 20) : 16);
    if (r) SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyTheme();
}

void App::ShowFromTray() {
    hiddenToTray_ = false;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    UpdateDemand();
    Invalidate();
}

void App::ExitApp() {
    exiting_ = true;
    DestroyWindow(hwnd_);
}

LRESULT App::WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        gfx_.Resize(LOWORD(lp), HIWORD(lp));
        if (wp == SIZE_MINIMIZED) {
            minimized_ = true;
            if (settings_.GetBool("window.hideWhenMinimized", false) && tray_ && tray_->Visible()) {
                ShowWindow(hwnd_, SW_HIDE);
                hiddenToTray_ = true;
            }
        } else if (minimized_) {
            minimized_ = false;
        }
        UpdateDemand();
        Invalidate();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {MulDiv(560, static_cast<int>(dpi_), 96), MulDiv(400, static_cast<int>(dpi_), 96)};
        return 0;
    }
    case WM_DPICHANGED: OnDpiChanged(HIWORD(wp), reinterpret_cast<RECT*>(lp)); return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED: ApplyTheme(); return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSELEAVE:
        HandleMouse(m, wp, lp);
        return 0;
    case WM_MOUSEWHEEL: {
        HandleMouse(m, wp, lp);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (HandleKey(static_cast<UINT>(wp))) return 0;
        break;
    case WM_TIMER:
        if (wp == kTimerStatus) { KillTimer(h, kTimerStatus); status_.clear(); Invalidate(); }
        else if (wp == kTimerTooltip) { KillTimer(h, kTimerTooltip); input_.tooltipReady = true; Invalidate(); }
        else if (wp == kTimerSave) { SaveSettings(); recorder_.Flush(); }
        return 0;
    case WM_NX_SNAPSHOT: OnSnapshot(); return 0;
    case WM_NX_DEFERRED: RunDeferred(); return 0;
    case WM_NX_WORKER: worker::DispatchCompletion(lp); return 0;
    case WM_NX_ACTIVATE: ShowFromTray(); return 0;
    case WM_NX_TRAY: if (tray_) tray_->OnMessage(wp, lp); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
            int n = GetWindowTextLengthW(search_);
            std::wstring s(n, L'\0');
            GetWindowTextW(search_, s.data(), n + 1);
            searchText_ = s;
            pages_[static_cast<int>(current_)]->OnSearch(s);
            Invalidate();
            return 0;
        }
        if (LOWORD(wp) == 100 && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) { Invalidate(); return 0; }
        break;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, ToColorRef(Opaque(theme_.text, theme_.surfaceAlt)));
        SetBkColor(dc, ToColorRef(theme_.surfaceAlt));
        return reinterpret_cast<LRESULT>(searchBrush_);
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && hiddenToTray_ == false) UpdateDemand();
        break;
    case WM_SHOWWINDOW: UpdateDemand(); break;
    case WM_GETOBJECT: {
        LRESULT r = a11y::HandleGetObject(h, wp, lp);
        if (r) return r;
        break;
    }
    case WM_QUERYENDSESSION: SaveSettings(); recorder_.Flush(); return TRUE;
    case WM_CLOSE:
        if (!exiting_ && settings_.GetBool("window.closeToTray", false) && tray_ && tray_->Visible()) {
            ShowWindow(hwnd_, SW_HIDE);
            hiddenToTray_ = true;
            UpdateDemand();
            return 0;
        }
        break;
    case WM_DESTROY:
        SaveSettings();
        recorder_.Flush();
        if (tray_) tray_->Hide();
        if (mini_) mini_->Destroy();
        ProcessWindow::CloseAll();
        if (sampler_) sampler_->Stop();
        worker::Shutdown();
        a11y::Detach(h);
        icons_.Shutdown();
        ui_.reset();
        gfx_.Shutdown();
        if (searchBrush_) DeleteObject(searchBrush_);
        if (searchFont_) DeleteObject(searchFont_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int App::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Route keyboard shortcuts from the search box to the app (Esc, Enter, arrows, Ctrl+...).
        if (msg.message == WM_KEYDOWN && msg.hwnd == search_) {
            UINT vk = static_cast<UINT>(msg.wParam);
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (vk == VK_ESCAPE) { SetWindowTextW(search_, L""); SetFocus(hwnd_); continue; }
            if (vk == VK_DOWN || vk == VK_UP || vk == VK_RETURN || vk == VK_F5 || vk == VK_NEXT || vk == VK_PRIOR ||
                (ctrl && vk != 'A' && vk != 'C' && vk != 'V' && vk != 'X' && vk != 'Z' && vk != VK_LEFT && vk != VK_RIGHT && vk != VK_BACK && vk != VK_DELETE)) {
                if (HandleKey(vk)) continue;
            }
        }
        if (msg.message == WM_KEYDOWN && msg.hwnd != hwnd_ && msg.hwnd != search_ && GetAncestor(msg.hwnd, GA_ROOT) == hwnd_) {
            // Other native edits inside pages: allow Escape to return focus.
            if (msg.wParam == VK_ESCAPE) { SetFocus(hwnd_); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ------------------------------------------------------------------------------------------
// NativeEdit
// ------------------------------------------------------------------------------------------
NativeEdit::~NativeEdit() { if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_); }

void NativeEdit::Place(App& app, const Rect& r, const wchar_t* cue) {
    float s = app.G().Scale();
    if (!hwnd_) {
        hwnd_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, app.Hwnd(), reinterpret_cast<HMENU>(101), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hwnd_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        SendMessageW(hwnd_, WM_SETFONT, SendMessageW(GetDlgItem(app.Hwnd(), 100), WM_GETFONT, 0, 0), TRUE);
        SetWindowTheme(hwnd_, app.Th().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    Gfx& g = app.G();
    g.FillRound(r, 4.f, app.Th().surfaceAlt);
    g.StrokeRound(r, 4.f, app.Th().cardBorder);
    g.FillRound({r.x + 1, r.B() - 2, r.w - 2, 1.5f}, 1.f, Focused() ? app.Th().accent : app.Th().textTertiary.Alpha(0.4f));
    RECT want{static_cast<LONG>((r.x + 8) * s), static_cast<LONG>((r.y + 6) * s), static_cast<LONG>((r.R() - 8) * s), static_cast<LONG>((r.B() - 5) * s)};
    if (memcmp(&want, &last_, sizeof want) != 0 || !IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        last_ = want;
    }
}

void NativeEdit::Hide() { if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE); }

std::wstring NativeEdit::Text() const {
    if (!hwnd_) return {};
    int n = GetWindowTextLengthW(hwnd_);
    std::wstring s(n, L'\0');
    GetWindowTextW(hwnd_, s.data(), n + 1);
    return s;
}

void NativeEdit::SetText(const std::wstring& s) { if (hwnd_) SetWindowTextW(hwnd_, s.c_str()); }
bool NativeEdit::Focused() const { return hwnd_ && GetFocus() == hwnd_; }

} // namespace nx
