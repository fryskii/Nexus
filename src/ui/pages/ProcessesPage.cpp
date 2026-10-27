// Processes page: Task Manager style grouped view (Apps / Background / Windows processes) with
// app process groups, plus hierarchical tree and flat views.
#include "ui/pages/Common.h"

#include <unordered_map>
#include <unordered_set>

namespace nx {

namespace {

enum ColId { CName, CStatus, CPid, CType, CPublisher, CProcName, CCmd, CCpu, CMem, CDisk, CNet, CGpu, CGpuEngine, CPower, CPowerTrend, CThreads,
             CHandles, CUser, CArch, CCount };

struct Row {
    int kind = 1; // 0 = category header, 1 = process / group node
    uint64_t key = 0;
    std::wstring name;
    const ProcSample* proc = nullptr;            // representative (root) process
    std::vector<const ProcSample*> members;      // all processes aggregated in this row
    float cpu = 0, gpu = 0, io = 0, net = -1, power = 0, powerTrend = 0;
    uint64_t mem = 0;
    uint32_t threads = 0, handles = 0;
    int depth = 0, expander = 0, highlight = 0;
    bool suspended = false, notResponding = false, efficiency = false;
    std::wstring iconPath;
};

class ProcessesPage final : public Page, public TableSource {
public:
    explicit ProcessesPage(App& a) : Page(a), table_(Id("proc.table")) {
        table_.columns = {
            Col("name", L"Name", 280),
            Col("status", L"Status", 110),
            Col("pid", L"PID", 70, Align::Right, false),
            Col("type", L"Type", 110, Align::Left, false),
            Col("publisher", L"Publisher", 160, Align::Left, false),
            Col("procname", L"Process name", 150, Align::Left, false),
            Col("cmd", L"Command line", 300, Align::Left, false),
            HeatCol("cpu", L"CPU", 76, L"Share of total processor time (all logical processors)."),
            HeatCol("mem", L"Memory", 96, L"Private working set: physical memory used by the process that cannot be shared."),
            HeatCol("disk", L"Disk I/O", 92, L"File and device I/O rate (read + write) from kernel process counters. Includes cached file I/O, so it can exceed physical disk throughput."),
            HeatCol("net", L"Network", 92, L"TCP/UDP payload throughput per process from kernel network ETW events (administrator required)."),
            HeatCol("gpu", L"GPU", 70, L"Busiest GPU engine used by the process (same method as Task Manager)."),
            Col("gpuengine", L"GPU engine", 110, Align::Left, false),
            HeatCol("power", L"Power usage", 104, L"NEXUS estimate from CPU, GPU, I/O and network activity. Not a measured wattage."),
            HeatCol("powertrend", L"Power usage trend", 130, L"Two-minute average of the power estimate."),
            Col("threads", L"Threads", 70, Align::Right, false),
            Col("handles", L"Handles", 76, Align::Right, false),
            Col("user", L"User name", 120, Align::Left, false),
            Col("arch", L"Architecture", 90, Align::Left, false),
        };
        table_.sortCol = -1;
        table_.LoadLayout(app.Config().GetString("processes.layout"));
        std::wstring v = app.Config().GetString("processes.view", L"grouped");
        view_ = v == L"tree" ? 1 : v == L"flat" ? 2 : 0;
        highlight_ = app.Config().GetBool("processes.highlight", true);
    }

