// Storage: physical disks, live activity, partition layout, volumes and SMART / NVMe health.
#include "ui/pages/Common.h"

namespace nx {

namespace {

class StoragePage final : public Page {
public:
    explicit StoragePage(App& a) : Page(a), smart_(Id("storage.smart")), vols_(Id("storage.vols")) {
        smart_.columns = {Col("id", L"ID", 50, Align::Right), Col("name", L"Attribute", 260), Col("cur", L"Current", 80, Align::Right),
                          Col("worst", L"Worst", 80, Align::Right), Col("thr", L"Threshold", 90, Align::Right), Col("raw", L"Raw value", 140, Align::Right),
                          Col("status", L"Status", 100)};
        vols_.columns = {Col("vol", L"Volume", 80), Col("label", L"Label", 160), Col("fs", L"File system", 100), Col("size", L"Capacity", 110, Align::Right),
                         Col("free", L"Free", 110, Align::Right), Col("pct", L"% free", 70, Align::Right), Col("cluster", L"Cluster size", 100, Align::Right),
                         Col("disk", L"Disk", 70, Align::Right)};
        for (Table* t : {&smart_, &vols_}) { t->heatMap = false; t->compact = true; t->showIcons = false; }
        smart_.sortCol = 0;
        smart_.sortDesc = false;
        vols_.sortCol = 0;
        vols_.sortDesc = false;
    }
    const wchar_t* Title() const override { return L"Storage"; }
    wchar_t Glyph() const override { return 0xEDA2; }
    uint32_t Demand() const override { return demand::Disk; }
    void OnShow() override { app.EnsureDiskInfo(true); }
    std::wstring AccessibleSummary() const override { return L"Storage, " + std::to_wstring(app.Static().disks.size()) + L" physical disks."; }

