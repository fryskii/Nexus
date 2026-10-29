// Performance page: Task Manager's CPU / Memory / Disk / Network / GPU views with extended data.
#include "ui/pages/Common.h"

#include <shellapi.h>

#include <map>

namespace nx {

namespace {

struct Device {
    enum Kind { Cpu, Mem, Disk, Nic, Gpu } kind;
    int index = 0;       // disk number
    uint64_t id = 0;     // nic / gpu luid
    std::wstring key;
};

class PerformancePage final : public Page {
public:
    explicit PerformancePage(App& a) : Page(a) {
        selected_ = app.Config().GetString("performance.device", L"cpu");
        logical_ = app.Config().GetBool("performance.cpu.logical", false);
        kernel_ = app.Config().GetBool("performance.cpu.kernel", false);
        summary_ = app.Config().GetBool("performance.summary", false);
    }
    const wchar_t* Title() const override { return L"Performance"; }
    wchar_t Glyph() const override { return 0xE9D2; }
    uint32_t Demand() const override { return demand::Disk | demand::PerCore | demand::GpuSystem | demand::GpuSensors | demand::Thermal; }
    void OnShow() override {
        // The insights rail picks a device by setting performance.device before navigating here.
        selected_ = app.Config().GetString("performance.device", selected_);
        summary_ = app.Config().GetBool("performance.summary", summary_);
        app.EnsureSystemInfo();
        app.EnsureDiskInfo(false);
        app.EnsureNicInfo();
        app.EnsureGpuInfo();
    }
    void OnHide() override { SaveState(); }
    void SaveState() override {
        app.Config().SetString("performance.device", selected_);
        app.Config().SetBool("performance.cpu.logical", logical_);
        app.Config().SetBool("performance.cpu.kernel", kernel_);
        app.Config().SetBool("performance.summary", summary_);
    }
    std::wstring AccessibleSummary() const override { return L"Performance. " + summaryText_; }
    bool OnKey(UINT vk, bool ctrl, bool) override {
        if (vk == VK_UP || vk == VK_DOWN) {
            auto devs = Devices();
            for (size_t i = 0; i < devs.size(); ++i) {
                if (devs[i].key != selected_) continue;
                size_t n = vk == VK_UP ? (i ? i - 1 : 0) : std::min(devs.size() - 1, i + 1);
                selected_ = devs[n].key;
                return true;
            }
            if (!devs.empty()) selected_ = devs[0].key;
            return true;
        }
        if (ctrl && vk == 'C') { CopyTextToClipboard(app.Hwnd(), copyText_); app.ShowStatus(L"Copied performance summary"); return true; }
        return false;
    }

    void Draw(Ui& ui, const Rect& area) override {
        auto snap = app.Snap();
        if (!snap) { ui.g.Text(L"Collecting data…", area, Font::Body, ui.t.textSecondary, Align::Center); return; }
        auto devs = Devices();
        if (std::none_of(devs.begin(), devs.end(), [&](const Device& d) { return d.key == selected_; })) selected_ = L"cpu";
        Rect r = area;
        r.y += 4;
        r.h -= 8;
        float listW = summary_ ? r.w : std::min(250.f, r.w * 0.3f);
        Rect list = r.TakeLeft(listW);
        DrawDeviceList(ui, list, devs, *snap);
        if (summary_) return;
        r.x += 16;
        r.w -= 16;
        for (auto& d : devs) {
            if (d.key != selected_) continue;
            switch (d.kind) {
            case Device::Cpu: DrawCpu(ui, r, *snap); break;
            case Device::Mem: DrawMem(ui, r, *snap); break;
            case Device::Disk: DrawDisk(ui, r, *snap, d.index); break;
            case Device::Nic: DrawNic(ui, r, *snap, d.id); break;
            case Device::Gpu: DrawGpu(ui, r, *snap, d.id); break;
            }
        }
        // Right-click anywhere in the detail area (no hit region, so graphs keep their hover read-out).
        if (ui.in.rReleased && r.Contains(ui.in.mx, ui.in.my)) ui.Defer([this] { ContextMenu(); });
    }

private:
    std::vector<Device> Devices() const {
        std::vector<Device> v;
        v.push_back({Device::Cpu, 0, 0, L"cpu"});
        v.push_back({Device::Mem, 0, 0, L"mem"});
        auto snap = app.Snap();
        if (!snap) return v;
        for (auto& d : snap->disks) v.push_back({Device::Disk, d.index, 0, L"disk:" + std::to_wstring(d.index)});
        for (auto& n : snap->nics) v.push_back({Device::Nic, 0, n.luid, L"nic:" + std::to_wstring(n.luid)});
        for (auto& g : snap->gpus) v.push_back({Device::Gpu, 0, g.luid, L"gpu:" + std::to_wstring(g.luid)});
        return v;
    }

