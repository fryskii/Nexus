// Details page: flat process table with Task Manager's full "Select columns" set and more.
#include "ui/pages/Common.h"

#include <unordered_map>

namespace nx {

namespace {

enum ColId {
    DName, DPid, DStatus, DUser, DSession, DCpu, DCpuTime, DCycles, DWs, DPeakWs, DWsDelta, DPrivWs, DSharedWs, DCommit, DPaged, DNonPaged, DPf, DPfDelta,
    DHardFaults, DBasePri, DPriority, DHandles, DThreads, DUserObj, DGdi, DIoReads, DIoWrites, DIoOther, DIoReadBytes, DIoWriteBytes, DIoOtherBytes,
    DPath, DCmd, DArch, DElevated, DUac, DDesc, DPublisher, DEfficiency, DGpuDed, DGpuShared, DGpu, DGpuEngine, DIntegrity, DParent, DStart, DNetSend,
    DNetRecv, DCount
};

class DetailsPage final : public Page, public TableSource {
public:
    explicit DetailsPage(App& a) : Page(a), table_(Id("details.table")) {
        auto c = [](const char* k, const wchar_t* t, float w, Align al, bool vis, const wchar_t* tip = L"") {
            TableColumn col = Col(k, t, w, al, vis);
            col.tooltip = tip;
            return col;
        };
        table_.columns = {
            c("name", L"Name", 220, Align::Left, true), c("pid", L"PID", 70, Align::Right, true), c("status", L"Status", 110, Align::Left, true),
            c("user", L"User name", 120, Align::Left, true), c("session", L"Session ID", 80, Align::Right, false), c("cpu", L"CPU", 60, Align::Right, true),
            c("cputime", L"CPU time", 90, Align::Right, false), c("cycles", L"Cycle", 120, Align::Right, false),
            c("ws", L"Working set (memory)", 140, Align::Right, false), c("peakws", L"Peak working set (memory)", 160, Align::Right, false),
            c("wsdelta", L"Working set delta (memory)", 160, Align::Right, false),
            c("privws", L"Memory (private working set)", 180, Align::Right, true), c("sharedws", L"Memory (shared working set)", 180, Align::Right, false),
            c("commit", L"Commit size", 110, Align::Right, false), c("paged", L"Paged pool", 100, Align::Right, false),
            c("nppool", L"NP pool", 90, Align::Right, false), c("pf", L"Page faults", 110, Align::Right, false), c("pfdelta", L"PF delta", 80, Align::Right, false),
            c("hardfaults", L"Hard faults", 100, Align::Right, false), c("basepri", L"Base priority", 100, Align::Right, false),
            c("priority", L"Priority class", 110, Align::Left, false), c("handles", L"Handles", 80, Align::Right, false),
            c("threads", L"Threads", 70, Align::Right, false), c("userobj", L"User objects", 90, Align::Right, false),
            c("gdi", L"GDI objects", 90, Align::Right, false), c("ioreads", L"I/O reads", 100, Align::Right, false),
            c("iowrites", L"I/O writes", 100, Align::Right, false), c("ioother", L"I/O other", 100, Align::Right, false),
            c("ioreadbytes", L"I/O read bytes", 120, Align::Right, false), c("iowritebytes", L"I/O write bytes", 120, Align::Right, false),
            c("iootherbytes", L"I/O other bytes", 120, Align::Right, false), c("path", L"Image path name", 300, Align::Left, false),
            c("cmd", L"Command line", 360, Align::Left, false), c("arch", L"Architecture", 90, Align::Left, true),
            c("elevated", L"Elevated", 70, Align::Left, false), c("uac", L"UAC virtualization", 120, Align::Left, false),
            c("desc", L"Description", 240, Align::Left, true), c("publisher", L"Publisher", 180, Align::Left, false),
            c("eff", L"Power throttling", 110, Align::Left, false), c("gpuded", L"Dedicated GPU memory", 150, Align::Right, false),
            c("gpushared", L"Shared GPU memory", 140, Align::Right, false), c("gpu", L"GPU", 60, Align::Right, false),
            c("gpuengine", L"GPU engine", 120, Align::Left, false), c("integrity", L"Integrity level", 100, Align::Left, false),
            c("parent", L"Parent PID", 80, Align::Right, false), c("start", L"Start time", 150, Align::Left, false),
            c("netsend", L"Network sent", 100, Align::Right, false, L"Requires administrator (kernel network ETW)"),
            c("netrecv", L"Network received", 110, Align::Right, false, L"Requires administrator (kernel network ETW)"),
        };
        table_.heatMap = false;
        table_.sortCol = DName;
        table_.sortDesc = false;
        table_.LoadLayout(app.Config().GetString("details.layout"));
    }
    const wchar_t* Title() const override { return L"Details"; }
    wchar_t Glyph() const override { return 0xE8FD; }
    uint32_t Demand() const override {
        uint32_t d = demand::ProcessExtra | demand::AppWindows;
        if (table_.columns[DGpu].visible || table_.columns[DGpuDed].visible || table_.columns[DGpuShared].visible || table_.columns[DGpuEngine].visible)
            d |= demand::GpuProcess | demand::GpuSystem;
        return d;
    }
    bool HasSearch() const override { return true; }
    const wchar_t* SearchHint() const override { return L"Filter: name, PID, pid:4, user:SYSTEM, /regex/, mem>1GB"; }
    void OnSearch(const std::wstring& s) override {
        std::wstring err;
        filter_.Compile(s, &err);
        error_ = err;
        Rebuild();
    }
    void OnShow() override {
        // "Go to details" from the Processes page.
        int64_t focus = app.Config().GetInt("details.focusPid", -1);
        Rebuild();
        if (focus >= 0) {
            app.Config().Remove("details.focusPid");
            for (size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i]->pid == static_cast<uint32_t>(focus)) { table_.SelectRow(i, *this); break; }
        }
    }
    void OnSnapshot() override {
        auto s = app.Snap();
        if (s && highlight_) {
            for (auto& p : s->procs) if (p.isNew) newUntil_[p.Key()] = s->seq + 2;
            ghosts_.clear();
            if (snap_) for (uint32_t pid : s->terminated) if (auto* o = snap_->FindProc(pid)) ghosts_.push_back(*o);
            for (auto it = newUntil_.begin(); it != newUntil_.end();) it = it->second < s->seq ? newUntil_.erase(it) : std::next(it);
        }
        snap_ = s;
        // Rows point into the snapshot: rebuild now, or drop them until the page is shown again.
        if (app.Current() == PageId::Details) Rebuild();
        else { rows_.clear(); ghost_.clear(); }
    }
    void SaveState() override { app.Config().SetString("details.layout", table_.SaveLayout()); }
    std::wstring AccessibleSummary() const override {
        int r = table_.SelectedRow(*this);
        std::wstring s = L"Details, " + std::to_wstring(rows_.size()) + L" processes.";
        if (r >= 0) s += L" Selected " + rows_[r]->Name() + L", PID " + std::to_wstring(rows_[r]->pid) + L".";
        return s;
    }
    bool OnKey(UINT vk, bool /*ctrl*/, bool shift) override {
        const ProcSample* p = Selected();
        if (vk == VK_DELETE && p) { actions::EndTask(app, *p); return true; }
        if (vk == VK_RETURN && p) { app.OpenProcessWindow(p->pid); return true; }
        if ((vk == VK_APPS || (vk == VK_F10 && shift)) && p) { actions::ProcessContextMenu(app, *p, true); return true; }
        return table_.OnKey(vk, *this);
    }

