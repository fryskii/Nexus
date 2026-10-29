// System diagnostics: evidence-based health findings, investigations, system/firmware summary,
// driver inventory, event log errors and application crash history.
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

#include <map>

namespace nx {

namespace {

enum Tab { DHealth, DInvestigate, DSystem, DDrivers, DEvents, DCrashes };

std::wstring LocalTime(uint64_t ft) {
    if (!ft) return L"";
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, l;
    FileTimeToLocalFileTime(&f, &l);
    return FormatFileTime(FileTimeToU64(l));
}

class DiagnosticsPage final : public Page {
public:
    explicit DiagnosticsPage(App& a) : Page(a), drivers_(Id("diag.drivers")), events_(Id("diag.events")), crashes_(Id("diag.crashes")) {
        drivers_.columns = {Col("name", L"Name", 160), Col("display", L"Display name", 260), Col("state", L"State", 90), Col("start", L"Start", 90),
                            Col("version", L"Version", 130), Col("company", L"Company", 180), Col("path", L"Path", 360)};
        events_.columns = {Col("time", L"Time", 150), Col("level", L"Level", 90), Col("log", L"Log", 100), Col("source", L"Source", 220), Col("id", L"Event ID", 80, Align::Right),
                           Col("msg", L"Message", 600)};
        crashes_.columns = {Col("time", L"Time", 150), Col("kind", L"Type", 110), Col("app", L"Application", 200), Col("ver", L"Version", 110), Col("module", L"Faulting module", 200),
                            Col("code", L"Exception", 110), Col("offset", L"Offset", 130), Col("path", L"Path", 360)};
        for (Table* t : {&drivers_, &events_, &crashes_}) { t->heatMap = false; t->compact = true; t->showIcons = false; }
        drivers_.sortCol = 2;
        drivers_.sortDesc = true;
        events_.sortCol = 0;
        events_.sortDesc = true;
        crashes_.sortCol = 0;
        crashes_.sortDesc = true;
    }
    const wchar_t* Title() const override { return L"System diagnostics"; }
    wchar_t Glyph() const override { return 0xE9D9; }
    uint32_t Demand() const override { return demand::Disk | demand::Thermal | demand::GpuSensors | demand::GpuSystem; }
    bool HasSearch() const override { return tab_ == DDrivers || tab_ == DEvents || tab_ == DCrashes; }
    void OnSearch(const std::wstring& s) override { drvGrid_.SetFilter(s); evGrid_.SetFilter(s); crGrid_.SetFilter(s); }
    void OnShow() override {
        app.EnsureSystemInfo();
        app.EnsureGpuInfo();
        app.EnsureDiskInfo(true);
        app.EnsureEventCounts();
        app.RefreshDiagnosticsNow();
    }
    std::wstring AccessibleSummary() const override {
        return L"System diagnostics, " + std::to_wstring(app.Alerts().size()) + L" findings.";
    }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        int t = ui.Tabs(Id("diag.tabs"), {bar.x, bar.y + 4, bar.w - 200, 32}, {L"Health", L"Investigate", L"System", L"Drivers", L"Events", L"Crashes"}, tab_);
        if (t >= 0) { tab_ = t; scroll_ = 0; }
        if (ui.Button(Id("diag.report"), {bar.R() - 180, bar.y + 4, 180, 32}, L"Export report", 0xE74E)) ui.Defer([this] { app.ExportReport(); });
        r.y += 6;
        r.h -= 6;
        switch (tab_) {
        case DHealth: DrawHealth(ui, r); break;
        case DInvestigate: DrawInvestigate(ui, r); break;
        case DSystem: DrawSystem(ui, r); break;
        case DDrivers: LoadDrivers(); DrawGrid(ui, r, drivers_, drvGrid_, loadingDrivers_); break;
        case DEvents: LoadEvents(); DrawEvents(ui, r); break;
        case DCrashes: LoadCrashes(); DrawGrid(ui, r, crashes_, crGrid_, loadingCrashes_); break;
        }
    }

private:
    void DrawHealth(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        const auto& alerts = app.Alerts();
        auto snap = app.Snap();
        double minutes = app.Hist().cpu.Size() * (snap ? snap->interval : 1.0) / 60.0;
        Rect head = r.TakeTop(56);
        size_t crit = 0, warn = 0;
        for (auto& a : alerts) { crit += a.severity == DiagAlert::Critical; warn += a.severity == DiagAlert::Warning; }
        Color sc = crit ? t.critical : warn ? t.warning : t.good;
        g.FillEllipse(head.x + 14, head.y + 20, 9, 9, sc);
        std::wstring title = crit ? L"Problems found" : warn ? L"Some things need attention" : L"No problems detected";
        g.Text(title, {head.x + 34, head.y + 4, head.w - 300, 30}, Font::Subtitle, t.text);
        wchar_t b[200];
        swprintf(b, 200, L"%zu critical, %zu warnings, %zu informational · based on %.0f minutes of monitoring", crit, warn, alerts.size() - crit - warn, minutes);
        g.Text(b, {head.x + 34, head.y + 30, head.w - 300, 20}, Font::Small, t.textSecondary);
        if (ui.Button(Id("diag.reeval"), {head.R() - 150, head.y + 8, 150, 32}, L"Re-evaluate", 0xE72C)) app.RefreshDiagnosticsNow();
        float off = scroll_;
        g.PushClip(r);
        float y = r.y - scroll_;
        if (alerts.empty()) {
            g.Paragraph(L"NEXUS continuously checks for sustained CPU saturation, processes with steadily growing memory, memory pressure and paging, abnormal disk "
                        L"latency, network adapter errors, low disk space, drive self-reported health problems, high temperatures, and recent crash, unexpected "
                        L"shutdown and hardware error (WHEA) events. Rules that look at trends need several minutes of data. Findings will appear here with the "
                        L"measurements that triggered them.",
                        {r.x, y, std::min(r.w, 820.f), 120}, Font::Body, t.textSecondary);
            y += 120;
        }
        for (size_t i = 0; i < alerts.size(); ++i) {
            const DiagAlert& a = alerts[i];
            float w = r.w - 12;
            float evH = a.evidence.size() * 20.f;
            float reasonH = g.MeasureParagraph(L"Why: " + a.reasoning, w - 60, Font::Small);
            float sugH = a.suggestion.empty() ? 0 : g.MeasureParagraph(L"What to do: " + a.suggestion, w - 60, Font::Small);
            float h = 50 + evH + reasonH + sugH + 24;
            Rect c{r.x, y, w, h};
            if (c.B() > r.y && c.y < r.B()) {
                ui.Card(c);
                Color ac = a.severity == DiagAlert::Critical ? t.critical : a.severity == DiagAlert::Warning ? t.warning : t.info;
                g.FillRound({c.x, c.y, 4, c.h}, 2.f, ac);
                g.Glyph(a.severity == DiagAlert::Info ? 0xE946 : 0xE7BA, {c.x + 14, c.y + 12, 24, 24}, Font::Icon, ac);
                g.Text(a.title, {c.x + 46, c.y + 10, c.w - 260, 26}, Font::BodySemi, t.text);
                ui.Badge({c.R() - 112, c.y + 14, 96, 20}, a.category, ac.Alpha(0.18f), ac);
                float yy = c.y + 42;
                for (auto& e : a.evidence) {
                    g.Text(L"•  " + e, {c.x + 46, yy, c.w - 60, 20}, Font::Small, t.text);
                    yy += 20;
                }
                yy += 4;
                yy += g.Paragraph(L"Why: " + a.reasoning, {c.x + 46, yy, c.w - 60, reasonH + 4}, Font::Small, t.textSecondary) + 4;
                if (!a.suggestion.empty()) g.Paragraph(L"What to do: " + a.suggestion, {c.x + 46, yy, c.w - 60, sugH + 4}, Font::Small, t.textSecondary);
                if (a.pid && snap && snap->FindProc(a.pid)) {
                    uint32_t pid = a.pid;
                    if (ui.Button(IdMix(Id("diag.inspect"), i), {c.R() - 230, c.y + 10, 108, 28}, L"Inspect", 0, ButtonStyle::Subtle))
                        ui.Defer([this, pid] { app.OpenProcessWindow(pid); });
                }
            }
            y += h + 10;
        }
        g.PopClip();
        ui.Scroll(Id("diag.health.scroll"), r, y + scroll_ - r.y, off);
        scroll_ = off;
    }

