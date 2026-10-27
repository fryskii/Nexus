// App history: per-application resource accounting collected while NEXUS runs (optionally
// persisted across sessions).
#include "data/History.h"
#include "ui/pages/Common.h"

#include <cstdio>

namespace nx {

namespace {

enum { HName, HPublisher, HCpu, HGpu, HIo, HNet, HLaunches, HFirst, HLast, HPath };

std::wstring LocalTime(uint64_t ft) {
    if (!ft) return L"";
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, l;
    FileTimeToLocalFileTime(&f, &l);
    return FormatFileTime(FileTimeToU64(l));
}

std::wstring Seconds(double s) {
    uint64_t t = static_cast<uint64_t>(s);
    wchar_t b[32];
    swprintf(b, 32, L"%llu:%02llu:%02llu", static_cast<unsigned long long>(t / 3600), static_cast<unsigned long long>((t / 60) % 60),
             static_cast<unsigned long long>(t % 60));
    return b;
}

class AppHistoryPage final : public Page {
public:
    explicit AppHistoryPage(App& a) : Page(a), table_(Id("apphist.table")) {
        table_.columns = {Col("name", L"Name", 260), Col("publisher", L"Publisher", 170), HeatCol("cpu", L"CPU time", 100), HeatCol("gpu", L"GPU time", 100),
                          HeatCol("io", L"Disk I/O", 100), HeatCol("net", L"Network", 100), Col("launches", L"Launches", 80, Align::Right),
                          Col("first", L"First seen", 150, Align::Left, false), Col("last", L"Last active", 150), Col("path", L"Path", 360, Align::Left, false)};
        table_.columns[HNet].tooltip = L"Requires administrator (kernel network ETW) while NEXUS runs.";
        table_.sortCol = HCpu;
        table_.sortDesc = true;
        table_.heatMap = false;
        table_.LoadLayout(app.Config().GetString("apphistory.layout"));
        since_ = NowFileTime();
        if (app.Config().GetBool("apphistory.persist", false)) LoadFile();
    }
    const wchar_t* Title() const override { return L"App history"; }
    wchar_t Glyph() const override { return 0xE81C; }
    bool HasSearch() const override { return true; }
    const wchar_t* SearchHint() const override { return L"Filter apps"; }
    void OnSearch(const std::wstring& s) override { grid_.SetFilter(s); }
    void OnShow() override { Fill(); }
    void OnSnapshot() override {
        if (app.Current() == PageId::AppHistory && ++tick_ % 2 == 0) Fill();
    }
    void SaveState() override {
        app.Config().SetString("apphistory.layout", table_.SaveLayout());
        if (app.Config().GetBool("apphistory.persist", false)) SaveFile();
    }
    std::wstring AccessibleSummary() const override { return L"App history, " + std::to_wstring(app.AppHistory().size()) + L" applications tracked."; }
    bool OnKey(UINT vk, bool, bool) override { return table_.OnKey(vk, grid_); }

    void Draw(Ui& ui, const Rect& area) override {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        Rect r = area;
        Rect bar = r.TakeTop(44);
        if (ui.Button(Id("ah.reset"), {bar.R() - 190, bar.y + 4, 190, 32}, L"Delete usage history", 0xE74D)) {
            ui.Defer([this] {
                if (app.Confirm(L"Delete app usage history?", L"The resource totals collected by NEXUS will be reset.", L"Delete")) {
                    app.ResetAppHistory();
                    since_ = NowFileTime();
                    if (app.Config().GetBool("apphistory.persist", false)) SaveFile();
                    Fill();
                }
            });
        }
        bool persist = app.Config().GetBool("apphistory.persist", false);
        if (ui.Button(Id("ah.persist"), {bar.R() - 400, bar.y + 4, 202, 32}, persist ? L"Keep across restarts: on" : L"Keep across restarts: off", 0xE74E)) {
            app.Config().SetBool("apphistory.persist", !persist);
            if (!persist) SaveFile();
        }
        g.Text(L"Resource usage since " + LocalTime(since_), {bar.x, bar.y, bar.w - 420, 40}, Font::Small, t.textTertiary);
        Rect note = r.TakeTop(40);
        InfoText(g, t, {note.x, note.y + 2, note.w, 36},
                 L"Collected by NEXUS while it runs. Task Manager's App history reads the System Resource Usage Monitor database, which Windows keeps locked for its own "
                 L"service, so NEXUS measures usage itself instead of estimating it.");
        if (grid_.Dirty() || table_.sortChanged) { grid_.Refresh(table_); table_.sortChanged = false; }
        table_.Draw(ui, r, grid_);
        if (table_.contextMenu) {
            table_.contextMenu = false;
            int row = table_.SelectedRow(grid_);
            const GridRow* gr = row >= 0 ? grid_.RowAt(row) : nullptr;
            if (gr) {
                std::wstring path = gr->cells[HPath], name = gr->cells[HName];
                ui.Defer([this, path, name] {
                    UINT c = app.ShowMenu({{1, L"Open file location"}, {2, L"Search online"}, {3, L"Copy path"}});
                    if (c == 1) OpenFileLocation(path);
                    if (c == 2) SearchOnline(name);
                    if (c == 3) CopyTextToClipboard(app.Hwnd(), path);
                });
            }
        }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); }); }
    }