    const DiskStatic* DiskInfo(int idx) const {
        for (auto& d : app.Static().disks) if (d.index == idx) return &d;
        return nullptr;
    }
    const NicStatic* NicInfo(uint64_t luid) const {
        for (auto& n : app.Static().nics) if (n.luid == luid) return &n;
        return nullptr;
    }
    const GpuStatic* GpuInfo(uint64_t luid) const {
        for (auto& g : app.Static().gpus) if (g.luid == luid) return &g;
        return nullptr;
    }

    void ContextMenu() {
        UINT c = app.ShowMenu({{1, L"Change graph to: Overall utilization", !logical_, selected_ == L"cpu", false, true},
                               {2, L"Change graph to: Logical processors", logical_, selected_ == L"cpu", false, true},
                               {3, L"Show kernel times", kernel_, selected_ == L"cpu"},
                               MenuItem::Sep(),
                               {4, L"Summary view", summary_},
                               {5, L"Copy\tCtrl+C"},
                               MenuItem::Sep(),
                               {8, L"Open Resource Monitor"},
                               {6, L"Export session history (CSV)..."},
                               {7, L"Export session history (JSON)..."}});
        if (c == 1) logical_ = false;
        if (c == 2) logical_ = true;
        if (c == 3) kernel_ = !kernel_;
        if (c == 4) summary_ = !summary_;
        if (c == 5) { CopyTextToClipboard(app.Hwnd(), copyText_); app.ShowStatus(L"Copied performance summary"); }
        if (c == 6) app.ExportHistory(false);
        if (c == 7) app.ExportHistory(true);
        if (c == 8) ShellExecuteW(app.Hwnd(), L"open", L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ------------------------------------------------------------------ device list
    void DrawDeviceList(Ui& ui, const Rect& r, const std::vector<Device>& devs, const Snapshot& s) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        auto& h = app.Hist();
        float itemH = app.Compact() ? 58.f : 68.f;
        float contentH = devs.size() * (itemH + 4);
        float off = listScroll_;
        ui.Scroll(Id("perf.list.scroll"), r, contentH, off);
        listScroll_ = off;
        g.PushClip(r);
        float y = r.y - listScroll_;
        for (auto& d : devs) {
            Rect ir{r.x, y, r.w - 10, itemH};
            y += itemH + 4;
            if (ir.B() < r.y || ir.y > r.B()) continue;
            WidgetId id = IdMix(Id("perf.dev"), std::hash<std::wstring>{}(d.key));
            bool hot = ui.Hit(id, ir);
            bool sel = d.key == selected_;
            if (sel) g.FillRound(ir, 6.f, t.selection);
            else if (hot) g.FillRound(ir, 6.f, t.hover);
            if (ui.Clicked(id)) { selected_ = d.key; if (summary_) summary_ = false; }
            if (ui.DoubleClicked(id)) summary_ = !summary_;
            Rect spark{ir.x + 8, ir.y + 10, 66, itemH - 20};
            g.Fill(spark, t.graphBg);
            g.Stroke(spark, t.cardBorder);
            std::wstring title, line1, line2;
            Color c = t.cpu;
            const RingBuffer<float>* data = nullptr;
            float maxV = 100;
            switch (d.kind) {
            case Device::Cpu:
                title = L"CPU";
                line1 = FormatPercent(s.cpu.total) + L"  " + FormatFreqMHz(s.cpu.currentMHz);
                data = &h.cpu;
                if (!s.thermalZonesC.empty()) {
                    double mx = -100;
                    for (auto& [n, v] : s.thermalZonesC) mx = std::max(mx, v);
                    line2 = L"Thermal zone " + TempText(app, mx);
                }
                break;
            case Device::Mem:
                title = L"Memory";
                c = t.memory;
                line1 = FormatBytes(s.mem.InUse()) + L"/" + FormatBytes(s.mem.total) + L" (" + FormatPercent(s.mem.total ? 100.0 * s.mem.InUse() / s.mem.total : 0) + L")";
                data = &h.memPct;
                break;
            case Device::Disk: {
                c = t.disk;
                const DiskSnap* ds = nullptr;
                for (auto& x : s.disks) if (x.index == d.index) ds = &x;
                const DiskStatic* st = DiskInfo(d.index);
                title = L"Disk " + std::to_wstring(d.index) + (ds && !ds->letters.empty() ? L" (" + ds->letters + L")" : L"");
                line1 = st ? (st->ssd ? L"SSD" : st->seekPenaltyKnown ? L"HDD" : st->busType) + std::wstring(st->removable ? L" (Removable)" : L"") : L"";
                line2 = ds ? FormatPercent(ds->activePct) : L"";
                auto it = h.disks.find(d.index);
                if (it != h.disks.end()) data = &it->second.active;
                break;
            }
            case Device::Nic: {
                c = t.network;
                const NicSnap* ns = nullptr;
                for (auto& x : s.nics) if (x.luid == d.id) ns = &x;
                title = ns ? (ns->wireless ? L"Wi-Fi" : L"Ethernet") : L"Network";
                line1 = ns ? ns->alias : L"";
                line2 = ns ? (ns->connected ? L"S: " + FormatNetRate(ns->sendBps, app.NetUnits()) + L" R: " + FormatNetRate(ns->recvBps, app.NetUnits()) : L"Not connected") : L"";
                auto it = h.nics.find(d.id);
                if (it != h.nics.end()) {
                    data = &it->second.recv;
                    maxV = std::max(1024.f, std::max(it->second.recv.Max(), it->second.send.Max()) * 1.2f);
                }
                break;
            }
            case Device::Gpu: {
                c = t.gpu;
                const GpuSnap* gs = nullptr;
                size_t gi = 0;
                for (size_t i = 0; i < s.gpus.size(); ++i) if (s.gpus[i].luid == d.id) { gs = &s.gpus[i]; gi = i; }
                title = L"GPU " + std::to_wstring(gi);
                line1 = gs ? gs->name : L"";
                line2 = gs ? FormatPercent(gs->util) + (gs->temperatureC > 0 ? L" (" + TempText(app, gs->temperatureC) + L")" : L"") : L"";
                auto it = h.gpus.find(d.id);
                if (it != h.gpus.end()) data = &it->second.util;
                break;
            }
            }
            if (data) GraphView::Spark(g, spark.Inset(1), *data, maxV, c);
            float tx = spark.R() + 12;
            g.Text(title, {tx, ir.y + 8, ir.R() - tx - 6, 20}, Font::BodySemi, t.text);
            g.Text(line1, {tx, ir.y + 28, ir.R() - tx - 6, 18}, Font::Small, t.textSecondary);
            if (!line2.empty() && itemH > 60) g.Text(line2, {tx, ir.y + 44, ir.R() - tx - 6, 18}, Font::Small, t.textSecondary);
            else if (!line2.empty()) g.Text(line1 + L"  " + line2, {tx, ir.y + 28, ir.R() - tx - 6, 18}, Font::Small, t.textSecondary);
        }
        g.PopClip();
    }

    // ------------------------------------------------------------------ helpers
    struct Stat { std::wstring label, value; };
    // Task Manager style: large live values on the left, static facts in a key/value column on the right.
    float DrawStats(Ui& ui, const Rect& r, const std::vector<Stat>& big, const std::vector<Stat>& facts) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        float bigW = std::min(150.f, r.w * 0.22f);
        int cols = r.w > 760 ? 3 : 2;
        float x = r.x, y = r.y;
        int i = 0;
        for (auto& s : big) {
            g.Text(s.label, {x, y, bigW - 8, 18}, Font::Small, t.textSecondary);
            g.Text(s.value, {x, y + 16, bigW - 8, 28}, Font::StatValue, t.text);
            x += bigW;
            if (++i % cols == 0) { x = r.x; y += 52; }
        }
        float leftH = ((big.size() + cols - 1) / cols) * 52.f;
        float fx = r.x + bigW * cols + 16, fw = r.R() - fx;
        float fy = r.y;
        float kw = std::min(170.f, fw * 0.45f);
        for (auto& f : facts) {
            g.Text(f.label, {fx, fy, kw, 20}, Font::Small, t.textSecondary);
            g.Text(f.value.empty() ? L"—" : f.value, {fx + kw, fy, fw - kw, 20}, Font::Small, t.text);
            fy += 20;
        }
        copyText_.clear();
        for (auto& s : big) copyText_ += s.label + L"\t" + s.value + L"\r\n";
        for (auto& f : facts) copyText_ += f.label + L"\t" + f.value + L"\r\n";
        return std::max(leftH, fy - r.y);
    }