    size_t RowCount() const override { return rows_.size(); }
    uint64_t RowKey(size_t r) const override { return rows_[r]->Key() ^ (ghost_[r] ? 0x7777 : 0); }
    int RowHighlight(size_t r) const override {
        if (ghost_[r]) return 2;
        if (newUntil_.count(rows_[r]->Key())) return 1;
        return rows_[r]->status == ProcStatus::Suspended ? 3 : 0;
    }
    ID2D1Bitmap* RowIcon(size_t r) override { return app.Icons().Get(rows_[r]->meta ? rows_[r]->meta->path : L""); }
    void CellText(size_t r, int col, std::wstring& out) const override { out = Text(*rows_[r], col, ghost_[r]); }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        const ProcSample* p = Selected();
        if (ui.Button(Id("det.end"), {bar.R() - 116, bar.y + 4, 116, 32}, L"End task", 0xE711, ButtonStyle::Standard, p != nullptr)) {
            ProcSample c = *p;
            ui.Defer([this, c] { actions::EndTask(app, c); });
        }
        if (ui.Button(Id("det.inspect"), {bar.R() - 240, bar.y + 4, 116, 32}, L"Inspect", 0xE946, ButtonStyle::Standard, p != nullptr)) {
            uint32_t pid = p->pid;
            ui.Defer([this, pid] { app.OpenProcessWindow(pid); });
        }
        if (ui.IconButton(Id("det.cols"), {bar.R() - 284, bar.y + 4, 36, 32}, 0xE712, L"Select columns / options")) ui.Defer([this] { Options(); });
        ui.g.Text(error_.empty() ? std::to_wstring(rows_.size()) + L" processes" : error_, {bar.x, bar.y, 400, 40}, Font::Small,
                  error_.empty() ? ui.t.textTertiary : ui.t.critical);
        table_.compact = true;
        if (table_.sortChanged) { table_.sortChanged = false; Rebuild(); }
        table_.Draw(ui, r, *this);
        if (table_.activated) {
            table_.activated = false;
            if (p) { uint32_t pid = p->pid; ui.Defer([this, pid] { app.OpenProcessWindow(pid); }); }
        }
        if (table_.contextMenu) {
            table_.contextMenu = false;
            if (const ProcSample* sp = Selected()) { ProcSample c = *sp; ui.Defer([this, c] { actions::ProcessContextMenu(app, c, true); }); }
        }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); }); }
    }

