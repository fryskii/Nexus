// Startup apps: registry Run keys, Startup folders and packaged-app startup tasks, with
// enable/disable through the same StartupApproved mechanism Task Manager uses.
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

#include <shellapi.h>
#include <shlobj.h>

namespace nx {

namespace {

enum { UName, UPublisher, UStatus, UImpact, URunning, UCpuNow, UMemNow, ULocation, UCommand, UDisabled };

class StartupPage final : public Page {
public:
    explicit StartupPage(App& a) : Page(a), table_(Id("startup.table")) {
        TableColumn impact = Col("impact", L"Startup impact", 120);
        impact.tooltip = L"Task Manager derives startup impact from boot performance traces that Windows does not expose through a public API. "
                         L"NEXUS shows 'Not measured' instead of guessing; see 'Running now' and current usage.";
        table_.columns = {Col("name", L"Name", 240), Col("publisher", L"Publisher", 190), Col("status", L"Status", 90), impact,
                          Col("running", L"Running now", 100), Col("cpu", L"CPU now", 80, Align::Right), Col("mem", L"Memory now", 100, Align::Right),
                          Col("location", L"Location", 200, Align::Left, false), Col("cmd", L"Command line", 420), Col("disabled", L"Disabled time", 150, Align::Left, false)};
        table_.sortCol = UName;
        table_.sortDesc = false;
        table_.heatMap = false;
        table_.LoadLayout(app.Config().GetString("startup.layout"));
    }
    const wchar_t* Title() const override { return L"Startup apps"; }
    wchar_t Glyph() const override { return 0xE7E8; }
    bool HasSearch() const override { return true; }
    const wchar_t* SearchHint() const override { return L"Filter startup apps"; }
    void OnSearch(const std::wstring& s) override { grid_.SetFilter(s); }
    void OnShow() override { Load(); }
    void OnSnapshot() override { if (app.Current() == PageId::Startup) UpdateRunning(); }
    void SaveState() override { app.Config().SetString("startup.layout", table_.SaveLayout()); }
    std::wstring AccessibleSummary() const override {
        const StartupItem* s = Selected();
        return L"Startup apps, " + std::to_wstring(items_.size()) + L" items." + (s ? L" Selected " + s->name + (s->enabled ? L", enabled" : L", disabled") : L"");
    }
    bool OnKey(UINT vk, bool, bool shift) override {
        if (vk == VK_APPS || (vk == VK_F10 && shift)) { Menu(); return true; }
        if (vk == VK_SPACE) { Toggle(); return true; }
        return table_.OnKey(vk, grid_);
    }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        const StartupItem* s = Selected();
        bool can = s && s->editable;
        if (ui.Button(Id("st.enable"), {bar.R() - 230, bar.y + 4, 110, 32}, L"Enable", 0xE73E, ButtonStyle::Standard, can && !s->enabled)) Toggle();
        if (ui.Button(Id("st.disable"), {bar.R() - 112, bar.y + 4, 112, 32}, L"Disable", 0xE711, ButtonStyle::Standard, can && s->enabled)) Toggle();
        size_t enabled = std::count_if(items_.begin(), items_.end(), [](const StartupItem& i) { return i.enabled; });
        std::wstring info = loading_ ? L"Loading…" : std::to_wstring(items_.size()) + L" startup items, " + std::to_wstring(enabled) + L" enabled";
        if (s && !s->editable) info += L"   ·   Packaged app tasks are managed in Settings › Apps › Startup";
        ui.g.Text(info, {bar.x, bar.y, bar.w - 250, 40}, Font::Small, ui.t.textTertiary);
        if (grid_.Dirty() || table_.sortChanged) { grid_.Refresh(table_); table_.sortChanged = false; }
        table_.Draw(ui, r, grid_);
        if (table_.contextMenu || table_.activated) { table_.contextMenu = table_.activated = false; ui.Defer([this] { Menu(); }); }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); }); }
    }