    void Header(Ui& ui, Rect& r, const std::wstring& title, const std::wstring& right) {
        ui.g.Text(title, {r.x, r.y, r.w * 0.4f, 34}, Font::Title, ui.t.text);
        ui.g.Text(right, {r.x + r.w * 0.4f, r.y + 4, r.w * 0.6f, 30}, Font::Body, ui.t.text, Align::Right);
        r.TakeTop(40);
        summaryText_ = title + L". " + right;
    }

    void GraphLabels(Ui& ui, const Rect& gr, const std::wstring& topLeft, const std::wstring& topRight) {
        ui.g.Text(topLeft, {gr.x, gr.y - 18, gr.w / 2, 16}, Font::Caption, ui.t.textSecondary);
        ui.g.Text(topRight, {gr.x + gr.w / 2, gr.y - 18, gr.w / 2, 16}, Font::Caption, ui.t.textSecondary, Align::Right);
    }

    double Interval() const { auto s = app.Snap(); return s ? s->interval : 1.0; }

    // ------------------------------------------------------------------ CPU
    void DrawCpu(Ui& ui, Rect r, const Snapshot& s) {
        const Theme& t = ui.t;
        const CpuStatic& cs = app.Collector().Cpu();
        auto& h = app.Hist();
        Header(ui, r, L"CPU", cs.brand);
        float statsH = 200;
        Rect gr{r.x, r.y + 20, r.w, std::max(120.f, r.h - statsH - 46)};
        GraphLabels(ui, gr, logical_ ? L"Utilization per logical processor" : L"% Utilization", L"100%");
        if (logical_ && !h.cores.empty()) {
            size_t n = h.cores.size();
            int cols = static_cast<int>(std::ceil(std::sqrt(n * gr.w / std::max(1.f, gr.h))));
            cols = std::clamp(cols, 1, static_cast<int>(n));
            int rows = static_cast<int>((n + cols - 1) / cols);
            float cw = gr.w / cols, ch = gr.h / rows;
            for (size_t i = 0; i < n; ++i) {
                Rect c{gr.x + (i % cols) * cw + 2, gr.y + (i / cols) * ch + 2, cw - 4, ch - 4};
                std::vector<GraphLine> lines{{&h.cores[i], t.cpu, true, L"CPU " + std::to_wstring(i)}};
                coreGraphs_.resize(n);
                coreGraphs_[i].Draw(ui, IdMix(Id("perf.core"), i), c, lines, 100.f, Interval(), [](float v) { return FormatPercent(v, 1); }, false);
                if (ch > 40 && i < cs.coreEfficiencyClass.size() && cs.efficiencyCores)
                    ui.g.Text(cs.coreEfficiencyClass[i] == *std::max_element(cs.coreEfficiencyClass.begin(), cs.coreEfficiencyClass.end()) ? L"P" : L"E",
                              {c.x + 4, c.y + 2, 20, 14}, Font::Caption, t.textTertiary);
            }
        } else {
            std::vector<GraphLine> lines{{&h.cpu, t.cpu, true, L"Utilization"}};
            if (kernel_) lines.push_back({&h.cpuKernel, t.cpu.Mix(t.text, 0.4f), true, L"Kernel"});
            graph_.Draw(ui, Id("perf.cpu.graph"), gr, lines, 100.f, Interval(), [](float v) { return FormatPercent(v, 1); }, true, true);
        }
        Rect sr{r.x, gr.B() + 26, r.w, r.B() - gr.B() - 26};
        std::wstring caches;
        std::vector<Stat> facts{{L"Base speed", FormatFreqMHz(cs.baseMHz)},
                                {L"Sockets", std::to_wstring(cs.sockets)},
                                {L"Cores", std::to_wstring(cs.cores) + (cs.efficiencyCores ? L" (" + std::to_wstring(cs.performanceCores) + L"P + " + std::to_wstring(cs.efficiencyCores) + L"E)" : L"")},
                                {L"Logical processors", std::to_wstring(cs.logical)},
                                {L"Virtualization", cs.virtualizationFirmware ? L"Enabled" : (cs.hypervisorPresent ? L"Hypervisor present" : L"Disabled")}};
        for (auto& c : cs.caches) {
            if (c.type == L'I') continue; // Task Manager reports L1 as data+instruction combined
            uint64_t total = c.size * c.count;
            if (c.level == 1)
                for (auto& ci : cs.caches) if (ci.level == 1 && ci.type == L'I') total += ci.size * ci.count;
            facts.push_back({L"L" + std::to_wstring(c.level) + L" cache", FormatBytes(total, 1)});
        }
        facts.push_back({L"Performance limit", s.cpu.performanceLimitPct < 99.5f ? FormatPercent(s.cpu.performanceLimitPct) + L" (throttled)" : L"None"});
        std::vector<Stat> big{{L"Utilization", FormatPercent(s.cpu.total)},
                              {L"Speed", FormatFreqMHz(s.cpu.currentMHz)},
                              {L"Processes", FormatCount(s.cpu.processes)},
                              {L"Threads", FormatCount(s.cpu.threads)},
                              {L"Handles", FormatCount(s.cpu.handles)},
                              {L"Up time", FormatUptime(s.cpu.uptimeSec)}};
        if (sr.w > 760) {
            big.push_back({L"Context switches/s", FormatCount(static_cast<uint64_t>(s.cpu.contextSwitchesPerSec))});
            big.push_back({L"Interrupts/s", FormatCount(static_cast<uint64_t>(s.cpu.interruptsPerSec))});
            big.push_back({L"Queue length", std::to_wstring(static_cast<int>(s.cpu.queueLength))});
        }
        DrawStats(ui, sr, big, facts);
    }