    // Top contributors over the recent history for each resource.
    void DrawInvestigate(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        auto snap = app.Snap();
        if (!snap) return;
        struct Entry { std::wstring name; uint32_t pid; double value; double extra; };
        std::vector<Entry> cpu, mem, io, net;
        for (auto& p : snap->procs) {
            if (p.pid == 0) continue;
            const ProcHist* h = app.FindProcHist(p);
            if (!h) continue;
            std::vector<float> v;
            h->cpu.Tail(60, v);
            double avg = Mean(v);
            cpu.push_back({p.Name(), p.pid, avg, h->cpu.Max()});
            double growth = 0;
            if (h->memLong.Size() >= 2) growth = h->memLong.Last() - h->memLong.At(0);
            mem.push_back({p.Name(), p.pid, static_cast<double>(p.privateBytes), growth});
            h->io.Tail(60, v);
            io.push_back({p.Name(), p.pid, Mean(v), p.ioReadRate + p.ioWriteRate});
            if (snap->netPerProcess) {
                h->net.Tail(60, v);
                net.push_back({p.Name(), p.pid, Mean(v), p.netSend + p.netRecv});
            }
        }
        auto top = [](std::vector<Entry>& v) {
            std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.value > b.value; });
            if (v.size() > 8) v.resize(8);
        };
        top(cpu); top(mem); top(io); top(net);
        float colW = (r.w - 16) / 2, cardH = std::max(220.f, (r.h - 16) / 2);
        auto card = [&](float x, float y, const std::wstring& title, const std::wstring& subtitle, const std::vector<Entry>& v,
                        const std::function<std::wstring(const Entry&)>& fmt, const std::wstring& empty) {
            Rect c{x, y, colW, cardH};
            ui.Card(c);
            g.Text(title, {c.x + 16, c.y + 10, c.w - 32, 24}, Font::BodySemi, t.text);
            g.Text(subtitle, {c.x + 16, c.y + 32, c.w - 32, 18}, Font::Caption, t.textTertiary);
            float yy = c.y + 56;
            if (v.empty()) g.Paragraph(empty, {c.x + 16, yy, c.w - 32, 60}, Font::Small, t.textSecondary);
            for (size_t i = 0; i < v.size() && yy + 22 < c.B(); ++i) {
                WidgetId id = IdMix(Id("diag.inv"), std::hash<std::wstring>{}(title) ^ v[i].pid);
                Rect rr{c.x + 8, yy, c.w - 16, 22};
                if (ui.Hit(id, rr)) g.FillRound(rr, 4.f, t.hover);
                g.Text(v[i].name + L"  (" + std::to_wstring(v[i].pid) + L")", {rr.x + 8, yy, rr.w * 0.6f, 22}, Font::Small, t.text);
                g.Text(fmt(v[i]), {rr.x + rr.w * 0.6f, yy, rr.w * 0.4f - 8, 22}, Font::Small, t.textSecondary, Align::Right);
                if (ui.DoubleClicked(id)) { uint32_t pid = v[i].pid; ui.Defer([this, pid] { app.OpenProcessWindow(pid); }); }
                yy += 22;
            }
        };
        card(r.x, r.y, L"High CPU usage", L"Average over the last 60 samples (peak in parentheses). Double-click to inspect.", cpu,
             [](const Entry& e) { return FormatPercent(e.value, 1) + L" (" + FormatPercent(e.extra, 0) + L")"; }, L"No data yet.");
        card(r.x + colW + 16, r.y, L"High memory usage", L"Private bytes now, and change over the long history window", mem, [](const Entry& e) {
            std::wstring s = FormatMB(static_cast<uint64_t>(e.value));
            if (std::fabs(e.extra) >= 1) s += std::wstring(e.extra > 0 ? L"  +" : L"  -") + FormatMB(static_cast<uint64_t>(std::fabs(e.extra) * 1024 * 1024));
            return s;
        }, L"No data yet.");
        card(r.x, r.y + cardH + 16, L"Disk / I/O activity", L"Average process I/O rate over the last 60 samples", io,
             [](const Entry& e) { return FormatRateBytes(e.value); }, L"No data yet.");
        card(r.x + colW + 16, r.y + cardH + 16, L"Network activity", L"Average per-process throughput over the last 60 samples", net,
             [this](const Entry& e) { return FormatNetRate(e.value, app.NetUnits()); }, app.Collector().NetTraceStatus());
    }

    void DrawSystem(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        auto& st = app.Static();
        if (!st.sysLoaded) { g.Text(L"Loading…", r, Font::Body, ui.t.textSecondary, Align::Center); return; }
        const SystemStatic& s = st.sys;
        const CpuStatic& c = app.Collector().Cpu();
        auto snap = app.Snap();
        float colW = (r.w - 16) / 2;
        auto kvCard = [&](float x, float y, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& rows) {
            float h = 48 + rows.size() * 22.f;
            ui.Card({x, y, colW, h});
            g.Text(title, {x + 16, y + 10, colW - 32, 24}, Font::BodySemi, ui.t.text);
            float yy = y + 40;
            for (auto& [k, v] : rows) { ui.KeyValue(x + 16, yy, 250, colW - 282, k, v); yy += 22; }
            return h;
        };
        std::wstring ver = s.osName + L" " + s.displayVersion;
        std::wstring build = std::to_wstring(s.build) + L"." + std::to_wstring(s.ubr);
        float h1 = kvCard(r.x, r.y, L"Windows",
                          {{L"Edition", ver},
                           {L"OS build", build},
                           {L"Architecture", s.architecture},
                           {L"Computer name", s.computerName},
                           {L"Installed", s.installDate},
                           {L"Last boot", LocalTime(s.bootTimeFt)},
                           {L"Uptime", snap ? FormatUptime(snap->cpu.uptimeSec) : L""},
                           {L"Registered owner", s.registeredOwner}});
        float h2 = kvCard(r.x + colW + 16, r.y, L"Security and firmware",
                          {{L"Boot mode", s.bootMode},
                           {L"Secure Boot", s.secureBoot},
                           {L"TPM", s.tpm + (s.tpmVersion.empty() ? L"" : L" (version " + s.tpmVersion + L")")},
                           {L"Virtualization-based security", s.vbs},
                           {L"Memory integrity (HVCI)", s.hvci},
                           {L"CPU virtualization", c.virtualizationFirmware ? L"Enabled" : L"Disabled"},
                           {L"BIOS", s.smbios.biosVendor + L" " + s.smbios.biosVersion},
                           {L"BIOS date", s.smbios.biosDate}});
        float y = r.y + std::max(h1, h2) + 16;
        std::vector<std::pair<std::wstring, std::wstring>> hw = {{L"Processor", c.brand},
                                                                 {L"Memory", FormatBytes(st.mem.installed) + (st.mem.type.empty() ? L"" : L" " + st.mem.type) +
                                                                                 (st.mem.configuredSpeedMTs ? L" @ " + std::to_wstring(st.mem.configuredSpeedMTs) + L" MT/s" : L"")},
                                                                 {L"System", s.smbios.systemManufacturer + L" " + s.smbios.systemProduct},
                                                                 {L"Motherboard", s.smbios.boardManufacturer + L" " + s.smbios.boardProduct}};
        for (auto& gpu : st.gpus) hw.push_back({L"Graphics", gpu.name});
        for (auto& d : st.disks) hw.push_back({L"Disk " + std::to_wstring(d.index), d.model + L" (" + FormatBytes(d.size) + L")"});
        kvCard(r.x, y, L"Hardware summary", hw);
        auto cnt = [](int n) { return n < 0 ? std::wstring(L"Loading…") : std::to_wstring(n); };
        kvCard(r.x + colW + 16, y, L"Reliability (event log)",
               {{L"App crashes / hangs, 24 h", cnt(app.events.crashes24h)},
                {L"Unexpected shutdowns, 30 days", cnt(app.events.kernelPower30d)},
                {L"Hardware errors (WHEA), 30 days", cnt(app.events.whea30d)},
                {L"System errors, 24 h", cnt(app.events.errors24h)}});
    }

    void DrawGrid(Ui& ui, const Rect& r, Table& t, SimpleGrid& g, bool loading) {
        if (loading && g.rows.empty()) { ui.g.Text(L"Loading…", r, Font::Body, ui.t.textSecondary, Align::Center); return; }
        if (g.Dirty() || t.sortChanged) { g.Refresh(t); t.sortChanged = false; }
        t.Draw(ui, r, g);
        if (t.contextMenu) {
            t.contextMenu = false;
            int row = t.SelectedRow(g);
            if (const GridRow* gr = row >= 0 ? g.RowAt(row) : nullptr) {
                std::wstring line;
                for (auto& c : gr->cells) { if (!line.empty()) line += L"\t"; line += c; }
                std::wstring last = gr->cells.back();
                ui.Defer([this, line, last] {
                    UINT c = app.ShowMenu({{1, L"Copy row"}, {2, L"Open file location", false, last.find(L'\\') != std::wstring::npos}, {3, L"Search online"}});
                    if (c == 1) CopyTextToClipboard(app.Hwnd(), line);
                    if (c == 2) OpenFileLocation(last);
                    if (c == 3) SearchOnline(line.substr(0, 200));
                });
            }
        }
        if (t.headerMenu) { t.headerMenu = false; Table* tp = &t; ui.Defer([this, tp] { ColumnMenu(app, *tp); }); }
    }

    void DrawEvents(Ui& ui, Rect r) {
        Rect detail = r.TakeBottom(std::min(160.f, r.h * 0.3f));
        DrawGrid(ui, r, events_, evGrid_, loadingEvents_);
        int row = events_.SelectedRow(evGrid_);
        const GridRow* gr = row >= 0 ? evGrid_.RowAt(row) : nullptr;
        ui.Card(detail.Inset(0, 6));
        if (gr) {
            ui.g.Text(gr->cells[3] + L" — event " + gr->cells[4] + L" (" + gr->cells[1] + L")", {detail.x + 16, detail.y + 14, detail.w - 32, 22}, Font::BodySemi, ui.t.text);
            ui.g.Paragraph(gr->cells[5], {detail.x + 16, detail.y + 40, detail.w - 32, detail.h - 52}, Font::Small, ui.t.textSecondary);
        } else {
            ui.g.Text(L"Critical, error and warning events from the System and Application logs in the last 7 days. Select an event to read it.",
                      detail.Inset(16, 0), Font::Small, ui.t.textTertiary);
        }
    }

    void LoadDrivers() {
        if (driversLoaded_ || loadingDrivers_) return;
        loadingDrivers_ = true;
        auto out = std::make_shared<std::vector<DriverInfo>>();
        worker::Run([out] { *out = QueryDrivers(); }, [this, out] {
            loadingDrivers_ = false;
            driversLoaded_ = true;
            drvGrid_.rows.clear();
            uint64_t k = 1;
            for (auto& d : *out) {
                GridRow row;
                row.key = k++;
                const wchar_t* start = d.startType == SERVICE_BOOT_START ? L"Boot" : d.startType == SERVICE_SYSTEM_START ? L"System" : d.startType == SERVICE_AUTO_START ? L"Auto" :
                                       d.startType == SERVICE_DEMAND_START ? L"Manual" : d.startType == SERVICE_DISABLED ? L"Disabled" : L"";
                row.cells = {d.name, d.displayName, ServiceStateName(d.state), start, d.version, d.company, d.path};
                row.highlight = d.state == SERVICE_RUNNING ? 0 : 3;
                drvGrid_.rows.push_back(std::move(row));
            }
            drvGrid_.MarkDirty();
            app.Invalidate();
        });
    }

    void LoadEvents() {
        if (eventsLoaded_ || loadingEvents_) return;
        loadingEvents_ = true;
        auto out = std::make_shared<std::vector<EventEntry>>();
        // On a thread of its own: event-log queries can take many seconds on a busy disk.
        worker::RunLong([out] {
            const wchar_t* q = L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= 604800000]]]";
            *out = QueryEvents(L"System", q, 200, true);
            auto a = QueryEvents(L"Application", q, 150, true);
            out->insert(out->end(), a.begin(), a.end());
        }, [this, out] {
            loadingEvents_ = false;
            eventsLoaded_ = true;
            evGrid_.rows.clear();
            uint64_t k = 1;
            for (auto& e : *out) {
                GridRow row;
                row.key = k++;
                std::wstring msg = e.message;
                if (msg.empty()) for (auto& d : e.data) { if (!msg.empty()) msg += L"; "; msg += d; }
                row.cells = {LocalTime(e.time), EventLevelName(e.level), e.channel, e.provider, std::to_wstring(e.id), msg};
                row.nums = {static_cast<double>(e.time), static_cast<double>(e.level), NoNum(), NoNum(), static_cast<double>(e.id)};
                if (e.level <= 2) { row.color = app.Th().critical; row.colorCol = 1; }
                else { row.color = app.Th().warning; row.colorCol = 1; }
                evGrid_.rows.push_back(std::move(row));
            }
            evGrid_.MarkDirty();
            app.Invalidate();
        });
    }

    void LoadCrashes() {
        if (crashesLoaded_ || loadingCrashes_) return;
        loadingCrashes_ = true;
        auto out = std::make_shared<std::vector<EventEntry>>();
        // On a thread of its own, like LoadEvents.
        worker::RunLong([out] {
            *out = QueryEvents(L"Application", L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 2592000000]]]", 300, false);
            auto bc = QueryEvents(L"System", L"*[System[Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001]]", 50, true);
            out->insert(out->end(), bc.begin(), bc.end());
        }, [this, out] {
            loadingCrashes_ = false;
            crashesLoaded_ = true;
            crGrid_.rows.clear();
            uint64_t k = 1;
            for (auto& e : *out) {
                GridRow row;
                row.key = k++;
                auto d = [&](size_t i) { return i < e.data.size() ? e.data[i] : std::wstring(); };
                if (e.channel == L"System") {
                    row.cells = {LocalTime(e.time), L"Stop error (bugcheck)", L"Windows", L"", L"", d(0), L"", e.message.substr(0, 300)};
                    row.color = app.Th().critical;
                    row.colorCol = 1;
                } else if (e.id == 1002) {
                    row.cells = {LocalTime(e.time), L"Hang", d(0), d(1), L"", L"", L"", d(5)};
                } else {
                    // Application Error 1000: AppName, AppVersion, AppTimeStamp, ModuleName, ModuleVersion, ModuleTimeStamp, ExceptionCode, FaultingOffset, PID, ..., AppPath, ModulePath
                    row.cells = {LocalTime(e.time), L"Crash", d(0), d(1), d(3) + (d(4).empty() ? L"" : L" " + d(4)), d(6), d(7), d(10)};
                }
                row.nums = {static_cast<double>(e.time)};
                crGrid_.rows.push_back(std::move(row));
            }
            crGrid_.MarkDirty();
            app.Invalidate();
        });
    }

    int tab_ = 0;
    float scroll_ = 0;
    Table drivers_, events_, crashes_;
    SimpleGrid drvGrid_, evGrid_, crGrid_;
    bool driversLoaded_ = false, loadingDrivers_ = false, eventsLoaded_ = false, loadingEvents_ = false, crashesLoaded_ = false, loadingCrashes_ = false;
};

} // namespace

std::unique_ptr<Page> MakeDiagnosticsPage(App& app) { return std::make_unique<DiagnosticsPage>(app); }

} // namespace nx