private:
    const StartupItem* Selected() const {
        for (size_t i = 0; i < items_.size(); ++i)
            if (KeyOf(items_[i], i) == table_.selectedKey) return &items_[i];
        return nullptr;
    }
    static uint64_t KeyOf(const StartupItem& it, size_t) { return std::hash<std::wstring>{}(it.location + L"|" + it.name); }

    void Load() {
        if (loading_) return;
        loading_ = true;
        auto out = std::make_shared<std::vector<StartupItem>>();
        worker::Run([out] { *out = QueryStartupItems(); }, [this, out] {
            items_ = std::move(*out);
            loading_ = false;
            UpdateRunning();
            app.Invalidate();
        });
    }

    void UpdateRunning() {
        auto snap = app.Snap();
        grid_.rows.clear();
        grid_.icons = &app.Icons();
        for (size_t i = 0; i < items_.size(); ++i) {
            auto& it = items_[i];
            float cpu = 0;
            uint64_t mem = 0;
            int count = 0;
            if (snap && !it.target.empty()) {
                for (auto& p : snap->procs) {
                    if (p.meta && !p.meta->path.empty() && _wcsicmp(p.meta->path.c_str(), it.target.c_str()) == 0) {
                        cpu += p.cpu;
                        mem += p.privateWorkingSet;
                        ++count;
                    }
                }
            }
            it.running = count > 0;
            GridRow r;
            r.key = KeyOf(it, i);
            std::wstring disabled;
            if (it.disabledTime) {
                FILETIME ft{static_cast<DWORD>(it.disabledTime), static_cast<DWORD>(it.disabledTime >> 32)}, lt;
                FileTimeToLocalFileTime(&ft, &lt);
                disabled = FormatFileTime(FileTimeToU64(lt));
            }
            r.cells = {it.description.empty() || it.sourceKind == L"Packaged app" ? it.name : it.description, it.publisher, it.enabled ? L"Enabled" : L"Disabled",
                       L"Not measured", count ? (count > 1 ? L"Yes (" + std::to_wstring(count) + L")" : L"Yes") : L"No",
                       count ? FormatPercent(cpu, 1) : L"", count ? FormatMB(mem) : L"", it.location, it.command, disabled};
            r.nums = {NoNum(), NoNum(), NoNum(), NoNum(), static_cast<double>(count), cpu, static_cast<double>(mem)};
            r.iconPath = it.target;
            r.highlight = it.enabled ? 0 : 3;
            if (it.enabled) { r.color = app.Th().good; r.colorCol = UStatus; }
            grid_.rows.push_back(std::move(r));
        }
        grid_.MarkDirty();
    }

    void Toggle() {
        const StartupItem* s = Selected();
        if (!s || !s->editable) return;
        bool enable = !s->enabled;
        std::wstring name = s->name;
        DWORD e;
        if (s->approvedRoot == HKEY_LOCAL_MACHINE && !app.Elevated())
            e = RunSelfElevatedAndWait(app.Hwnd(), L"--startup-set " + QuoteArg(s->approvedKey.substr(s->approvedKey.find_last_of(L'\\') + 1)) + L" " +
                                                       QuoteArg(s->approvedValue) + L" " + (enable ? L"1" : L"0"));
        else
            e = SetStartupItemEnabled(s->approvedRoot, s->approvedKey, s->approvedValue, enable);
        if (e == ERROR_CANCELLED) return;
        if (e) app.ShowStatus(L"Could not change " + name + L": " + LastErrorMessage(e), true);
        else app.ShowStatus(name + (enable ? L" will start with Windows" : L" will no longer start with Windows"));
        Load();
    }

    void Menu() {
        const StartupItem* s = Selected();
        if (!s) return;
        StartupItem it = *s;
        UINT c = app.ShowMenu({{1, it.enabled ? L"Disable" : L"Enable", false, it.editable}, MenuItem::Sep(), {2, L"Open file location", false, !it.target.empty()},
                               {3, L"Search online"}, {4, L"Properties", false, !it.target.empty()}, {5, L"Copy command line"}, MenuItem::Sep(),
                               {6, L"Open Startup folder"}, {7, L"Refresh"}});
        switch (c) {
        case 1: Toggle(); break;
        case 2: OpenFileLocation(it.target); break;
        case 3: SearchOnline(it.name + L" " + it.publisher); break;
        case 4: SHObjectProperties(app.Hwnd(), SHOP_FILEPATH, it.target.c_str(), nullptr); break;
        case 5: CopyTextToClipboard(app.Hwnd(), it.command); break;
        case 6: ShellExecuteW(app.Hwnd(), L"open", L"shell:startup", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 7: Load(); break;
        }
    }

    Table table_;
    SimpleGrid grid_;
    std::vector<StartupItem> items_;
    bool loading_ = false;
};

} // namespace

std::unique_ptr<Page> MakeStartupPage(App& app) { return std::make_unique<StartupPage>(app); }

} // namespace nx