    // ------------------------------------------------------------------ Memory
    void DrawMem(Ui& ui, Rect r, const Snapshot& s) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        auto& h = app.Hist();
        const MemSnap& m = s.mem;
        const MemStatic& ms = app.Static().mem;
        Header(ui, r, L"Memory", FormatBytes(m.installed ? m.installed : m.total));
        float statsH = 210;
        Rect gr{r.x, r.y + 20, r.w, std::max(100.f, r.h - statsH - 110)};
        GraphLabels(ui, gr, L"Memory usage", FormatBytes(m.total));
        graph_.Draw(ui, Id("perf.mem.graph"), gr, {{&h.memUsed, t.memory, true, L"In use"}}, static_cast<float>(m.total / (1024.0 * 1024.0 * 1024.0)), Interval(),
                    [](float v) { wchar_t b[32]; swprintf(b, 32, L"%.1f GB", v); return std::wstring(b); }, true, true);
        // Memory composition bar.
        Rect comp{r.x, gr.B() + 40, r.w, 34};
        g.Text(L"Memory composition", {comp.x, comp.y - 18, 200, 16}, Font::Caption, t.textSecondary);
        g.Fill(comp, t.graphBg);
        g.Stroke(comp, t.memory);
        double total = static_cast<double>(m.total ? m.total : 1);
        double inUse = static_cast<double>(m.InUse());
        double modified = static_cast<double>(m.modified), standby = static_cast<double>(m.standby);
        struct Seg { double v; Color c; const wchar_t* name; };
        Seg segs[] = {{inUse, t.memory.Alpha(0.75f), L"In use"}, {modified, t.memory.Alpha(0.45f), L"Modified"}, {standby, t.memory.Alpha(0.2f), L"Standby"},
                      {std::max(0.0, total - inUse - modified - standby), Color{0, 0, 0, 0}, L"Free"}};
        float x = comp.x;
        WidgetId cid = Id("perf.mem.comp");
        ui.Hit(cid, comp);
        std::wstring tip;
        for (auto& sg : segs) {
            float w = static_cast<float>(sg.v / total * comp.w);
            if (sg.c.a > 0) g.Fill({x, comp.y, w, comp.h}, sg.c);
            g.Line(x + w, comp.y, x + w, comp.B(), t.memory.Alpha(0.6f));
            if (ui.in.mx >= x && ui.in.mx < x + w) tip = std::wstring(sg.name) + L": " + FormatBytes(static_cast<uint64_t>(sg.v));
            x += w;
        }
        ui.Tooltip(cid, tip);
        Rect sr{r.x, comp.B() + 16, r.w, r.B() - comp.B() - 16};
        std::wstring inUseText = FormatBytes(m.InUse()) + (m.compressed ? L" (" + FormatBytes(m.compressed) + L")" : L"");
        std::vector<Stat> big{{L"In use (Compressed)", inUseText},
                              {L"Available", FormatBytes(m.available)},
                              {L"Committed", FormatBytes(m.commit) + L"/" + FormatBytes(m.commitLimit)},
                              {L"Cached", FormatBytes(m.cached)},
                              {L"Paged pool", FormatBytes(m.pagedPool)},
                              {L"Non-paged pool", FormatBytes(m.nonPagedPool)}};
        if (sr.w > 760) {
            big.push_back({L"Page faults/s", FormatCount(static_cast<uint64_t>(m.pageFaultsPerSec))});
            big.push_back({L"Hard faults/s", FormatCount(static_cast<uint64_t>(m.pagesPerSec))});
            big.push_back({L"Page file usage", FormatPercent(m.pagefileUsagePct)});
        }
        std::vector<Stat> facts{{L"Speed", ms.configuredSpeedMTs ? std::to_wstring(ms.configuredSpeedMTs) + L" MT/s" : L""},
                                {L"Slots used", ms.slotsTotal ? std::to_wstring(ms.slotsUsed) + L" of " + std::to_wstring(ms.slotsTotal) : L""},
                                {L"Form factor", ms.formFactor},
                                {L"Type", ms.type},
                                {L"Hardware reserved", FormatBytes(m.hardwareReserved)},
                                {L"Commit peak", FormatBytes(m.commitPeak)}};
        if (!app.Static().sysLoaded) facts[0].value = L"Loading…";
        DrawStats(ui, sr, big, facts);
    }

