#pragma once
#include "core/DiagnosticsRules.h"
#include "core/Format.h"
#include "core/RingBuffer.h"
#include "core/Settings.h"
#include "data/Hardware.h"
#include "data/History.h"
#include "data/Sampler.h"
#include "ui/Gfx.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nx {

constexpr UINT WM_NX_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_NX_WORKER = WM_APP + 2;
constexpr UINT WM_NX_TRAY = WM_APP + 3;
constexpr UINT WM_NX_ACTIVATE = WM_APP + 4; // sent by a second instance
constexpr UINT WM_NX_DEFERRED = WM_APP + 5;

enum class PageId : int { Overview, Processes, Performance, AppHistory, Startup, Users, Details, Services, Hardware, Network, Storage, Diagnostics, Settings, Count };

class Page;
class App;
class Tray;
class MiniMonitor;

struct MenuItem {
    UINT id = 0;
    std::wstring text;
    bool checked = false, enabled = true, separator = false, radio = false;
    std::vector<MenuItem> sub;
    static MenuItem Sep() { MenuItem m; m.separator = true; return m; }
};

// Rolling histories kept on the UI thread. Capacity follows Settings > Graph history length.
struct DiskHist { RingBuffer<float> active, read, write, latency; explicit DiskHist(size_t c = 60) : active(c), read(c), write(c), latency(c) {} };
struct NicHist { RingBuffer<float> send, recv; explicit NicHist(size_t c = 60) : send(c), recv(c) {} };
struct GpuHist {
    RingBuffer<float> util, dedicated, shared, temp;
    std::map<std::wstring, RingBuffer<float>> engines;
    explicit GpuHist(size_t c = 60) : util(c), dedicated(c), shared(c), temp(c) {}
};
struct ProcHist {
    RingBuffer<float> cpu{120}, mem{120}, io{120}, gpu{120}, net{120};
    RingBuffer<float> cpuLong{90}, memLong{90}; // 10-sample averages (~15 min at 1 s)
    float cpuAcc = 0, memAcc = 0;
    int accN = 0;
    std::wstring name;
    uint32_t pid = 0;
    uint64_t lastSeq = 0;
};
struct AppUsage {
    std::wstring name, path, publisher;
    double cpuSeconds = 0, gpuSeconds = 0;
    double ioBytes = 0, netBytes = 0;
    uint64_t firstSeen = 0, lastSeen = 0;
    uint32_t launches = 0;
};

struct Histories {
    size_t cap = 60;
    RingBuffer<float> cpu{60}, cpuKernel{60}, memPct{60}, commitPct{60}, memUsed{60}, pagesPerSec{60}, thermal{60}, cpuMHz{60};
    std::vector<RingBuffer<float>> cores;
    std::map<int, DiskHist> disks;
    std::map<uint64_t, NicHist> nics;
    std::map<uint64_t, GpuHist> gpus;
    RingBuffer<float> netTotalSend{60}, netTotalRecv{60}, diskTotalRead{60}, diskTotalWrite{60}, gpuMax{60};
    void Resize(size_t c);
};

// Lazily loaded static hardware/system information.
struct StaticInfo {
    bool sysLoaded = false, sysLoading = false;
    SystemStatic sys;
    MemStatic mem;
    bool gpusLoaded = false, gpusLoading = false;
    std::vector<GpuStatic> gpus;
    bool disksLoaded = false, disksLoading = false;
    std::vector<DiskStatic> disks;
    std::map<int, StorageHealth> health;
    bool nicsLoaded = false, nicsLoading = false;
    std::vector<NicStatic> nics;
    uint64_t nicsTime = 0;
};

