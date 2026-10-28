// Overview: customizable dashboard of widgets (add / remove / reorder / resize, compact or detailed).
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

namespace nx {

namespace {

enum class W { Cpu, Memory, Gpu, Disk, Network, System, TopCpu, TopMem, Alerts, Startup, Temps, Count };
const wchar_t* kNames[] = {L"CPU", L"Memory", L"GPU", L"Disk", L"Network", L"System", L"Top CPU processes", L"Top memory processes", L"Health", L"Startup impact", L"Temperatures"};
const char* kKeys[] = {"cpu", "mem", "gpu", "disk", "net", "sys", "topcpu", "topmem", "alerts", "startup", "temps"};

struct Widget { W type; int span = 1; };

class OverviewPage final : public Page {
public:
    explicit OverviewPage(App& a) : Page(a) {
        Load();
        detailed_ = app.Config().GetBool("overview.detailed", true);
    }
    const wchar_t* Title() const override { return L"Overview"; }
    wchar_t Glyph() const override { return 0xE80F; }
    uint32_t Demand() const override { return demand::Disk | demand::GpuSystem | demand::GpuSensors | demand::Thermal; }
    void OnShow() override {
        app.EnsureSystemInfo();
        app.EnsureDiskInfo(true);
        if (!startupLoaded_ && !startupLoading_) {
            startupLoading_ = true;
            auto out = std::make_shared<std::pair<int, int>>();
            worker::Run([out] {
                auto items = QueryStartupItems();
                out->first = static_cast<int>(items.size());
                for (auto& i : items) out->second += i.enabled ? 1 : 0;
            }, [this, out] {
                startupTotal_ = out->first;
                startupEnabled_ = out->second;
                startupLoaded_ = true;
                startupLoading_ = false;
                app.Invalidate();
            });
        }
    }
    void SaveState() override {
        Save();
        app.Config().SetBool("overview.detailed", detailed_);
    }
    std::wstring AccessibleSummary() const override {
        auto s = app.Snap();
        if (!s) return L"Overview";
        return L"Overview. CPU " + FormatPercent(s->cpu.total) + L", memory " + FormatPercent(s->mem.total ? 100.0 * s->mem.InUse() / s->mem.total : 0) + L", " +
               std::to_wstring(app.Alerts().size()) + L" health findings.";
    }

    void Draw(Ui& ui, const Rect& area) override {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        Rect r = area;
        Rect bar = r.TakeTop(44);
        auto snap = app.Snap();
        std::wstring greet = app.Static().sysLoaded ? app.Static().sys.computerName + L" · " + app.Static().sys.osName + L" " + app.Static().sys.displayVersion : L"";
        g.Text(greet, {bar.x, bar.y, bar.w - 420, 40}, Font::Small, t.textSecondary);
        if (ui.Button(Id("ov.edit"), {bar.R() - 120, bar.y + 4, 120, 32}, editing_ ? L"Done" : L"Customize", editing_ ? 0xE73E : 0xE70F,
                      editing_ ? ButtonStyle::Accent : ButtonStyle::Standard))
            editing_ = !editing_;
        if (ui.Button(Id("ov.mode"), {bar.R() - 260, bar.y + 4, 132, 32}, detailed_ ? L"Detailed" : L"Compact", detailed_ ? 0xE9D2 : 0xE8FD)) detailed_ = !detailed_;
        if (editing_ && ui.Button(Id("ov.add"), {bar.R() - 390, bar.y + 4, 122, 32}, L"Add widget", 0xE710)) ui.Defer([this] { AddMenu(); });
        if (!snap) { g.Text(L"Collecting data…", r, Font::Body, t.textSecondary, Align::Center); return; }

        int cols = std::clamp(static_cast<int>(r.w / 300), 1, 4);
        float gap = 14;
        float colW = (r.w - gap * (cols - 1) - 10) / cols;
        float off = scroll_;
        g.PushClip(r);
        float y = r.y - scroll_;
        int col = 0;
        float rowH = 0;
        int index = 0;
        for (size_t i = 0; i < widgets_.size(); ++i) {
            Widget& w = widgets_[i];
            int span = std::min(w.span, cols);
            if (col + span > cols) { y += rowH + gap; col = 0; rowH = 0; }
            float h = Height(w.type);
            Rect wr{r.x + col * (colW + gap), y, colW * span + gap * (span - 1), h};
            if (wr.B() > r.y && wr.y < r.B()) DrawWidget(ui, wr, w, *snap, static_cast<int>(i));
            rowH = std::max(rowH, h);
            col += span;
            ++index;
        }
        y += rowH + gap;
        g.PopClip();
        ui.Scroll(Id("ov.scroll"), r, y + scroll_ - r.y, off);
        scroll_ = off;
        if (widgets_.empty()) g.Text(L"All widgets are hidden. Use Customize › Add widget.", r, Font::Body, t.textSecondary, Align::Center);
    }

private:
    float Height(W type) const {
        bool big = type == W::TopCpu || type == W::TopMem || type == W::Alerts;
        if (!detailed_) return big ? 200.f : 104.f;
        return big ? 270.f : 186.f;
    }