    const wchar_t* Title() const override { return L"Processes"; }
    wchar_t Glyph() const override { return 0xE71D; }
    uint32_t Demand() const override {
        uint32_t d = demand::AppWindows | demand::ProcessExtra | demand::Services | demand::Disk;
        if (table_.columns[CGpu].visible || table_.columns[CGpuEngine].visible) d |= demand::GpuProcess | demand::GpuSystem;
        return d;
    }
    bool HasSearch() const override { return true; }
    const wchar_t* SearchHint() const override { return L"Search: name, PID, publisher, /regex/, cpu>5, mem>200MB"; }
    void OnSearch(const std::wstring& text) override {
        std::wstring err;
        filter_.Compile(text, &err);
        filterError_ = err;
        Rebuild();
    }
    void OnSnapshot() override {
        auto s = app.Snap();
        if (!s) return;
        // Track recently started / exited processes for highlighting.
        if (highlight_) {
            for (auto& p : s->procs)
                if (p.isNew) newUntil_[p.Key()] = s->seq + 2;
            ghosts_.clear();
            if (snap_) {
                for (uint32_t pid : s->terminated) {
                    const ProcSample* old = snap_->FindProc(pid);
                    if (old && old->pid) ghosts_.push_back(*old);
                }
            }
            for (auto it = newUntil_.begin(); it != newUntil_.end();) it = it->second < s->seq ? newUntil_.erase(it) : std::next(it);
        }
        snap_ = s;
        if (app.Current() == PageId::Processes) Rebuild();
    }
    void OnShow() override { Rebuild(); }
    void SaveState() override {
        app.Config().SetString("processes.layout", table_.SaveLayout());
        app.Config().SetString("processes.view", view_ == 1 ? L"tree" : view_ == 2 ? L"flat" : L"grouped");
        app.Config().SetBool("processes.highlight", highlight_);
    }
    std::wstring AccessibleSummary() const override {
        std::wstring s = L"Processes, " + std::to_wstring(snap_ ? snap_->cpu.processes : 0) + L" processes.";
        int r = table_.SelectedRow(*this);
        if (r >= 0) {
            const Row& row = rows_[r];
            s += L" Selected: " + row.name + L", CPU " + FormatPercent(row.cpu, 1) + L", memory " + FormatMB(row.mem) + L".";
        }
        return s;
    }

    bool OnKey(UINT vk, bool ctrl, bool shift) override {
        if (vk == VK_DELETE) { EndSelected(); return true; }
        if (vk == VK_APPS || (vk == VK_F10 && shift)) { ContextMenu(); return true; }
        if (vk == VK_RETURN) {
            if (const ProcSample* p = SelectedProc()) app.OpenProcessWindow(p->pid);
            return true;
        }
        return table_.OnKey(vk, *this);
    }