    // ------------------------------------------------------------------ Disk
    void DrawDisk(Ui& ui, Rect r, const Snapshot& s, int idx) {
        const Theme& t = ui.t;
        auto& h = app.Hist();
        const DiskSnap* d = nullptr;
        for (auto& x : s.disks) if (x.index == idx) d = &x;
        const DiskStatic* st = DiskInfo(idx);
        Header(ui, r, L"Disk " + std::to_wstring(idx) + (d && !d->letters.empty() ? L" (" + d->letters + L")" : L""), st ? st->model : L"");
        auto it = h.disks.find(idx);
        if (!d || it == h.disks.end()) return;
        float statsH = 170;
        float gh = std::max(80.f, (r.h - statsH - 70) / 2);
        Rect g1{r.x, r.y + 20, r.w, gh};
        GraphLabels(ui, g1, L"Active time", L"100%");
        graph_.Draw(ui, Id("perf.disk.active"), g1, {{&it->second.active, t.disk, true, L"Active time"}}, 100.f, Interval(),
                    [](float v) { return FormatPercent(v, 1); }, true, false);
        Rect g2{r.x, g1.B() + 34, r.w, gh};
        float mx = std::max(1024.f * 1024.f, std::max(it->second.read.Max(), it->second.write.Max()) * 1.2f);
        GraphLabels(ui, g2, L"Disk transfer rate", FormatRateBytes(mx));
        graph2_.Draw(ui, Id("perf.disk.rate"), g2, {{&it->second.read, t.disk, true, L"Read"}, {&it->second.write, t.disk.Mix(t.text, 0.45f), false, L"Write"}}, mx,
                     Interval(), [](float v) { return FormatRateBytes(v); }, true, true);
        Rect sr{r.x, g2.B() + 26, r.w, r.B() - g2.B() - 26};
        std::vector<Stat> big{{L"Active time", FormatPercent(d->activePct)},
                              {L"Average response time", FormatLatencyMs(d->latencyMs)},
                              {L"Read speed", FormatRateBytes(d->readBps)},
                              {L"Write speed", FormatRateBytes(d->writeBps)}};
        if (sr.w > 760) {
            big.push_back({L"Queue length", std::to_wstring(static_cast<int>(d->queue))});
            big.push_back({L"Read / write latency", FormatLatencyMs(d->readLatencyMs) + L" / " + FormatLatencyMs(d->writeLatencyMs)});
        }
        uint64_t formatted = 0;
        if (st) for (auto& v : st->volumes) formatted += v.total;
        std::vector<Stat> facts{{L"Capacity", st ? FormatBytes(st->size) : L""},
                                {L"Formatted", formatted ? FormatBytes(formatted) : L""},
                                {L"System disk", st ? (st->isSystem ? L"Yes" : L"No") : L""},
                                {L"Page file", st ? (st->hasPageFile ? L"Yes" : L"No") : L""},
                                {L"Type", st ? (st->ssd ? L"SSD" : st->seekPenaltyKnown ? L"HDD" : L"Unknown") + (st->busType.empty() ? L"" : L" (" + st->busType + L")") : L""}};
        DrawStats(ui, sr, big, facts);
    }