    void Load() {
        auto list = app.Config().GetList("overview.widgets");
        widgets_.clear();
        for (auto& s : list) {
            size_t c = s.find(L':');
            std::string key = ToUtf8(s.substr(0, c));
            int span = c == std::wstring::npos ? 1 : _wtoi(s.c_str() + c + 1);
            for (int i = 0; i < static_cast<int>(W::Count); ++i)
                if (key == kKeys[i]) widgets_.push_back({static_cast<W>(i), std::clamp(span, 1, 2)});
        }
        if (widgets_.empty() && !app.Config().Has("overview.widgets"))
            widgets_ = {{W::Cpu, 1}, {W::Memory, 1}, {W::Gpu, 1}, {W::Disk, 1}, {W::Network, 1}, {W::Temps, 1}, {W::System, 1}, {W::Startup, 1},
                        {W::TopCpu, 1}, {W::TopMem, 1}, {W::Alerts, 2}};
    }
    void Save() {
        std::vector<std::wstring> v;
        for (auto& w : widgets_) v.push_back(FromUtf8(kKeys[static_cast<int>(w.type)]) + L":" + std::to_wstring(w.span));
        app.Config().SetList("overview.widgets", v);
    }

    void AddMenu() {
        std::vector<MenuItem> items;
        for (int i = 0; i < static_cast<int>(W::Count); ++i) {
            bool present = std::any_of(widgets_.begin(), widgets_.end(), [&](const Widget& w) { return static_cast<int>(w.type) == i; });
            items.push_back({static_cast<UINT>(i + 1), kNames[i], present, !present});
        }
        items.push_back(MenuItem::Sep());
        items.push_back({100, L"Reset to default layout"});
        UINT c = app.ShowMenu(items);
        if (c == 100) { app.Config().Remove("overview.widgets"); Load(); }
        else if (c >= 1 && c <= static_cast<UINT>(W::Count)) widgets_.push_back({static_cast<W>(c - 1), 1});
        Save();
    }