private:
    void Fill() {
        grid_.rows.clear();
        grid_.icons = &app.Icons();
        for (auto& [k, u] : app.AppHistory()) {
            GridRow r;
            r.key = std::hash<std::wstring>{}(k);
            r.cells = {u.name, u.publisher, Seconds(u.cpuSeconds), Seconds(u.gpuSeconds), FormatBytes(static_cast<uint64_t>(u.ioBytes)),
                       u.netBytes > 0 ? FormatBytes(static_cast<uint64_t>(u.netBytes)) : (app.Snap() && app.Snap()->netPerProcess ? L"0 B" : L"—"),
                       std::to_wstring(u.launches), LocalTime(u.firstSeen), LocalTime(u.lastSeen), u.path};
            r.nums = {NoNum(), NoNum(), u.cpuSeconds, u.gpuSeconds, u.ioBytes, u.netBytes, static_cast<double>(u.launches), static_cast<double>(u.firstSeen),
                      static_cast<double>(u.lastSeen)};
            r.iconPath = u.path;
            grid_.rows.push_back(std::move(r));
        }
        grid_.MarkDirty();
    }

    std::wstring FilePath() const { return app.ConfigDir() + L"\\apphistory.tsv"; }

    void SaveFile() {
        std::string s = "# NEXUS app history v1\tsince=" + std::to_string(since_) + "\n";
        char b[128];
        for (auto& [k, u] : app.AppHistory()) {
            std::snprintf(b, sizeof b, "%.3f\t%.3f\t%.0f\t%.0f\t%llu\t%llu\t%u\t", u.cpuSeconds, u.gpuSeconds, u.ioBytes, u.netBytes,
                          static_cast<unsigned long long>(u.firstSeen), static_cast<unsigned long long>(u.lastSeen), u.launches);
            s += b + ToUtf8(u.path) + "\t" + ToUtf8(u.name) + "\t" + ToUtf8(u.publisher) + "\n";
        }
        std::wstring err;
        WriteFileAtomic(FilePath(), s, err);
    }

    void LoadFile() {
        HANDLE h = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        std::string data(static_cast<size_t>(std::min<LONGLONG>(sz.QuadPart, 16 << 20)), '\0');
        DWORD rd = 0;
        ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &rd, nullptr);
        CloseHandle(h);
        data.resize(rd);
        size_t pos = 0;
        while (pos < data.size()) {
            size_t nl = data.find('\n', pos);
            if (nl == std::string::npos) nl = data.size();
            std::string line = data.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.empty()) continue;
            if (line[0] == '#') {
                size_t p = line.find("since=");
                if (p != std::string::npos) since_ = std::strtoull(line.c_str() + p + 6, nullptr, 10);
                continue;
            }
            std::vector<std::string> f;
            size_t s = 0;
            while (true) {
                size_t tab = line.find('\t', s);
                f.push_back(line.substr(s, tab == std::string::npos ? std::string::npos : tab - s));
                if (tab == std::string::npos) break;
                s = tab + 1;
            }
            if (f.size() < 10) continue;
            AppUsage u;
            u.cpuSeconds = std::atof(f[0].c_str());
            u.gpuSeconds = std::atof(f[1].c_str());
            u.ioBytes = std::atof(f[2].c_str());
            u.netBytes = std::atof(f[3].c_str());
            u.firstSeen = std::strtoull(f[4].c_str(), nullptr, 10);
            u.lastSeen = std::strtoull(f[5].c_str(), nullptr, 10);
            u.launches = static_cast<uint32_t>(std::strtoul(f[6].c_str(), nullptr, 10));
            u.path = FromUtf8(f[7]);
            u.name = FromUtf8(f[8]);
            u.publisher = FromUtf8(f[9]);
            if (!u.path.empty()) app.AppHistory()[ToLower(u.path)] = u;
        }
    }

    Table table_;
    SimpleGrid grid_;
    uint64_t since_ = 0;
    uint64_t tick_ = 0;
};

} // namespace

std::unique_ptr<Page> MakeAppHistoryPage(App& app) { return std::make_unique<AppHistoryPage>(app); }

} // namespace nx