    // ------------------------------------------------------------------ Network
    void DrawNic(Ui& ui, Rect r, const Snapshot& s, uint64_t luid) {
        const Theme& t = ui.t;
        auto& h = app.Hist();
        const NicSnap* n = nullptr;
        for (auto& x : s.nics) if (x.luid == luid) n = &x;
        if (!n) return;
        const NicStatic* st = NicInfo(luid);
        Header(ui, r, n->wireless ? L"Wi-Fi" : L"Ethernet", n->description);
        auto it = h.nics.find(luid);
        if (it == h.nics.end()) return;
        float statsH = 200;
        Rect gr{r.x, r.y + 20, r.w, std::max(120.f, r.h - statsH - 46)};
        float mx = std::max(1024.f, std::max(it->second.send.Max(), it->second.recv.Max()) * 1.25f);
        GraphLabels(ui, gr, L"Throughput", FormatNetRate(mx, app.NetUnits()));
        graph_.Draw(ui, Id("perf.nic.graph"), gr, {{&it->second.recv, t.network, true, L"Receive"}, {&it->second.send, t.network.Mix(t.text, 0.45f), false, L"Send"}},
                    mx, Interval(), [this](float v) { return FormatNetRate(v, app.NetUnits()); }, true, true);
        Rect sr{r.x, gr.B() + 26, r.w, r.B() - gr.B() - 26};
        std::vector<Stat> big{{L"Send", FormatNetRate(n->sendBps, app.NetUnits())}, {L"Receive", FormatNetRate(n->recvBps, app.NetUnits())}};
        if (sr.w > 600) {
            big.push_back({L"Packets/s", FormatCount(static_cast<uint64_t>(n->sendPps + n->recvPps))});
            big.push_back({L"Errors", FormatCount(n->inErrors + n->outErrors)});
        }
        auto join = [](const std::vector<std::wstring>& v) {
            std::wstring s;
            for (auto& x : v) { if (!s.empty()) s += L", "; s += x; }
            return s;
        };
        std::vector<Stat> facts{{L"Adapter name", n->alias},
                                {L"Connection type", n->wireless ? L"802.11" : L"Ethernet"},
                                {L"Link speed", n->connected ? FormatRateBits(static_cast<double>(std::max(n->speedRx, n->speedTx))) : L"Not connected"}};
        if (st) {
            if (!st->ssid.empty()) facts.push_back({L"SSID", st->ssid});
            facts.push_back({L"DNS suffix", st->dnsSuffix});
            facts.push_back({L"IPv4 address", join(st->ipv4)});
            facts.push_back({L"IPv6 address", st->ipv6.empty() ? L"" : st->ipv6.front()});
        }
        DrawStats(ui, sr, big, facts);
    }