    void Draw(Ui& ui, const Rect& area) override {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        auto& st = app.Static();
        Rect r = area;
        Rect bar = r.TakeTop(44);
        if (ui.Button(Id("storage.refresh"), {bar.R() - 120, bar.y + 4, 120, 32}, L"Refresh", 0xE72C, ButtonStyle::Standard, !st.disksLoading)) {
            st.disksLoaded = false;
            st.health.clear();
            app.EnsureDiskInfo(true);
        }
        int tab = ui.Tabs(Id("storage.tabs"), {bar.x, bar.y + 4, 500, 32}, {L"Disks", L"Volumes"}, tab_);
        if (tab >= 0) tab_ = tab;
        if (!st.disksLoaded) { g.Text(L"Reading storage devices…", r, Font::Body, t.textSecondary, Align::Center); return; }
        if (tab_ == 1) { DrawVolumes(ui, r); return; }
        if (st.disks.empty()) { g.Text(L"No physical disks could be opened.", r, Font::Body, t.textSecondary, Align::Center); return; }
        if (sel_ < 0 || sel_ >= static_cast<int>(st.disks.size())) sel_ = 0;
        // Disk selector strip.
        float x = r.x;
        for (size_t i = 0; i < st.disks.size(); ++i) {
            auto& d = st.disks[i];
            std::wstring label = L"Disk " + std::to_wstring(d.index) + (d.Letters().empty() ? L"" : L" (" + d.Letters() + L")");
            float w = g.Measure(label, Font::BodySemi) + 28;
            Rect br{x, r.y + 4, w, 32};
            WidgetId id = IdMix(Id("storage.disk"), i);
            bool hot = ui.Hit(id, br);
            g.FillRound(br, 6.f, static_cast<int>(i) == sel_ ? t.selection : hot ? t.hover : Color{0, 0, 0, 0});
            g.Text(label, br, static_cast<int>(i) == sel_ ? Font::BodySemi : Font::Body, t.text, Align::Center);
            if (ui.Clicked(id)) sel_ = static_cast<int>(i);
            x += w + 6;
        }
        r.TakeTop(46);
        const DiskStatic& d = st.disks[sel_];
        auto hi = st.health.find(d.index);
        const StorageHealth* h = hi != st.health.end() ? &hi->second : nullptr;

        float off = scroll_;
        Rect view = r;
        g.PushClip(view);
        float y = view.y - scroll_;
        float colW = (view.w - 16) / 2;
        int healthRows = 3;
        if (h && h->available)
            healthRows += (h->percentUsed >= 0) + (h->availableSpare >= 0) + (h->dataWrittenBytes != 0) + (h->powerOnHours != 0) + (h->unsafeShutdowns || h->mediaErrors);
        float cardH = std::max(210.f, 44 + healthRows * 22.f + 14);
        // --- Identity card
        Rect c1{view.x, y, colW, cardH};
        ui.Card(c1);
        float cy = c1.y + 12;
        g.Text(d.model.empty() ? L"Unknown device" : d.model, {c1.x + 16, cy, c1.w - 32, 24}, Font::Subtitle, t.text);
        cy += 32;
        auto kv = [&](Rect c, float& yy, const std::wstring& k, const std::wstring& v, Color vc = Color::None()) {
            ui.KeyValue(c.x + 16, yy, 220, c.w - 250, k, v, vc);
            yy += 22;
        };
        kv(c1, cy, L"Capacity", FormatBytes(d.size));
        kv(c1, cy, L"Media type", d.ssd ? L"Solid state (SSD)" : d.seekPenaltyKnown ? L"Hard disk (HDD)" : L"Unknown");
        kv(c1, cy, L"Interface", d.busType + (d.removable ? L" (removable)" : L""));
        kv(c1, cy, L"Serial number", d.serial);
        kv(c1, cy, L"Firmware", d.firmware);
        kv(c1, cy, L"Partition style", d.partitionStyle.empty() ? L"Requires administrator" : d.partitionStyle);
        kv(c1, cy, L"Role", std::wstring(d.isSystem ? L"System disk" : L"Data disk") + (d.hasPageFile ? L", page file" : L""));
        // --- Health card
        Rect c2{view.x + colW + 16, y, colW, cardH};
        ui.Card(c2);
        cy = c2.y + 12;
        g.Text(L"Health", {c2.x + 16, cy, 200, 24}, Font::Subtitle, t.text);
        cy += 32;
        if (!h) {
            g.Text(st.disksLoading ? L"Reading…" : L"Not available", {c2.x + 16, cy, c2.w - 32, 22}, Font::Body, t.textSecondary);
        } else if (!h->available) {
            InfoText(g, t, {c2.x + 16, cy, c2.w - 32, 120}, h->unavailableReason.empty() ? L"This device does not report health data." : h->unavailableReason);
        } else {
            Color hc = h->healthEstimate.rfind(L"Good", 0) == 0 ? t.good : h->healthEstimate.rfind(L"Bad", 0) == 0 ? t.critical : t.warning;
            kv(c2, cy, L"Assessment", h->healthEstimate, hc);
            kv(c2, cy, L"Source", h->source);
            kv(c2, cy, L"Temperature", h->temperatureC > -50 ? TempText(app, h->temperatureC) +
                                                                 (h->temperatureWarnC > 0 ? L" (warning at " + TempText(app, h->temperatureWarnC) + L")" : L"")
                                                           : L"Not reported");
            if (h->percentUsed >= 0) kv(c2, cy, L"Endurance used", std::to_wstring(h->percentUsed) + L"%");
            if (h->availableSpare >= 0) kv(c2, cy, L"Available spare", std::to_wstring(h->availableSpare) + L"% (threshold " + std::to_wstring(h->spareThreshold) + L"%)");
            if (h->dataWrittenBytes) kv(c2, cy, L"Data read / written", FormatBytes(h->dataReadBytes) + L" / " + FormatBytes(h->dataWrittenBytes));
            if (h->powerOnHours) kv(c2, cy, L"Power-on hours", FormatCount(h->powerOnHours) + (h->powerCycles ? L" (" + FormatCount(h->powerCycles) + L" power cycles)" : L""));
            if (h->unsafeShutdowns || h->mediaErrors) kv(c2, cy, L"Unsafe shutdowns / media errors", FormatCount(h->unsafeShutdowns) + L" / " + FormatCount(h->mediaErrors));
        }
        y = c1.B() + 16;
        // --- Live activity
        auto snap = app.Snap();
        const DiskSnap* ds = nullptr;
        if (snap) for (auto& x2 : snap->disks) if (x2.index == d.index) ds = &x2;
        auto& hist = app.Hist();
        auto it = hist.disks.find(d.index);
        Rect act{view.x, y, view.w, 150};
        ui.Card(act);
        g.Text(L"Activity", {act.x + 16, act.y + 10, 200, 22}, Font::BodySemi, t.text);
        if (ds) {
            std::wstring s = L"Active " + FormatPercent(ds->activePct) + L"   Read " + FormatRateBytes(ds->readBps) + L"   Write " + FormatRateBytes(ds->writeBps) +
                             L"   Response " + FormatLatencyMs(ds->latencyMs) + L"   Queue " + std::to_wstring(static_cast<int>(ds->queue));
            g.Text(s, {act.x + 120, act.y + 10, act.w - 136, 22}, Font::Small, t.textSecondary, Align::Right);
        }
        if (it != hist.disks.end() && snap) {
            float mx = std::max(1024.f * 1024.f, std::max(it->second.read.Max(), it->second.write.Max()) * 1.2f);
            float hw = (act.w - 48) / 2;
            graphA_.Draw(ui, Id("storage.g1"), {act.x + 16, act.y + 40, hw, act.h - 56}, {{&it->second.active, t.disk, true, L"Active time"}}, 100.f, snap->interval,
                         [](float v) { return FormatPercent(v, 1); }, false);
            graphB_.Draw(ui, Id("storage.g2"), {act.x + 32 + hw, act.y + 40, hw, act.h - 56},
                         {{&it->second.read, t.disk, true, L"Read"}, {&it->second.write, t.disk.Mix(t.text, 0.45f), false, L"Write"}}, mx, snap->interval,
                         [](float v) { return FormatRateBytes(v); }, false);
        }
        y = act.B() + 16;
        // --- Partition layout bar
        g.Text(L"Partition layout", {view.x, y, 300, 22}, Font::BodySemi, t.text);
        y += 26;
        Rect pb{view.x, y, view.w, 44};
        g.FillRound(pb, 4.f, t.graphBg);
        double total = static_cast<double>(d.size ? d.size : 1);
        static const Color palette[] = {Color::Hex(0x4CA8FF), Color::Hex(0x5CCB8A), Color::Hex(0xC08CFF), Color::Hex(0xF5A742), Color::Hex(0x3ED2E0), Color::Hex(0xFF7A6B)};
        int pi = 0;
        for (auto& p : d.partitions) {
            float px = pb.x + static_cast<float>(p.offset / total * pb.w);
            float pw = std::max(3.f, static_cast<float>(p.length / total * pb.w));
            Rect prc{px, pb.y, pw, pb.h};
            Color pc = palette[pi++ % 6];
            g.Fill(prc, pc.Alpha(0.35f));
            g.Stroke(prc, pc);
            std::wstring label = p.name.empty() ? p.type : p.name;
            if (pw > 60) g.Text(label + L"\n" + FormatBytes(p.length), prc.Inset(6, 2), Font::Caption, t.text);
            WidgetId pid = IdMix(Id("storage.part"), static_cast<uint64_t>(p.offset));
            ui.Hit(pid, prc);
            ui.Tooltip(pid, label + L" — " + p.type + L"\n" + FormatBytes(p.length) + L" at offset " + FormatBytes(p.offset));
        }
        y = pb.B() + 8;
        if (d.partitionStyle.empty())
            g.Text(L"Showing volumes only. The complete partition table (including hidden EFI/recovery partitions) requires administrator rights.", {view.x, y, view.w, 20},
                   Font::Caption, t.textTertiary);
        y += 26;
        // --- SMART attributes (ATA)
        if (h && !h->smart.empty()) {
            g.Text(L"SMART attributes", {view.x, y, 300, 22}, Font::BodySemi, t.text);
            y += 26;
            smartGrid_.rows.clear();
            for (auto& a : h->smart) {
                GridRow row;
                row.key = static_cast<uint64_t>(a.id);
                row.cells = {std::to_wstring(a.id), a.name, std::to_wstring(a.current), std::to_wstring(a.worst), std::to_wstring(a.threshold), FormatCount(a.raw),
                             a.failing ? L"FAILING" : L"OK"};
                row.nums = {static_cast<double>(a.id), NoNum(), static_cast<double>(a.current), static_cast<double>(a.worst), static_cast<double>(a.threshold),
                            static_cast<double>(a.raw)};
                if (a.failing) { row.color = t.critical; row.colorCol = 6; }
                smartGrid_.rows.push_back(std::move(row));
            }
            smartGrid_.Refresh(smart_);
            float th = std::min(400.f, 34.f + h->smart.size() * smart_.RowHeight());
            smart_.Draw(ui, {view.x, y, view.w, th}, smartGrid_);
            y += th + 16;
        }
        g.PopClip();
        float content = y + scroll_ - view.y;
        ui.Scroll(Id("storage.scroll"), view, content, off);
        scroll_ = off;
    }

private:
    void DrawVolumes(Ui& ui, const Rect& r) {
        volGrid_.rows.clear();
        for (auto& d : app.Static().disks) {
            for (auto& v : d.volumes) {
                GridRow row;
                row.key = std::hash<std::wstring>{}(v.guidPath);
                double pct = v.total ? 100.0 * v.free / v.total : 0;
                row.cells = {v.letter.empty() ? L"(no letter)" : v.letter, v.label, v.fileSystem, FormatBytes(v.total), FormatBytes(v.free), FormatPercent(pct),
                             v.clusterSize ? FormatBytes(v.clusterSize, 0) : L"", std::to_wstring(d.index)};
                row.nums = {NoNum(), NoNum(), NoNum(), static_cast<double>(v.total), static_cast<double>(v.free), pct, static_cast<double>(v.clusterSize),
                            static_cast<double>(d.index)};
                if (v.total && pct < 10) { row.color = ui.t.warning; row.colorCol = 5; }
                volGrid_.rows.push_back(std::move(row));
            }
        }
        if (vols_.sortChanged) vols_.sortChanged = false;
        volGrid_.Refresh(vols_);
        vols_.Draw(ui, r, volGrid_);
    }

    int tab_ = 0, sel_ = 0;
    float scroll_ = 0;
    Table smart_, vols_;
    SimpleGrid smartGrid_, volGrid_;
    GraphView graphA_, graphB_;
};

} // namespace

std::unique_ptr<Page> MakeStoragePage(App& app) { return std::make_unique<StoragePage>(app); }

} // namespace nx