private:
    void Options() {
        UINT c = app.ShowMenu({{1, L"Select columns..."}, {2, L"Highlight new and exited processes", highlight_}});
        if (c == 1) ColumnMenu(app, table_);
        if (c == 2) highlight_ = !highlight_;
    }

    const ProcSample* Selected() const {
        int r = table_.SelectedRow(*this);
        return r >= 0 && !ghost_[r] ? rows_[r] : nullptr;
    }

    std::wstring Text(const ProcSample& p, int col, bool ghost) const {
        const ProcMeta* m = p.meta.get();
        auto num = [](uint64_t v) { return FormatCount(v); };
        auto kb = [](uint64_t v) { return FormatCount(v / 1024) + L" K"; };
        switch (col) {
        case DName: return p.Name();
        case DPid: return std::to_wstring(p.pid);
        case DStatus: return ghost ? L"Exited" : p.status == ProcStatus::Suspended ? L"Suspended" : p.status == ProcStatus::NotResponding ? L"Not responding" : L"Running";
        case DUser: return m ? m->userShort : L"";
        case DSession: return std::to_wstring(p.session);
        case DCpu: { wchar_t b[16]; swprintf(b, 16, L"%02.0f", std::min(99.f, p.cpu)); return p.pid == 0 ? FormatPercent(p.cpu) : std::wstring(b); }
        case DCpuTime: return FormatDuration100ns(p.cpuTime);
        case DCycles: return num(p.cycles);
        case DWs: return kb(p.workingSet);
        case DPeakWs: return kb(p.peakWorkingSet);
        case DWsDelta: return (p.workingSetDelta < 0 ? L"-" : L"") + kb(static_cast<uint64_t>(std::llabs(p.workingSetDelta)));
        case DPrivWs: return kb(p.privateWorkingSet);
        case DSharedWs: return kb(p.workingSet > p.privateWorkingSet ? p.workingSet - p.privateWorkingSet : 0);
        case DCommit: return kb(p.privateBytes);
        case DPaged: return kb(p.pagedPool);
        case DNonPaged: return kb(p.nonPagedPool);
        case DPf: return num(p.pageFaults);
        case DPfDelta: return num(p.pageFaultDelta);
        case DHardFaults: return num(p.hardFaults);
        case DBasePri: return std::to_wstring(p.basePriority);
        case DPriority: return PriorityClassName(p.priorityClass);
        case DHandles: return num(p.handles);
        case DThreads: return num(p.threads);
        case DUserObj: return num(p.userObjects);
        case DGdi: return num(p.gdiObjects);
        case DIoReads: return num(p.ioReadOps);
        case DIoWrites: return num(p.ioWriteOps);
        case DIoOther: return num(p.ioOtherOps);
        case DIoReadBytes: return num(p.ioReadBytes);
        case DIoWriteBytes: return num(p.ioWriteBytes);
        case DIoOtherBytes: return num(p.ioOtherBytes);
        case DPath: return m ? m->path : L"";
        case DCmd: return m ? m->commandLine : L"";
        case DArch: return m ? MachineName(m->machine) : L"";
        case DElevated: return m && !m->accessDenied ? (m->elevated ? L"Yes" : L"No") : L"";
        case DUac: return m && !m->accessDenied ? (m->uacVirtualization ? L"Enabled" : L"Not allowed") : L"";
        case DDesc: return m ? m->description : L"";
        case DPublisher: return m ? m->company : L"";
        case DEfficiency: return p.efficiencyMode ? L"Enabled" : L"Disabled";
        case DGpuDed: return kb(p.gpuDedicated);
        case DGpuShared: return kb(p.gpuShared);
        case DGpu: { wchar_t b[16]; swprintf(b, 16, L"%02.0f", p.gpu); return b; }
        case DGpuEngine: return p.gpuEngine != GpuEngineType::None && p.gpu > 0 ? L"GPU " + std::to_wstring(p.gpuAdapter) + L" - " + GpuEngineTypeName(p.gpuEngine) : L"";
        case DIntegrity: return m ? IntegrityName(m->integrity) : L"";
        case DParent: return std::to_wstring(p.ppid);
        case DStart: {
            if (!p.createTime) return L"";
            FILETIME ft{static_cast<DWORD>(p.createTime), static_cast<DWORD>(p.createTime >> 32)}, lt;
            FileTimeToLocalFileTime(&ft, &lt);
            return FormatFileTime(FileTimeToU64(lt));
        }
        case DNetSend: return p.netSend >= 0 ? FormatNetRate(p.netSend, app.NetUnits()) : L"—";
        case DNetRecv: return p.netRecv >= 0 ? FormatNetRate(p.netRecv, app.NetUnits()) : L"—";
        }
        return L"";
    }

    double Num(const ProcSample& p, int col) const {
        switch (col) {
        case DPid: return p.pid;
        case DSession: return p.session;
        case DCpu: return p.cpu;
        case DCpuTime: return static_cast<double>(p.cpuTime);
        case DCycles: return static_cast<double>(p.cycles);
        case DWs: return static_cast<double>(p.workingSet);
        case DPeakWs: return static_cast<double>(p.peakWorkingSet);
        case DWsDelta: return static_cast<double>(p.workingSetDelta);
        case DPrivWs: return static_cast<double>(p.privateWorkingSet);
        case DSharedWs: return static_cast<double>(p.workingSet - std::min(p.workingSet, p.privateWorkingSet));
        case DCommit: return static_cast<double>(p.privateBytes);
        case DPaged: return static_cast<double>(p.pagedPool);
        case DNonPaged: return static_cast<double>(p.nonPagedPool);
        case DPf: return p.pageFaults;
        case DPfDelta: return p.pageFaultDelta;
        case DHardFaults: return p.hardFaults;
        case DBasePri: return p.basePriority;
        case DHandles: return p.handles;
        case DThreads: return p.threads;
        case DUserObj: return p.userObjects;
        case DGdi: return p.gdiObjects;
        case DIoReads: return static_cast<double>(p.ioReadOps);
        case DIoWrites: return static_cast<double>(p.ioWriteOps);
        case DIoOther: return static_cast<double>(p.ioOtherOps);
        case DIoReadBytes: return static_cast<double>(p.ioReadBytes);
        case DIoWriteBytes: return static_cast<double>(p.ioWriteBytes);
        case DIoOtherBytes: return static_cast<double>(p.ioOtherBytes);
        case DGpuDed: return static_cast<double>(p.gpuDedicated);
        case DGpuShared: return static_cast<double>(p.gpuShared);
        case DGpu: return p.gpu;
        case DParent: return p.ppid;
        case DStart: return static_cast<double>(p.createTime);
        case DNetSend: return p.netSend;
        case DNetRecv: return p.netRecv;
        default: return std::nan("");
        }
    }

    void Rebuild() {
        rows_.clear();
        ghost_.clear();
        if (!snap_) return;
        for (auto& p : snap_->procs) {
            if (!filter_.Empty() && !filter_.Matches(MakeSubject(p))) continue;
            rows_.push_back(&p);
            ghost_.push_back(false);
        }
        if (highlight_)
            for (auto& g : ghosts_) { rows_.push_back(&g); ghost_.push_back(true); }
        int col = table_.sortCol;
        bool desc = table_.sortDesc;
        std::vector<size_t> idx(rows_.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        if (col >= 0) {
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                const ProcSample& A = *rows_[a];
                const ProcSample& B = *rows_[b];
                double x = Num(A, col), y = Num(B, col);
                int c;
                if (!std::isnan(x)) c = x < y ? -1 : x > y ? 1 : 0;
                else c = _wcsicmp(Text(A, col, false).c_str(), Text(B, col, false).c_str());
                if (c == 0) c = A.pid < B.pid ? -1 : A.pid > B.pid;
                return desc ? c > 0 : c < 0;
            });
        }
        std::vector<const ProcSample*> r2;
        std::vector<bool> g2;
        for (size_t i : idx) { r2.push_back(rows_[i]); g2.push_back(ghost_[i]); }
        rows_.swap(r2);
        ghost_.swap(g2);
    }

    Table table_;
    SnapshotPtr snap_;
    std::vector<const ProcSample*> rows_;
    std::vector<bool> ghost_;
    std::vector<ProcSample> ghosts_;
    std::unordered_map<uint64_t, uint64_t> newUntil_;
    ProcessFilter filter_;
    std::wstring error_;
    bool highlight_ = true;
};

} // namespace

std::unique_ptr<Page> MakeDetailsPage(App& app) { return std::make_unique<DetailsPage>(app); }

} // namespace nx