    // ---- TableSource ----
    size_t RowCount() const override { return rows_.size(); }
    uint64_t RowKey(size_t r) const override { return rows_[r].key; }
    bool RowIsGroup(size_t r) const override { return rows_[r].kind == 0; }
    int RowDepth(size_t r) const override { return rows_[r].depth; }
    int RowExpander(size_t r) const override { return rows_[r].expander; }
    int RowHighlight(size_t r) const override {
        const Row& row = rows_[r];
        if (row.highlight) return row.highlight;
        return row.suspended || row.efficiency ? 3 : 0;
    }
    void ToggleExpand(size_t r) override {
        uint64_t k = rows_[r].key;
        // Category headers and tree nodes are expanded by default (collapsed set); app groups are
        // collapsed by default (expanded set), matching Task Manager.
        auto& set = (view_ == 1 || rows_[r].kind == 0) ? collapsedTree_ : expanded_;
        if (set.count(k)) set.erase(k);
        else set.insert(k);
        Rebuild();
        app.Invalidate();
    }
    ID2D1Bitmap* RowIcon(size_t r) override { return rows_[r].kind ? app.Icons().Get(rows_[r].iconPath) : nullptr; }
    Color CellColor(size_t r, int col, const Theme& t) const override {
        if (col == CStatus) {
            if (rows_[r].notResponding) return t.critical;
            if (rows_[r].efficiency) return t.good;
        }
        return {};
    }
    float CellHeat(size_t r, int col) const override {
        const Row& row = rows_[r];
        if (!snap_ || row.kind == 0) return -1;
        switch (col) {
        case CCpu: return std::min(1.f, row.cpu / 60.f + (row.cpu > 0.5f ? 0.08f : 0.f));
        case CMem: return snap_->mem.total ? std::min(1.f, static_cast<float>(row.mem) / snap_->mem.total * 6.f + (row.mem > (64ull << 20) ? 0.05f : 0.f)) : 0.f;
        case CDisk: return std::min(1.f, row.io / (20.f * 1024 * 1024) + (row.io > 1024 * 64 ? 0.06f : 0.f));
        case CNet: return row.net > 0 ? std::min(1.f, row.net / (5.f * 1024 * 1024) + 0.06f) : 0.f;
        case CGpu: return std::min(1.f, row.gpu / 60.f + (row.gpu > 0.5f ? 0.08f : 0.f));
        case CPower: return PowerHeat(row.power);
        case CPowerTrend: return PowerHeat(row.powerTrend);
        default: return -1;
        }
    }
    void CellText(size_t r, int col, std::wstring& out) const override {
        const Row& row = rows_[r];
        out.clear();
        if (row.kind == 0) { if (col == 0) out = row.name; return; }
        const ProcSample* p = row.proc;
        bool single = row.members.size() == 1;
        switch (col) {
        case CName: out = row.name; break;
        case CStatus:
            if (row.notResponding) out = L"Not responding";
            else if (row.suspended) out = L"Suspended";
            else if (row.efficiency) out = L"Efficiency mode";
            else if (row.highlight == 2) out = L"Exited";
            break;
        case CPid: if (p && (single || row.depth > 0 || view_ != 0)) out = std::to_wstring(p->pid); break;
        case CType: if (p) out = p->category == ProcCategory::App ? L"App" : p->category == ProcCategory::Windows ? L"Windows process" : L"Background process"; break;
        case CPublisher: if (p && p->meta) out = p->meta->company; break;
        case CProcName: if (p && p->meta) out = p->meta->name; break;
        case CCmd: if (p && p->meta && single) out = p->meta->commandLine; break;
        case CCpu: out = FormatPercent(row.cpu, row.cpu < 10 && row.cpu > 0 ? 1 : 0); break;
        case CMem: out = FormatMB(row.mem); break;
        case CDisk: out = FormatRateBytes(row.io); break;
        case CNet: out = row.net >= 0 ? FormatNetRate(row.net, app.NetUnits()) : L"—"; break;
        case CGpu: out = FormatPercent(row.gpu, row.gpu < 10 && row.gpu > 0 ? 1 : 0); break;
        case CGpuEngine:
            if (p && p->gpuEngine != GpuEngineType::None && row.gpu > 0) out = L"GPU " + std::to_wstring(p->gpuAdapter) + L" - " + GpuEngineTypeName(p->gpuEngine);
            break;
        case CPower: out = PowerBand(row.power); break;
        case CPowerTrend: out = PowerBand(row.powerTrend); break;
        case CThreads: out = std::to_wstring(row.threads); break;
        case CHandles: out = FormatCount(row.handles); break;
        case CUser: if (p && p->meta) out = p->meta->userShort; break;
        case CArch: if (p && p->meta) out = MachineName(p->meta->machine); break;
        }
    }