    void DrawWidget(Ui& ui, const Rect& r, Widget& w, const Snapshot& s, int idx) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        WidgetId wid = IdMix(Id("ov.w"), static_cast<uint64_t>(idx) * 31 + static_cast<int>(w.type));
        bool hot = ui.Hit(wid, r);
        ui.Card(r, hot && !editing_);
        Rect in = r.Inset(16, 12);
        g.Text(kNames[static_cast<int>(w.type)], {in.x, in.y, in.w - (editing_ ? 120 : 0), 22}, Font::BodySemi, t.text);
        // Clicking a widget opens the relevant detailed page.
        if (!editing_ && ui.Clicked(wid)) {
            PageId target = PageId::Count;
            switch (w.type) {
            case W::Cpu: case W::Memory: case W::Gpu: case W::Disk: target = PageId::Performance; break;
            case W::Network: target = PageId::Network; break;
            case W::TopCpu: case W::TopMem: target = PageId::Processes; break;
            case W::Alerts: target = PageId::Diagnostics; break;
            case W::Startup: target = PageId::Startup; break;
            case W::Temps: target = PageId::Hardware; break;
            case W::System: target = PageId::Diagnostics; break;
            default: break;
            }
            if (target != PageId::Count) {
                if (w.type == W::Memory) app.Config().SetString("performance.device", L"mem");
                if (w.type == W::Cpu) app.Config().SetString("performance.device", L"cpu");
                ui.Defer([this, target] { app.Navigate(target); });
            }
        }
        if (editing_) {
            float bx = in.R() - 112;
            if (ui.IconButton(IdMix(wid, 1), {bx, in.y - 4, 28, 28}, 0xE76B, L"Move earlier", idx > 0)) std::swap(widgets_[idx], widgets_[idx - 1]), Save();
            if (ui.IconButton(IdMix(wid, 2), {bx + 28, in.y - 4, 28, 28}, 0xE76C, L"Move later", idx + 1 < static_cast<int>(widgets_.size())))
                std::swap(widgets_[idx], widgets_[idx + 1]), Save();
            if (ui.IconButton(IdMix(wid, 3), {bx + 56, in.y - 4, 28, 28}, 0xE740, w.span == 1 ? L"Make wide" : L"Make narrow")) { w.span = w.span == 1 ? 2 : 1; Save(); }
            if (ui.IconButton(IdMix(wid, 4), {bx + 84, in.y - 4, 28, 28}, 0xE711, L"Remove widget")) {
                widgets_.erase(widgets_.begin() + idx);
                Save();
                return;
            }
        }
        Rect body{in.x, in.y + 28, in.w, in.h - 28};
        auto& h = app.Hist();
        auto metric = [&](const std::wstring& big, const std::wstring& detail, const RingBuffer<float>* data, float maxV, Color c, float fraction) {
            g.Text(big, {body.x, body.y, body.w * 0.6f, 36}, Font::Display, c);
            g.Text(detail, {body.x, body.y + 38, body.w, 18}, Font::Small, t.textSecondary);
            if (fraction >= 0) ui.Meter({body.x, body.y + 62, body.w, 5}, fraction, c);
            if (detailed_ && data && data->Size() > 1) GraphView::Spark(g, {body.x, body.y + 74, body.w, body.h - 76}, *data, maxV, c);
        };
        switch (w.type) {
        case W::Cpu: {
            double tz = -100;
            for (auto& [n, v] : s.thermalZonesC) tz = std::max(tz, v);
            metric(FormatPercent(s.cpu.total), FormatFreqMHz(s.cpu.currentMHz) + L" · " + std::to_wstring(s.cpu.processes) + L" processes" +
                                                   (tz > -50 ? L" · " + TempText(app, tz) : L""),
                   &h.cpu, 100, t.cpu, s.cpu.total / 100.f);
            break;
        }
        case W::Memory: {
            float pct = s.mem.total ? 100.f * s.mem.InUse() / s.mem.total : 0.f;
            metric(FormatPercent(pct), FormatBytes(s.mem.InUse()) + L" of " + FormatBytes(s.mem.total) + L" · commit " +
                                           FormatPercent(s.mem.commitLimit ? 100.0 * s.mem.commit / s.mem.commitLimit : 0),
                   &h.memPct, 100, t.memory, pct / 100.f);
            break;
        }
        case W::Gpu: {
            if (s.gpus.empty()) { g.Text(s.gpuCollected ? L"No GPU detected" : L"Collecting…", body, Font::Body, t.textSecondary); break; }
            const GpuSnap* best = &s.gpus[0];
            for (auto& gp : s.gpus) if (gp.util > best->util) best = &gp;
            std::wstring sub = best->name + (best->temperatureC > 0 ? L" · " + TempText(app, best->temperatureC) : L"") + L" · " + FormatBytes(best->dedicatedUsed) + L" VRAM";
            metric(FormatPercent(best->util), sub, &h.gpuMax, 100, t.gpu, best->util / 100.f);
            break;
        }
        case W::Disk: {
            double rd = 0, wr = 0;
            float act = 0;
            for (auto& d : s.disks) { rd += d.readBps; wr += d.writeBps; act = std::max(act, d.activePct); }
            float mx = std::max(1024.f * 1024.f, std::max(h.diskTotalRead.Max(), h.diskTotalWrite.Max()) * 1.2f);
            metric(FormatRateBytes(rd + wr), L"Read " + FormatRateBytes(rd) + L" · Write " + FormatRateBytes(wr) + L" · busiest " + FormatPercent(act),
                   &h.diskTotalRead, mx, t.disk, act / 100.f);
            break;
        }
        case W::Network: {
            double se = 0, re = 0;
            for (auto& n : s.nics) { se += n.sendBps; re += n.recvBps; }
            float mx = std::max(1024.f, std::max(h.netTotalSend.Max(), h.netTotalRecv.Max()) * 1.2f);
            g.Text(L"↓ " + FormatNetRate(re, app.NetUnits()), {body.x, body.y, body.w / 2, 30}, Font::Title, t.network);
            g.Text(L"↑ " + FormatNetRate(se, app.NetUnits()), {body.x + body.w / 2, body.y, body.w / 2, 30}, Font::Title, t.network.Mix(t.text, 0.4f));
            size_t up = std::count_if(s.nics.begin(), s.nics.end(), [](const NicSnap& n) { return n.connected; });
            g.Text(std::to_wstring(up) + L" connected adapter(s)", {body.x, body.y + 38, body.w, 18}, Font::Small, t.textSecondary);
            if (detailed_ && h.netTotalRecv.Size() > 1) {
                Rect gr{body.x, body.y + 74, body.w, body.h - 76};
                GraphView::Spark(g, gr, h.netTotalRecv, mx, t.network);
                GraphView::Spark(g, gr, h.netTotalSend, mx, t.network.Mix(t.text, 0.4f), false);
            }
            break;
        }
        case W::System: {
            float y = body.y;
            auto line = [&](const std::wstring& k, const std::wstring& v) {
                g.Text(k, {body.x, y, 110, 20}, Font::Small, t.textSecondary);
                g.Text(v, {body.x + 110, y, body.w - 110, 20}, Font::Small, t.text);
                y += 21;
            };
            line(L"Uptime", FormatUptime(s.cpu.uptimeSec));
            line(L"Processes", FormatCount(s.cpu.processes));
            line(L"Threads", FormatCount(s.cpu.threads));
            line(L"Handles", FormatCount(s.cpu.handles));
            if (detailed_) {
                line(L"Processor", app.Collector().Cpu().brand);
                if (app.Static().sysLoaded) line(L"Build", std::to_wstring(app.Static().sys.build) + L"." + std::to_wstring(app.Static().sys.ubr));
                line(L"Rights", app.Elevated() ? L"Administrator" : L"Standard user");
            }
            break;
        }
        case W::TopCpu: case W::TopMem: {
            std::vector<const ProcSample*> v;
            for (auto& p : s.procs) if (p.pid) v.push_back(&p);
            bool cpu = w.type == W::TopCpu;
            size_t n = std::min<size_t>(v.size(), detailed_ ? 8 : 6);
            std::partial_sort(v.begin(), v.begin() + n, v.end(), [cpu](auto* a, auto* b) { return cpu ? a->cpu > b->cpu : a->privateWorkingSet > b->privateWorkingSet; });
            float y = body.y;
            for (size_t i = 0; i < n; ++i) {
                auto* p = v[i];
                Rect rr{body.x - 6, y, body.w + 12, 24};
                WidgetId pid = IdMix(wid, 100 + p->pid);
                if (ui.Hit(pid, rr)) g.FillRound(rr, 4.f, t.hover);
                if (ID2D1Bitmap* icon = app.Icons().Get(p->meta ? p->meta->path : L"")) g.DrawBitmap(icon, {body.x, y + 4, 16, 16});
                g.Text(p->meta ? p->meta->DisplayName() : L"", {body.x + 24, y, body.w * 0.62f - 24, 24}, Font::Small, t.text);
                std::wstring val = cpu ? FormatPercent(p->cpu, 1) : FormatMB(p->privateWorkingSet);
                float frac = cpu ? p->cpu / 100.f : s.mem.total ? static_cast<float>(p->privateWorkingSet) / s.mem.total : 0.f;
                ui.Meter({body.x + body.w * 0.62f, y + 10, body.w * 0.18f, 4}, frac * (cpu ? 1.f : 4.f), cpu ? t.cpu : t.memory);
                g.Text(val, {body.x + body.w * 0.8f, y, body.w * 0.2f, 24}, Font::Small, t.textSecondary, Align::Right);
                if (ui.DoubleClicked(pid)) { uint32_t id = p->pid; ui.Defer([this, id] { app.OpenProcessWindow(id); }); }
                y += 25;
            }
            break;
        }
        case W::Alerts: {
            const auto& alerts = app.Alerts();
            if (alerts.empty()) {
                g.FillEllipse(body.x + 8, body.y + 12, 6, 6, t.good);
                g.Text(L"No problems detected", {body.x + 24, body.y, body.w - 24, 24}, Font::Body, t.text);
                g.Paragraph(L"NEXUS watches CPU, memory, disk latency, network errors, disk space, drive health, temperatures and the event log. Findings appear here with their evidence.",
                            {body.x, body.y + 32, body.w, body.h - 32}, Font::Small, t.textSecondary);
                break;
            }
            float y = body.y;
            for (size_t i = 0; i < alerts.size() && y + 40 < body.B(); ++i) {
                auto& a = alerts[i];
                Color c = a.severity == DiagAlert::Critical ? t.critical : a.severity == DiagAlert::Warning ? t.warning : t.info;
                g.FillEllipse(body.x + 6, y + 11, 5, 5, c);
                g.Text(a.title, {body.x + 20, y, body.w - 20, 22}, Font::Small, t.text);
                if (!a.evidence.empty()) g.Text(a.evidence.front(), {body.x + 20, y + 19, body.w - 20, 18}, Font::Caption, t.textSecondary);
                y += 42;
            }
            break;
        }
        case W::Startup: {
            if (!startupLoaded_) { g.Text(L"Loading…", body, Font::Body, t.textSecondary); break; }
            g.Text(std::to_wstring(startupEnabled_), {body.x, body.y, 80, 36}, Font::Display, t.accentText);
            g.Text(L"of " + std::to_wstring(startupTotal_) + L" startup apps enabled", {body.x + 70, body.y + 10, body.w - 70, 22}, Font::Body, t.textSecondary);
            if (detailed_)
                g.Paragraph(L"Startup impact ratings require Windows boot traces that are not publicly accessible; NEXUS lists which startup apps are running and what they use now.",
                            {body.x, body.y + 46, body.w, body.h - 46}, Font::Small, t.textTertiary);
            break;
        }
        case W::Temps: {
            float y = body.y;
            auto line = [&](const std::wstring& k, double c) {
                if (y + 20 > body.B()) return;
                g.Text(k, {body.x, y, body.w * 0.65f, 20}, Font::Small, t.textSecondary);
                Color col = c >= 90 ? t.critical : c >= 75 ? t.warning : t.text;
                g.Text(TempText(app, c), {body.x + body.w * 0.65f, y, body.w * 0.35f, 20}, Font::BodySemi, col, Align::Right);
                y += 22;
            };
            int any = 0;
            for (auto& [n, v] : s.thermalZonesC) { line(L"Thermal zone " + n, v); ++any; }
            for (size_t i = 0; i < s.gpus.size(); ++i) if (s.gpus[i].temperatureC > 0) { line(L"GPU " + std::to_wstring(i), s.gpus[i].temperatureC); ++any; }
            for (auto& [diskIdx, hh] : app.Static().health) if (hh.temperatureC > -50) { line(L"Disk " + std::to_wstring(diskIdx), hh.temperatureC); ++any; }
            if (!any) g.Paragraph(L"No temperature sensors are exposed through Windows on this system. CPU die temperatures need a kernel driver, which NEXUS does not install.",
                                  body, Font::Small, t.textSecondary);
            break;
        }
        default: break;
        }
    }

    std::vector<Widget> widgets_;
    bool editing_ = false, detailed_ = true;
    float scroll_ = 0;
    bool startupLoaded_ = false, startupLoading_ = false;
    int startupTotal_ = 0, startupEnabled_ = 0;
};

} // namespace

std::unique_ptr<Page> MakeOverviewPage(App& app) { return std::make_unique<OverviewPage>(app); }

} // namespace nx