class App {
public:
    App();
    ~App();
    bool Create(int nCmdShow, bool startMinimized);
    int Run();
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    // ---- services for pages ----
    HWND Hwnd() const { return hwnd_; }
    Settings& Config() { return settings_; }
    void SaveSettings();
    const Theme& Th() const { return theme_; }
    Gfx& G() { return gfx_; }
    IconCache& Icons() { return icons_; }
    Sampler& Collector() { return *sampler_; }
    const SnapshotPtr& Snap() const { return snap_; }
    const SnapshotPtr& PrevSnap() const { return prevSnap_; }
    Histories& Hist() { return hist_; }
    std::unordered_map<uint64_t, ProcHist>& ProcHistory() { return procHist_; }
    const ProcHist* FindProcHist(const ProcSample& p) const;
    std::map<std::wstring, AppUsage>& AppHistory() { return appUsage_; }
    void ResetAppHistory();
    const std::vector<DiagAlert>& Alerts() const { return alerts_; }
    void RefreshDiagnosticsNow();
    StaticInfo& Static() { return static_; }
    void EnsureSystemInfo();
    void EnsureGpuInfo();
    void EnsureDiskInfo(bool withHealth);
    void EnsureNicInfo(bool force = false);
    struct EventCounts { int crashes24h = -1, kernelPower30d = -1, whea30d = -1, errors24h = -1; bool loading = false, loaded = false; } events;
    void EnsureEventCounts();

    void Navigate(PageId id);
    PageId Current() const { return current_; }
    Page* GetPage(PageId id) { return pages_[static_cast<int>(id)].get(); }
    void ShowStatus(const std::wstring& text, bool error = false);
    UINT ShowMenu(const std::vector<MenuItem>& items, float x = -1, float y = -1); // DIP coords; -1 = cursor
    bool Confirm(const std::wstring& title, const std::wstring& message, const std::wstring& okText, bool danger = false);
    void Info(const std::wstring& title, const std::wstring& message, bool error = false);
    void Invalidate();
    void OpenProcessWindow(uint32_t pid);
    void RequestElevatedRestart(const std::wstring& reason);
    bool Elevated() const { return elevated_; }
    float Dpi() const { return dpi_; }
    TempUnit TempUnits() const { return settings_.GetString("units.temperature", L"C") == L"F" ? TempUnit::Fahrenheit : TempUnit::Celsius; }
    RateUnit NetUnits() const { return settings_.GetString("units.network", L"bits") == L"bytes" ? RateUnit::Bytes : RateUnit::Bits; }
    void ApplyTheme();
    void ApplyRefreshRate();
    void SetTopmost(bool on);
    void ToggleMiniMonitor();
    bool MiniMonitorVisible() const;
    HistoryRecorder& Recorder() { return recorder_; }
    void UpdateDemand();
    bool Compact() const { return compact_; }
    void SetCompact(bool c);
    void RunNewTaskDialog();
    void ExportReport();
    void ExportHistory(bool json);
    bool PageVisibleInNav(PageId id) const;
    void SetPageHidden(PageId id, bool hidden);
    const std::vector<HistoryPoint>& SessionHistory() const { return sessionHistory_; }
    std::wstring ConfigDir() const { return configDir_; }
    void ShowFromTray();
    void ExitApp();

private:
    void OnSnapshot();
    void UpdateHistories(const Snapshot& s);
    void UpdateProcHistories(const Snapshot& s);
    void UpdateAppUsage(const Snapshot& s);
    void EvaluateDiagnostics();
    void Paint();
    void DrawNav(Ui& ui, Rect r);
    void DrawStatusBar(Ui& ui, Rect r);
    void DrawTopBar(Ui& ui, Rect r);
    void LayoutSearch(const Rect& r, bool visible);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
    void HandleMouse(UINT msg, WPARAM wp, LPARAM lp);
    bool HandleKey(UINT vk);
    void LoadSettings();
    void ApplyMica();
    void RestoreWindowPlacement(int& nCmdShow);
    void SaveWindowPlacement();
    void RunDeferred();