    // ---- Drawing ----
    void Draw(Ui& ui, const Rect& area) override {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        Rect r = area;
        Rect bar = r.TakeTop(44);
        int sel = ui.Tabs(Id("proc.view"), {bar.x, bar.y + 4, 300, 32}, {L"Grouped", L"Tree", L"Flat"}, view_);
        if (sel >= 0 && sel != view_) { view_ = sel; Rebuild(); }
        float bx = bar.R();
        const ProcSample* sp = SelectedProc();
        const Row* srow = SelectedRowPtr();
        bool canEnd = srow && srow->kind == 1;
        bx -= 116;
        if (ui.Button(Id("proc.end"), {bx, bar.y + 4, 116, 32}, L"End task", 0xE711, ButtonStyle::Standard, canEnd)) ui.Defer([this] { EndSelected(); });
        bx -= 158;
        bool effOn = sp && sp->efficiencyMode;
        bool effEnabled = sp && srow && srow->members.size() >= 1 && sp->category != ProcCategory::Windows;
        if (ui.Button(Id("proc.eff"), {bx, bar.y + 4, 150, 32}, effOn ? L"Efficiency: on" : L"Efficiency mode", 0, ButtonStyle::Standard, effEnabled)) {
            ProcSample c = *sp;
            ui.Defer([this, c] { actions::ToggleEfficiency(app, c); });
        }
        bx -= 44;
        if (ui.IconButton(Id("proc.more"), {bx, bar.y + 4, 36, 32}, 0xE712, L"View options")) ui.Defer([this] { ViewMenu(); });
        std::wstring info;
        if (!filterError_.empty()) info = filterError_;
        else if (snap_) info = std::to_wstring(visibleProcs_) + L" of " + std::to_wstring(snap_->cpu.processes) + L" processes";
        g.Text(info, {bar.x + 310, bar.y, bx - bar.x - 320, 40}, Font::Small, filterError_.empty() ? t.textTertiary : t.critical);

        // Header totals like Task Manager.
        if (snap_) {
            auto& s = *snap_;
            table_.columns[CCpu].header2 = FormatPercent(s.cpu.total);
            table_.columns[CMem].header2 = FormatPercent(s.mem.total ? 100.0 * s.mem.InUse() / s.mem.total : 0);
            float dmax = 0;
            for (auto& d : s.disks) dmax = std::max(dmax, d.activePct);
            table_.columns[CDisk].header2 = s.diskCollected ? FormatPercent(dmax) : L"";
            double rate = 0, cap = 0;
            for (auto& n : s.nics) { rate += (n.sendBps + n.recvBps) * 8; if (n.connected) cap += std::max(n.speedRx, n.speedTx); }
            table_.columns[CNet].header2 = cap > 0 ? FormatPercent(100.0 * rate / cap) : L"0%";
            float gmax = 0;
            for (auto& gp : s.gpus) gmax = std::max(gmax, gp.util);
            table_.columns[CGpu].header2 = s.gpuCollected ? FormatPercent(gmax) : L"";
        }
        table_.compact = app.Compact() || table_.compact;
        if (table_.sortChanged) { table_.sortChanged = false; Rebuild(); }
        table_.Draw(ui, r, *this);
        if (table_.activated) {
            table_.activated = false;
            if (const ProcSample* p = SelectedProc()) {
                uint32_t pid = p->pid;
                ui.Defer([this, pid] { app.OpenProcessWindow(pid); });
            }
        }
        if (table_.contextMenu) { table_.contextMenu = false; ui.Defer([this] { ContextMenu(); }); }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); Rebuild(); }); }
        if (snap_ && !snap_->netPerProcess && table_.columns[CNet].visible) table_.columns[CNet].tooltip = app.Collector().NetTraceStatus();
    }

