// Services page: all Win32 services with status, PID, group, start type and control actions.
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

#include <shellapi.h>

#include <unordered_map>

namespace nx {

namespace {

enum { SName, SPid, SDisplay, SStatus, SGroup, SStart, SAccount, SPath, SDesc };

class ServicesPage final : public Page {
public:
    explicit ServicesPage(App& a) : Page(a), table_(Id("svc.table")) {
        table_.columns = {Col("name", L"Name", 200), Col("pid", L"PID", 70, Align::Right), Col("display", L"Description", 300),
                          Col("status", L"Status", 90), Col("group", L"Group", 140), Col("start", L"Startup type", 190),
                          Col("account", L"Log on as", 160, Align::Left, false), Col("path", L"Path to executable", 380, Align::Left, false),
                          Col("desc", L"Full description", 400, Align::Left, false)};
        table_.sortCol = SName;
        table_.sortDesc = false;
        table_.showIcons = false;
        table_.heatMap = false;
        table_.LoadLayout(app.Config().GetString("services.layout"));
    }
    const wchar_t* Title() const override { return L"Services"; }
    wchar_t Glyph() const override { return 0xEA86; }
    bool HasSearch() const override { return true; }
    const wchar_t* SearchHint() const override { return L"Filter services"; }
    void OnSearch(const std::wstring& s) override { grid_.SetFilter(s); }
    void OnShow() override { Load(); }
    void OnSnapshot() override {
        if (app.Current() == PageId::Services && GetTickCount64() - lastLoad_ > 5000) Load();
    }
    void SaveState() override { app.Config().SetString("services.layout", table_.SaveLayout()); }
    std::wstring AccessibleSummary() const override {
        const ServiceInfo* s = Selected();
        return L"Services, " + std::to_wstring(services_.size()) + L" services." + (s ? L" Selected " + s->displayName + L", " + ServiceStateName(s->state) : L"");
    }
    bool OnKey(UINT vk, bool, bool shift) override {
        if (vk == VK_APPS || (vk == VK_F10 && shift)) { Menu(); return true; }
        return table_.OnKey(vk, grid_);
    }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        const ServiceInfo* s = Selected();
        bool running = s && s->state == SERVICE_RUNNING;
        bool stopped = s && s->state == SERVICE_STOPPED;
        bool canPause = s && (s->controlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE);
        float x = bar.x;
        auto btn = [&](const char* id, const wchar_t* label, wchar_t glyph, float w, bool en) {
            bool c = ui.Button(Id(id), {x, bar.y + 4, w, 32}, label, glyph, ButtonStyle::Standard, en && !busy_);
            x += w + 8;
            return c;
        };
        if (btn("svc.start", L"Start", 0xE768, 90, stopped || (s && s->state == SERVICE_PAUSED))) Act(s->state == SERVICE_PAUSED ? ServiceAction::Continue : ServiceAction::Start);
        if (btn("svc.stop", L"Stop", 0xE71A, 90, running && (s->controlsAccepted & SERVICE_ACCEPT_STOP))) Act(ServiceAction::Stop);
        if (btn("svc.restart", L"Restart", 0xE72C, 100, running && (s->controlsAccepted & SERVICE_ACCEPT_STOP))) Act(ServiceAction::Restart);
        if (btn("svc.pause", L"Pause", 0xE769, 90, running && canPause)) Act(ServiceAction::Pause);
        if (ui.Button(Id("svc.open"), {bar.R() - 150, bar.y + 4, 150, 32}, L"Open Services", 0xE8A7)) ShellExecuteW(app.Hwnd(), L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
        size_t runningCount = std::count_if(services_.begin(), services_.end(), [](const ServiceInfo& s) { return s.state == SERVICE_RUNNING; });
        std::wstring info = busy_ ? busyText_ : std::to_wstring(services_.size()) + L" services, " + std::to_wstring(runningCount) + L" running";
        ui.g.Text(info, {x + 8, bar.y, bar.R() - 160 - x - 16, 40}, Font::Small, ui.t.textTertiary);
        table_.compact = true;
        if (grid_.Dirty() || table_.sortChanged) { grid_.Refresh(table_); table_.sortChanged = false; }
        table_.Draw(ui, r, grid_);
        if (table_.contextMenu) { table_.contextMenu = false; ui.Defer([this] { Menu(); }); }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); }); }
        if (table_.activated) { table_.activated = false; ui.Defer([this] { Menu(); }); }
    }