    HWND hwnd_ = nullptr, search_ = nullptr;
    HBRUSH searchBrush_ = nullptr;
    HFONT searchFont_ = nullptr;
    Gfx gfx_;
    Theme theme_;
    InputState input_;
    std::unique_ptr<Ui> ui_;
    Settings settings_;
    std::wstring configDir_, configPath_;
    std::unique_ptr<Sampler> sampler_;
    IconCache icons_;
    SnapshotPtr snap_, prevSnap_;
    Histories hist_;
    std::unordered_map<uint64_t, ProcHist> procHist_;
    std::map<std::wstring, AppUsage> appUsage_;
    std::vector<HistoryPoint> sessionHistory_;
    std::vector<DiagAlert> alerts_;
    uint64_t lastDiag_ = 0;
    StaticInfo static_;
    std::unique_ptr<Page> pages_[static_cast<int>(PageId::Count)];
    PageId current_ = PageId::Processes;
    std::wstring status_;
    bool statusError_ = false;
    uint64_t statusTime_ = 0;
    float dpi_ = 96.f;
    bool elevated_ = false, compact_ = false, navCollapsed_ = false, minimized_ = false, hiddenToTray_ = false, exiting_ = false;
    bool trackingMouse_ = false;
    WidgetId lastHot_ = 0;
    std::unique_ptr<Tray> tray_;
    std::unique_ptr<MiniMonitor> mini_;
    HistoryRecorder recorder_;
    std::vector<std::function<void()>> deferred_;
    float navScroll_ = 0;
    std::wstring searchText_;
    friend class Tray;
};

App& TheApp();

// ---- Page interface ----
class Page {
public:
    explicit Page(App& app) : app(app) {}
    virtual ~Page() = default;
    virtual const wchar_t* Title() const = 0;
    virtual wchar_t Glyph() const = 0;
    virtual uint32_t Demand() const { return 0; }
    virtual void OnShow() {}
    virtual void OnHide() {}
    virtual void OnSnapshot() {}
    virtual void Draw(Ui& ui, const Rect& area) = 0;
    virtual bool HasSearch() const { return false; }
    virtual const wchar_t* SearchHint() const { return L"Search"; }
    virtual void OnSearch(const std::wstring& text) {}
    virtual bool OnKey(UINT vk, bool ctrl, bool shift) { return false; }
    // Text that accessibility tools read for the page's current state.
    virtual std::wstring AccessibleSummary() const { return Title(); }
    virtual void SaveState() {}

protected:
    App& app;
};

std::unique_ptr<Page> MakeOverviewPage(App&);
std::unique_ptr<Page> MakeProcessesPage(App&);
std::unique_ptr<Page> MakePerformancePage(App&);
std::unique_ptr<Page> MakeAppHistoryPage(App&);
std::unique_ptr<Page> MakeStartupPage(App&);
std::unique_ptr<Page> MakeUsersPage(App&);
std::unique_ptr<Page> MakeDetailsPage(App&);
std::unique_ptr<Page> MakeServicesPage(App&);
std::unique_ptr<Page> MakeHardwarePage(App&);
std::unique_ptr<Page> MakeNetworkPage(App&);
std::unique_ptr<Page> MakeStoragePage(App&);
std::unique_ptr<Page> MakeDiagnosticsPage(App&);
std::unique_ptr<Page> MakeSettingsPage(App&);

// Shared process-action helpers (used by Processes, Details, Users and the process window).
namespace actions {
void EndTask(App& app, const ProcSample& p);
void EndTree(App& app, const ProcSample& p);
void SetPriority(App& app, const ProcSample& p, uint32_t cls);
void SetAffinityDialog(App& app, const ProcSample& p);
void ToggleEfficiency(App& app, const ProcSample& p);
void CreateDump(App& app, const ProcSample& p, bool full);
void SuspendResume(App& app, const ProcSample& p, bool suspend);
void OpenLocation(const ProcSample& p);
void Properties(App& app, const ProcSample& p);
// Builds the standard process context menu; returns the chosen command (already executed).
void ProcessContextMenu(App& app, const ProcSample& p, bool detailsView);
} // namespace actions

// Small native text-input overlay used by pages that need typed input.
class NativeEdit {
public:
    ~NativeEdit();
    void Place(App& app, const Rect& r, const wchar_t* cue);
    void Hide();
    std::wstring Text() const;
    void SetText(const std::wstring& s);
    bool Focused() const;
    HWND Handle() const { return hwnd_; }
private:
    HWND hwnd_ = nullptr;
    RECT last_{};
};

} // namespace nx