private:
    const Row* SelectedRowPtr() const {
        int r = table_.SelectedRow(*this);
        return r >= 0 ? &rows_[r] : nullptr;
    }
    const ProcSample* SelectedProc() const {
        const Row* r = SelectedRowPtr();
        return r && r->kind == 1 && r->highlight != 2 ? r->proc : nullptr;
    }

    void ViewMenu() {
        UINT c = app.ShowMenu({{1, L"Highlight new and exited processes", highlight_}, {2, L"Compact rows", table_.compact}, {3, L"Heat-map shading", table_.heatMap},
                               MenuItem::Sep(), {4, L"Choose columns..."}, {5, L"Expand all"}, {6, L"Collapse all"}});
        if (c == 1) highlight_ = !highlight_;
        if (c == 2) table_.compact = !table_.compact;
        if (c == 3) table_.heatMap = !table_.heatMap;
        if (c == 4) ColumnMenu(app, table_);
        if (c == 5) { for (auto& r : rows_) if (r.expander) expanded_.insert(r.key); collapsedTree_.clear(); }
        if (c == 6) { expanded_.clear(); for (auto& r : rows_) if (r.expander) collapsedTree_.insert(r.key); }
        Rebuild();
    }

    void EndSelected() {
        const Row* r = SelectedRowPtr();
        if (!r || r->kind != 1 || r->highlight == 2) return;
        if (r->members.size() == 1 || view_ == 2 || (view_ == 1 && r->expander == 0)) { actions::EndTask(app, *r->proc); return; }
        if (view_ == 1) { actions::EndTree(app, *r->proc); return; }
        // Group: an app group ends its root's tree; an image group ends every instance.
        if (r->proc && r->proc->category == ProcCategory::App) { actions::EndTree(app, *r->proc); return; }
        if (!app.Confirm(L"End all " + std::to_wstring(r->members.size()) + L" processes of " + r->name + L"?", L"Unsaved data in these processes will be lost.",
                         L"End processes", true))
            return;
        int failed = 0;
        for (auto* p : r->members) {
            std::wstring why;
            if (IsCriticalSystemProcess(*p) || TerminateProcessId(p->pid, p->createTime, why)) ++failed;
        }
        app.ShowStatus(failed ? std::to_wstring(failed) + L" process(es) could not be ended" : L"Ended " + r->name, failed != 0);
        app.Collector().RefreshNow();
    }

    void ContextMenu() {
        const Row* r = SelectedRowPtr();
        if (!r) return;
        if (r->kind == 0) {
            if (app.ShowMenu({{1, L"Expand / collapse"}}) == 1) {
                int idx = table_.SelectedRow(*this);
                if (idx >= 0) ToggleExpand(idx);
            }
            return;
        }
        if (r->highlight == 2) return;
        if (r->members.size() == 1 || view_ != 0) { actions::ProcessContextMenu(app, *r->proc, false); return; }
        ProcSample root = *r->proc;
        bool expanded = expanded_.count(r->key) != 0;
        UINT c = app.ShowMenu({{1, expanded ? L"Collapse" : L"Expand"}, {2, L"End task\tDel"}, MenuItem::Sep(), {3, L"Open file location"}, {4, L"Search online"},
                               {5, L"Properties"}, {6, L"Inspect root process..."}});
        if (c == 1) { int idx = table_.SelectedRow(*this); if (idx >= 0) ToggleExpand(idx); }
        if (c == 2) EndSelected();
        if (c == 3) actions::OpenLocation(root);
        if (c == 4) SearchOnline(root.Name());
        if (c == 5) actions::Properties(app, root);
        if (c == 6) app.OpenProcessWindow(root.pid);
    }

    std::wstring ServiceLabel(const ProcSample& p) const {
        if (!snap_ || !snap_->servicesByPid) return {};
        auto it = snap_->servicesByPid->find(p.pid);
        return it == snap_->servicesByPid->end() ? std::wstring() : it->second;
    }

    std::wstring DisplayName(const ProcSample& p) const {
        if (!p.meta) return L"";
        if (p.pid == 0) return L"System Idle Process";
        if (_wcsicmp(p.meta->name.c_str(), L"svchost.exe") == 0) {
            std::wstring svc = ServiceLabel(p);
            if (!svc.empty()) return L"Service Host: " + svc;
        }
        if (p.category == ProcCategory::App && p.windowTitle && !p.windowTitle->empty() && p.meta->description.empty()) return *p.windowTitle;
        return p.meta->DisplayName();
    }

    bool Matches(const ProcSample& p) const {
        if (filter_.Empty()) return true;
        std::wstring svc = ServiceLabel(p);
        return filter_.Matches(MakeSubject(p, svc.empty() ? nullptr : &svc));
    }

    void Accumulate(Row& row, const ProcSample& p) const {
        row.cpu += p.cpu;
        row.gpu = std::max(row.gpu, p.gpu);
        row.io += p.ioReadRate + p.ioWriteRate;
        if (p.netSend >= 0) row.net = std::max(0.f, row.net) + p.netSend + p.netRecv;
        row.mem += p.privateWorkingSet ? p.privateWorkingSet : p.privateBytes;
        row.power += p.power;
        row.powerTrend += p.powerTrend;
        row.threads += p.threads;
        row.handles += p.handles;
        row.suspended = row.members.size() == 1 ? p.status == ProcStatus::Suspended : row.suspended && p.status == ProcStatus::Suspended;
        row.notResponding |= p.status == ProcStatus::NotResponding;
        row.efficiency |= p.efficiencyMode;
    }

    Row MakeNode(const ProcSample& p, int depth) const {
        Row r;
        r.key = p.Key();
        r.proc = &p;
        r.members = {&p};
        r.name = DisplayName(p);
        r.depth = depth;
        r.iconPath = p.meta ? p.meta->path : L"";
        Accumulate(r, p);
        r.suspended = p.status == ProcStatus::Suspended;
        if (highlight_ && newUntil_.count(p.Key())) r.highlight = 1;
        return r;
    }

    double SortValue(const Row& r, int col) const {
        switch (col) {
        case CPid: return r.proc ? r.proc->pid : 0;
        case CCpu: return r.cpu;
        case CMem: return static_cast<double>(r.mem);
        case CDisk: return r.io;
        case CNet: return r.net;
        case CGpu: return r.gpu;
        case CPower: return r.power;
        case CPowerTrend: return r.powerTrend;
        case CThreads: return r.threads;
        case CHandles: return r.handles;
        default: return std::nan("");
        }
    }

    void SortRows(std::vector<Row>& v) const {
        int col = table_.sortCol;
        bool desc = table_.sortDesc;
        if (col < 0) {
            std::stable_sort(v.begin(), v.end(), [](const Row& a, const Row& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
            return;
        }
        std::wstring ta, tb;
        std::stable_sort(v.begin(), v.end(), [&](const Row& a, const Row& b) {
            double x = SortValue(a, col), y = SortValue(b, col);
            int c;
            if (!std::isnan(x)) c = x < y ? -1 : x > y ? 1 : 0;
            else {
                // Text columns: compare formatted cell text.
                c = _wcsicmp(TextFor(a, col).c_str(), TextFor(b, col).c_str());
            }
            if (c == 0) c = _wcsicmp(a.name.c_str(), b.name.c_str()) * (desc ? -1 : 1);
            return desc ? c > 0 : c < 0;
        });
    }

    std::wstring TextFor(const Row& r, int col) const {
        const ProcSample* p = r.proc;
        switch (col) {
        case CName: return r.name;
        case CPublisher: return p && p->meta ? p->meta->company : L"";
        case CProcName: return p && p->meta ? p->meta->name : L"";
        case CCmd: return p && p->meta ? p->meta->commandLine : L"";
        case CUser: return p && p->meta ? p->meta->userShort : L"";
        case CStatus: return r.notResponding ? L"Not responding" : r.suspended ? L"Suspended" : r.efficiency ? L"Efficiency mode" : L"";
        case CArch: return p && p->meta ? MachineName(p->meta->machine) : L"";
        case CType: return p ? std::to_wstring(static_cast<int>(p->category)) : L"";
        case CGpuEngine: return p ? GpuEngineTypeName(p->gpuEngine) : L"";
        default: return L"";
        }
    }

    void EmitWithChildren(std::vector<Row>& out, Row node, const std::vector<const ProcSample*>& children, int depth) {
        bool exp = expanded_.count(node.key) != 0;
        node.expander = children.empty() ? 0 : (exp ? 2 : 1);
        out.push_back(node);
        if (!exp) return;
        std::vector<Row> kids;
        for (auto* c : children) kids.push_back(MakeNode(*c, depth + 1));
        SortRows(kids);
        for (auto& k : kids) out.push_back(std::move(k));
    }

    void Rebuild() {
        rows_.clear();
        visibleProcs_ = 0;
        if (!snap_) return;
        const Snapshot& s = *snap_;
        // Parent/child map validated by creation time (protects against PID reuse).
        std::unordered_map<uint32_t, std::vector<const ProcSample*>> children;
        for (auto& p : s.procs) {
            if (p.pid == 0 || p.pid == p.ppid) continue;
            const ProcSample* parent = s.FindProc(p.ppid);
            if (parent && parent->createTime <= p.createTime && parent->pid != 0) children[p.ppid].push_back(&p);
        }

        if (view_ == 2) { // Flat
            std::vector<Row> v;
            for (auto& p : s.procs) if (Matches(p)) v.push_back(MakeNode(p, 0));
            for (auto& gst : ghosts_) { Row r = MakeNode(gst, 0); r.highlight = 2; r.key ^= 0x7777; v.push_back(r); }
            SortRows(v);
            visibleProcs_ = v.size();
            rows_ = std::move(v);
            return;
        }

        if (view_ == 1) { // Tree
            std::vector<const ProcSample*> roots;
            for (auto& p : s.procs) {
                const ProcSample* parent = s.FindProc(p.ppid);
                bool hasParent = parent && p.pid != p.ppid && parent->createTime <= p.createTime && p.pid != 0 && parent->pid != 0;
                if (!hasParent) roots.push_back(&p);
            }
            std::unordered_set<uint32_t> matched;
            if (!filter_.Empty()) {
                // Keep ancestors of matching processes so the hierarchy remains visible.
                for (auto& p : s.procs) {
                    if (!Matches(p)) continue;
                    const ProcSample* cur = &p;
                    for (int d = 0; cur && d < 64; ++d) {
                        if (!matched.insert(cur->pid).second) break;
                        const ProcSample* parent = s.FindProc(cur->ppid);
                        cur = parent && parent->createTime <= cur->createTime && parent->pid != cur->pid ? parent : nullptr;
                    }
                }
            }
            std::function<void(const ProcSample*, int)> walk = [&](const ProcSample* p, int depth) {
                if (depth > 64) return;
                if (!filter_.Empty() && !matched.count(p->pid)) return;
                Row r = MakeNode(*p, depth);
                auto it = children.find(p->pid);
                bool hasKids = it != children.end() && !it->second.empty();
                bool collapsed = collapsedTree_.count(r.key) != 0;
                r.expander = hasKids ? (collapsed ? 1 : 2) : 0;
                rows_.push_back(r);
                ++visibleProcs_;
                if (!hasKids || collapsed) return;
                std::vector<Row> tmp;
                for (auto* c : it->second) tmp.push_back(MakeNode(*c, 0));
                SortRows(tmp);
                for (auto& k : tmp) walk(k.proc, depth + 1);
            };
            std::vector<Row> rootRows;
            for (auto* p : roots) rootRows.push_back(MakeNode(*p, 0));
            SortRows(rootRows);
            for (auto& rr : rootRows) walk(rr.proc, 0);
            return;
        }

        // Grouped (Task Manager default).
        std::unordered_set<uint32_t> assigned;
        std::vector<Row> apps, background, windows;
        std::vector<std::vector<const ProcSample*>> appMembers;
        for (auto& p : s.procs) {
            if (p.category != ProcCategory::App || assigned.count(p.pid)) continue;
            Row r;
            r.kind = 1;
            r.key = p.Key() ^ 0xA5A5000000000000ULL;
            r.proc = &p;
            r.name = DisplayName(p);
            r.iconPath = p.meta ? p.meta->path : L"";
            // Members: the app process plus non-app descendants (e.g. browser renderers).
            std::vector<const ProcSample*> stack{&p};
            while (!stack.empty()) {
                const ProcSample* c = stack.back();
                stack.pop_back();
                if (assigned.count(c->pid)) continue;
                if (c != &p && c->category == ProcCategory::App) continue;
                assigned.insert(c->pid);
                r.members.push_back(c);
                auto it = children.find(c->pid);
                if (it != children.end()) for (auto* k : it->second) stack.push_back(k);
            }
            apps.push_back(std::move(r));
        }
        // Background/Windows: group instances that share the same executable (except service hosts).
        std::unordered_map<std::wstring, size_t> byPath;
        for (auto& p : s.procs) {
            if (assigned.count(p.pid) || p.pid == 0) continue;
            auto& list = p.category == ProcCategory::Windows ? windows : background;
            bool groupable = p.meta && !p.meta->path.empty() && _wcsicmp(p.meta->name.c_str(), L"svchost.exe") != 0;
            std::wstring key = groupable ? (p.category == ProcCategory::Windows ? L"w" : L"b") + ToLower(p.meta->path) : L"";
            auto it = groupable ? byPath.find(key) : byPath.end();
            if (it != byPath.end()) {
                list[it->second].members.push_back(&p);
                continue;
            }
            Row r;
            r.key = groupable ? (std::hash<std::wstring>{}(key) | 0x8000000000000000ULL) : p.Key();
            r.proc = &p;
            r.members = {&p};
            r.name = DisplayName(p);
            r.iconPath = p.meta ? p.meta->path : L"";
            if (groupable) byPath[key] = list.size();
            list.push_back(std::move(r));
        }
        auto finish = [&](std::vector<Row>& list) {
            std::vector<Row> out;
            for (auto& r : list) {
                bool any = false;
                for (auto* m : r.members) any |= Matches(*m);
                if (!any) continue;
                Row agg = r;
                agg.cpu = agg.gpu = agg.io = agg.power = agg.powerTrend = 0;
                agg.net = -1;
                agg.mem = 0;
                agg.threads = agg.handles = 0;
                std::vector<const ProcSample*> mem = agg.members;
                agg.members.clear();
                for (auto* m : mem) { agg.members.push_back(m); Accumulate(agg, *m); }
                if (agg.members.size() > 1) agg.name += L" (" + std::to_wstring(agg.members.size()) + L")";
                if (highlight_) for (auto* m : agg.members) if (newUntil_.count(m->Key())) agg.highlight = 1;
                visibleProcs_ += agg.members.size();
                out.push_back(std::move(agg));
            }
            SortRows(out);
            return out;
        };
        auto emitCategory = [&](const wchar_t* title, std::vector<Row> list, uint64_t key) {
            if (list.empty()) return;
            Row h;
            h.kind = 0;
            h.key = key;
            h.name = std::wstring(title) + L" (" + std::to_wstring(list.size()) + L")";
            rows_.push_back(h);
            if (collapsedTree_.count(key)) return;
            for (auto& r : list) {
                if (r.members.size() > 1) {
                    std::vector<const ProcSample*> kids(r.members.begin(), r.members.end());
                    EmitWithChildren(rows_, r, kids, 0);
                } else {
                    rows_.push_back(r);
                }
            }
        };
        emitCategory(L"Apps", finish(apps), 1);
        emitCategory(L"Background processes", finish(background), 2);
        emitCategory(L"Windows processes", finish(windows), 3);
        if (!ghosts_.empty()) {
            std::vector<Row> g;
            for (auto& gst : ghosts_) { Row r = MakeNode(gst, 0); r.highlight = 2; r.key ^= 0x7777; g.push_back(r); }
            Row h;
            h.kind = 0;
            h.key = 4;
            h.name = L"Recently exited (" + std::to_wstring(g.size()) + L")";
            rows_.push_back(h);
            for (auto& r : g) rows_.push_back(r);
        }
    }

    Table table_;
    std::vector<Row> rows_;
    SnapshotPtr snap_;
    std::vector<ProcSample> ghosts_;
    std::unordered_map<uint64_t, uint64_t> newUntil_;
    std::unordered_set<uint64_t> expanded_, collapsedTree_;
    ProcessFilter filter_;
    std::wstring filterError_;
    int view_ = 0;
    bool highlight_ = true;
    size_t visibleProcs_ = 0;
};

} // namespace

std::unique_ptr<Page> MakeProcessesPage(App& app) { return std::make_unique<ProcessesPage>(app); }

} // namespace nx