    // ------------------------------------------------------------------ GPU
    void DrawGpu(Ui& ui, Rect r, const Snapshot& s, uint64_t luid) {
        const Theme& t = ui.t;
        auto& h = app.Hist();
        const GpuSnap* gs = nullptr;
        size_t gi = 0;
        for (size_t i = 0; i < s.gpus.size(); ++i) if (s.gpus[i].luid == luid) { gs = &s.gpus[i]; gi = i; }
        if (!gs) return;
        const GpuStatic* st = GpuInfo(luid);
        Header(ui, r, L"GPU " + std::to_wstring(gi), gs->name);
        auto it = h.gpus.find(luid);
        if (it == h.gpus.end()) return;
        GpuHist& gh = it->second;
        float statsH = 190;
        float engH = std::max(70.f, (r.h - statsH - 40) * 0.55f);
        // Four engine graphs chosen by the user (defaults like Task Manager: 3D, Copy, Video Encode, Video Decode).
        std::vector<std::wstring> keys;
        for (auto& [k, v] : gh.engines) keys.push_back(k);
        auto pick = [&](int slot) -> std::wstring {
            std::wstring want = app.Config().GetString(("performance.gpu.engine" + std::to_string(slot)).c_str());
            if (!want.empty() && gh.engines.count(want)) return want;
            static const wchar_t* defaults[] = {L"3D", L"Copy", L"VideoEncode", L"VideoDecode"};
            for (auto& k : keys) if (k.rfind(defaults[slot], 0) == 0) return k;
            return slot < static_cast<int>(keys.size()) ? keys[slot] : L"";
        };
        float ew = (r.w - 3 * 12) / 4;
        for (int i = 0; i < 4; ++i) {
            std::wstring k = pick(i);
            Rect er{r.x + i * (ew + 12), r.y + 24, ew, engH - 24};
            std::wstring label = k.empty() ? L"" : k.substr(0, k.find(L'#'));
            WidgetId lid = IdMix(Id("perf.gpu.eng"), i);
            Rect lr{er.x, er.y - 22, er.w, 20};
            bool hot = ui.Hit(lid, lr);
            ui.g.Text(label + L" ▾", lr, Font::Small, hot ? t.text : t.textSecondary);
            if (ui.Clicked(lid)) {
                ui.Defer([this, keys, i] {
                    std::vector<MenuItem> items;
                    for (size_t j = 0; j < keys.size(); ++j) items.push_back({static_cast<UINT>(j + 1), keys[j]});
                    UINT c = app.ShowMenu(items);
                    if (c) app.Config().SetString(("performance.gpu.engine" + std::to_string(i)).c_str(), keys[c - 1]);
                });
            }
            if (!k.empty())
                engGraphs_[i].Draw(ui, IdMix(Id("perf.gpu.g"), i), er, {{&gh.engines[k], t.gpu, true, label}}, 100.f, Interval(),
                                   [](float v) { return FormatPercent(v, 1); }, false);
        }
        float memY = r.y + engH + 26;
        float mh = std::max(50.f, (r.h - statsH - 40) * 0.45f - 40);
        float half = (r.w - 12) / 2;
        Rect m1{r.x, memY, half, mh}, m2{r.x + half + 12, memY, half, mh};
        float dedTotal = static_cast<float>(gs->dedicatedTotal / (1024.0 * 1024.0 * 1024.0));
        float shTotal = static_cast<float>(gs->sharedTotal / (1024.0 * 1024.0 * 1024.0));
        auto gb = [](float v) { wchar_t b[32]; swprintf(b, 32, L"%.1f GB", v); return std::wstring(b); };
        GraphLabels(ui, m1, L"Dedicated GPU memory usage", gb(dedTotal));
        GraphLabels(ui, m2, L"Shared GPU memory usage", gb(shTotal));
        graph_.Draw(ui, Id("perf.gpu.ded"), m1, {{&gh.dedicated, t.gpu, true, L"Dedicated"}}, std::max(0.1f, dedTotal), Interval(), gb, false);
        graph2_.Draw(ui, Id("perf.gpu.sh"), m2, {{&gh.shared, t.gpu, true, L"Shared"}}, std::max(0.1f, shTotal), Interval(), gb, false);
        Rect sr{r.x, m1.B() + 20, r.w, r.B() - m1.B() - 20};
        std::vector<Stat> big{{L"Utilization", FormatPercent(gs->util)},
                              {L"Dedicated GPU memory", FormatBytes(gs->dedicatedUsed) + L"/" + FormatBytes(gs->dedicatedTotal)},
                              {L"Shared GPU memory", FormatBytes(gs->sharedUsed) + L"/" + FormatBytes(gs->sharedTotal)},
                              {L"GPU memory", FormatBytes(gs->dedicatedUsed + gs->sharedUsed) + L"/" + FormatBytes(gs->dedicatedTotal + gs->sharedTotal)},
                              {L"GPU temperature", gs->temperatureC > 0 ? TempText(app, gs->temperatureC) : L"Not reported"}};
        if (sr.w > 760) {
            big.push_back({L"Core clock", gs->coreFreqMHz > 0 ? FormatFreqMHz(gs->coreFreqMHz) : L"Not reported"});
            big.push_back({L"Memory clock", gs->memoryFreqMHz > 0 ? FormatFreqMHz(gs->memoryFreqMHz) : L"Not reported"});
            big.push_back({L"Fan", gs->fanRpm > 0 ? std::to_wstring(static_cast<int>(gs->fanRpm)) + L" RPM" : L"Not reported"});
        }
        std::vector<Stat> facts;
        if (st) {
            facts = {{L"Driver version", st->driverVersion},
                     {L"Driver date", st->driverDate},
                     {L"DirectX version", st->directX + (st->wddm.empty() ? L"" : L" (WDDM " + st->wddm + L")")},
                     {L"Physical location", st->location},
                     {L"Hardware-accelerated scheduling", st->hagsSupported < 0 ? L"Unknown" : st->hagsSupported ? (st->hagsEnabled ? L"On" : L"Off (supported)") : L"Not supported"},
                     {L"PCIe link", st->pcieCurrent.empty() ? L"" : st->pcieCurrent + (st->pcieMax.empty() || st->pcieMax == st->pcieCurrent ? L"" : L" (max " + st->pcieMax + L")")}};
        } else {
            facts = {{L"Driver", L"Loading…"}};
        }
        DrawStats(ui, sr, big, facts);
    }

    std::wstring selected_, copyText_, summaryText_;
    bool logical_ = false, kernel_ = false, summary_ = false;
    float listScroll_ = 0;
    GraphView graph_, graph2_, engGraphs_[4];
    std::vector<GraphView> coreGraphs_;
};

} // namespace

std::unique_ptr<Page> MakePerformancePage(App& app) { return std::make_unique<PerformancePage>(app); }

} // namespace nx