private:
    const ServiceInfo* Selected() const {
        for (auto& s : services_)
            if (std::hash<std::wstring>{}(s.name) == table_.selectedKey) return &s;
        return nullptr;
    }

    void Load() {
        if (loading_) return;
        loading_ = true;
        lastLoad_ = GetTickCount64();
        bool needConfig = configs_.empty();
        auto out = std::make_shared<std::vector<ServiceInfo>>();
        worker::Run([out, needConfig] { *out = QueryServices(needConfig); }, [this, out, needConfig] {
            loading_ = false;
            if (needConfig) for (auto& s : *out) configs_[s.name] = s;
            services_.clear();
            for (auto& s : *out) {
                ServiceInfo m = s;
                auto it = configs_.find(s.name);
                if (it != configs_.end() && !s.configLoaded) {
                    m.description = it->second.description; m.group = it->second.group; m.binaryPath = it->second.binaryPath; m.account = it->second.account;
                    m.startType = it->second.startType; m.delayedAuto = it->second.delayedAuto; m.triggerStart = it->second.triggerStart; m.configLoaded = it->second.configLoaded;
                }
                services_.push_back(std::move(m));
            }
            FillGrid();
            int64_t focus = app.Config().GetInt("services.focusPid", -1);
            if (focus >= 0) {
                app.Config().Remove("services.focusPid");
                grid_.SetFilter(L"");
                grid_.Refresh(table_);
                for (size_t i = 0; i < grid_.RowCount(); ++i) {
                    const GridRow* gr = grid_.RowAt(i);
                    if (gr && gr->cells[SPid] == std::to_wstring(focus)) { table_.SelectRow(i, grid_); break; }
                }
            }
            app.Invalidate();
        });
    }

    void FillGrid() {
        grid_.rows.clear();
        for (auto& s : services_) {
            GridRow r;
            r.key = std::hash<std::wstring>{}(s.name);
            r.cells = {s.name, s.pid ? std::to_wstring(s.pid) : L"", s.displayName, ServiceStateName(s.state), s.group, ServiceStartTypeName(s),
                       s.account, s.binaryPath, s.description};
            r.nums = {NoNum(), s.pid ? static_cast<double>(s.pid) : -1.0};
            r.highlight = s.startType == SERVICE_DISABLED ? 3 : 0;
            if (s.state == SERVICE_RUNNING) { r.color = app.Th().good; r.colorCol = SStatus; }
            grid_.rows.push_back(std::move(r));
        }
        grid_.MarkDirty();
    }

    void Act(ServiceAction a) {
        const ServiceInfo* s = Selected();
        if (!s) return;
        std::wstring name = s->name, display = s->displayName;
        static const wchar_t* verbs[] = {L"start", L"stop", L"restart", L"pause", L"continue"};
        const wchar_t* verb = verbs[static_cast<int>(a)];
        // Without elevation, disruptive actions go straight to the elevated helper, which asks for
        // confirmation in its own window (so the user is asked once, by the process that acts).
        bool disruptive = a == ServiceAction::Stop || a == ServiceAction::Restart || a == ServiceAction::Pause;
        bool viaHelper = disruptive && !app.Elevated();
        if (!viaHelper && (a == ServiceAction::Stop || a == ServiceAction::Restart)) {
            if (!app.Confirm(std::wstring(a == ServiceAction::Stop ? L"Stop " : L"Restart ") + display + L"?",
                             L"Programs or Windows features that depend on this service may stop working until it is started again.",
                             a == ServiceAction::Stop ? L"Stop service" : L"Restart service", true))
                return;
        }
        busy_ = true;
        busyText_ = std::wstring(L"Sending ") + verb + L" to " + display + L"…";
        auto result = std::make_shared<std::pair<DWORD, std::wstring>>();
        HWND hwnd = app.Hwnd();
        bool elevated = app.Elevated();
        worker::Run([result, name, a, viaHelper] { result->first = viaHelper ? ERROR_ACCESS_DENIED : ControlServiceAction(name, a, result->second); },
                    [this, result, name, display, verb, hwnd, elevated] {
            if (result->first == ERROR_ACCESS_DENIED && !elevated) {
                // Use a one-off elevated helper instead of elevating the whole UI.
                auto r2 = std::make_shared<DWORD>(0);
                std::wstring args = std::wstring(L"--svc ") + verb + L" " + QuoteArg(name);
                worker::Run([r2, hwnd, args] { *r2 = RunSelfElevatedAndWait(hwnd, args); }, [this, r2, display, verb] {
                    busy_ = false;
                    if (*r2 == 0) app.ShowStatus(L"Service " + display + L": " + verb + L" completed");
                    else if (*r2 != ERROR_CANCELLED) app.ShowStatus(L"Could not " + std::wstring(verb) + L" " + display + L": " + LastErrorMessage(*r2), true);
                    else app.ShowStatus(L"Cancelled");
                    Load();
                });
                return;
            }
            busy_ = false;
            if (result->first) app.ShowStatus(L"Could not " + std::wstring(verb) + L" " + display + L": " + (result->second.empty() ? LastErrorMessage(result->first) : result->second), true);
            else app.ShowStatus(L"Service " + display + L": " + verb + L" completed");
            Load();
        });
    }

    void SetStartType(uint32_t type, bool delayed) {
        const ServiceInfo* s = Selected();
        if (!s) return;
        std::wstring name = s->name;
        if (type == SERVICE_DISABLED && app.Elevated() &&
            !app.Confirm(L"Disable " + s->displayName + L"?", L"It will no longer start, even when Windows or other programs need it.", L"Disable service", true))
            return;
        DWORD e = SetServiceStartType(name, type, delayed);
        if (e == ERROR_ACCESS_DENIED && !app.Elevated())
            e = RunSelfElevatedAndWait(app.Hwnd(), L"--svc-starttype " + QuoteArg(name) + L" " + std::to_wstring(type) + (delayed ? L" 1" : L" 0"));
        if (e && e != ERROR_CANCELLED) app.ShowStatus(L"Could not change the startup type: " + LastErrorMessage(e), true);
        else if (!e) app.ShowStatus(L"Startup type changed for " + s->displayName);
        configs_.clear();
        Load();
    }

    void Menu() {
        const ServiceInfo* s = Selected();
        if (!s) return;
        bool running = s->state == SERVICE_RUNNING, stopped = s->state == SERVICE_STOPPED, paused = s->state == SERVICE_PAUSED;
        bool canStop = (s->controlsAccepted & SERVICE_ACCEPT_STOP) != 0;
        bool canPause = (s->controlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE) != 0;
        MenuItem st{0, L"Startup type"};
        st.sub = {{30, L"Automatic", s->startType == SERVICE_AUTO_START && !s->delayedAuto}, {31, L"Automatic (Delayed Start)", s->startType == SERVICE_AUTO_START && s->delayedAuto},
                  {32, L"Manual", s->startType == SERVICE_DEMAND_START}, {33, L"Disabled", s->startType == SERVICE_DISABLED}};
        uint32_t pid = s->pid;
        std::wstring display = s->displayName, name = s->name, path = s->binaryPath;
        UINT c = app.ShowMenu({{1, L"Start", false, stopped}, {2, L"Stop", false, running && canStop}, {3, L"Restart", false, running && canStop},
                               {4, L"Pause", false, running && canPause}, {5, L"Resume", false, paused}, MenuItem::Sep(), st, MenuItem::Sep(),
                               {6, L"Open Services"}, {7, L"Search online"}, {8, L"Go to details", false, pid != 0},
                               {9, L"Copy name"}, {10, L"Open file location", false, !path.empty()}});
        switch (c) {
        case 1: Act(ServiceAction::Start); break;
        case 2: Act(ServiceAction::Stop); break;
        case 3: Act(ServiceAction::Restart); break;
        case 4: Act(ServiceAction::Pause); break;
        case 5: Act(ServiceAction::Continue); break;
        case 6: ShellExecuteW(app.Hwnd(), L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 7: SearchOnline(display + L" " + name + L" service"); break;
        case 8: app.Config().SetInt("details.focusPid", pid); app.Navigate(PageId::Details); break;
        case 9: CopyTextToClipboard(app.Hwnd(), name); break;
        case 10: {
            std::wstring exe = path;
            if (!exe.empty() && exe[0] == L'"') exe = exe.substr(1, exe.find(L'"', 1) - 1);
            else exe = exe.substr(0, exe.find(L" "));
            OpenFileLocation(ExpandEnv(exe));
            break;
        }
        case 30: SetStartType(SERVICE_AUTO_START, false); break;
        case 31: SetStartType(SERVICE_AUTO_START, true); break;
        case 32: SetStartType(SERVICE_DEMAND_START, false); break;
        case 33:
            if (app.Confirm(L"Disable " + display + L"?", L"Disabling services can break Windows features or applications. Only disable services you understand.",
                            L"Disable", true))
                SetStartType(SERVICE_DISABLED, false);
            break;
        }
    }

    Table table_;
    SimpleGrid grid_;
    std::vector<ServiceInfo> services_;
    std::unordered_map<std::wstring, ServiceInfo> configs_;
    uint64_t lastLoad_ = 0;
    bool loading_ = false, busy_ = false;
    std::wstring busyText_;
};

} // namespace

std::unique_ptr<Page> MakeServicesPage(App& app) { return std::make_unique<ServicesPage>(app); }

} // namespace nx
