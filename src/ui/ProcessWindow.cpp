#include "ui/ProcessWindow.h"
#include "app/App.h"
#include "data/Connections.h"
#include "data/ProcessOps.h"
#include "data/Processes.h"
#include "data/Worker.h"
#include "ui/FramePacer.h"
#include "ui/Graph.h"
#include "ui/Grid.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <memory>

namespace nx::ProcessWindow {

namespace {

enum Tab { General, Performance, Threads, Modules, Memory, Handles, Environment, Security, Network, TabCount };
const wchar_t* kTabNames[] = {L"General", L"Performance", L"Threads", L"Modules", L"Memory", L"Handles", L"Environment", L"Security", L"Network"};

class Window;
std::map<HWND, Window*> g_windows;
std::vector<std::unique_ptr<Window>> g_owned;

LRESULT CALLBACK WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l);

class Window {
public:
    Window(App& app, const ProcSample& p) : app_(app), pid_(p.pid), createTime_(p.createTime), key_(p.Key()), meta_(p.meta) {
        for (int i = 0; i < TabCount; ++i) tables_[i] = std::make_unique<Table>(IdMix(Id("pw.table"), i));
        name_ = p.Name();
    }
    ~Window() {
        ui_.reset();
        gfx_.Shutdown();
    }
    uint32_t Pid() const { return pid_; }
    uint64_t CreateTime() const { return createTime_; }
    HWND Hwnd() const { return hwnd_; }

    bool Create() {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProcStatic;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"NEXUS.ProcessWindow";
        RegisterClassExW(&wc);
        std::wstring title = name_ + L" (PID " + std::to_wstring(pid_) + L") — NEXUS";
        UINT dpi = GetDpiForWindow(app_.Hwnd());
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(900, dpi, 96), MulDiv(640, dpi, 96),
                                nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return false;
        g_windows[hwnd_] = this;
        BOOL dark = app_.Th().dark;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        gfx_.Init(hwnd_);
        gfx_.SetDpi(static_cast<float>(GetDpiForWindow(hwnd_)) * static_cast<float>(app_.Config().GetDouble("ui.scale", 1.0)));
        SetupTables();
        LoadTab(General);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        return true;
    }

    void Activate() {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }
    void OnSnapshot() {
        auto s = app_.Snap();
        if (s) {
            const ProcSample* p = s->FindProc(pid_);
            exited_ = !p || p->createTime != createTime_;
        }
        if (tab_ == Performance || tab_ == General) pacer_.Invalidate(hwnd_);
    }

    LRESULT Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
        case WM_PAINT: pacer_.Paint([this] { Paint(); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: gfx_.Resize(LOWORD(lp), HIWORD(lp)); pacer_.Invalidate(h); return 0;
        case WM_DPICHANGED: {
            gfx_.SetDpi(static_cast<float>(HIWORD(wp)) * static_cast<float>(app_.Config().GetDouble("ui.scale", 1.0)));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEWHEEL:
        case WM_MOUSELEAVE: Mouse(m, wp, lp); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
            if (wp == VK_F5) { Reload(); return 0; }
            if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
                bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                SwitchTab((tab_ + (back ? TabCount - 1 : 1)) % TabCount);
                return 0;
            }
            if (tab_ >= Threads && tables_[tab_]->OnKey(static_cast<UINT>(wp), grids_[tab_])) { pacer_.Invalidate(h); return 0; }
            break;
        case WM_TIMER:
            if (pacer_.OnTimer(h, wp)) return 0;
            KillTimer(h, 1);
            in_.tooltipReady = true;
            pacer_.Invalidate(h);
            return 0;
        case WM_APP + 9: RunDeferred(); return 0;
        case WM_DESTROY:
            g_windows.erase(h);
            hwnd_ = nullptr;
            // Deleted lazily by the owner list (we are inside our own window procedure here).
            closed_ = true;
            return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    bool Closed() const { return closed_; }

private:
    void SetupTables() {
        auto& th = tables_[Threads]->columns;
        th = {Col("tid", L"TID", 70, Align::Right), Col("cpu", L"CPU", 64, Align::Right), Col("cycles", L"CPU time", 90, Align::Right),
              Col("start", L"Start address", 260), Col("prio", L"Priority", 70, Align::Right), Col("state", L"State", 110),
              Col("wait", L"Wait reason", 130), Col("cs", L"Context switches", 120, Align::Right), Col("desc", L"Description", 160)};
        tables_[Threads]->sortCol = 1;
        auto& mo = tables_[Modules]->columns;
        mo = {Col("name", L"Name", 200), Col("base", L"Base address", 140), Col("size", L"Size", 90, Align::Right), Col("desc", L"Description", 220),
              Col("company", L"Company", 160), Col("version", L"Version", 110), Col("path", L"Path", 360)};
        auto& me = tables_[Memory]->columns;
        me = {Col("base", L"Base address", 140), Col("size", L"Size", 90, Align::Right), Col("type", L"Type", 80), Col("state", L"State", 80),
              Col("prot", L"Protection", 150), Col("file", L"Mapped file", 400)};
        auto& ha = tables_[Handles]->columns;
        ha = {Col("handle", L"Handle", 90), Col("type", L"Type", 140), Col("name", L"Name", 460), Col("access", L"Granted access", 110)};
        tables_[Handles]->sortCol = 1;
        tables_[Handles]->sortDesc = false;
        auto& en = tables_[Environment]->columns;
        en = {Col("name", L"Variable", 220), Col("value", L"Value", 600)};
        auto& se = tables_[Security]->columns;
        se = {Col("name", L"Group / privilege", 320), Col("flags", L"Flags / state", 220), Col("desc", L"Description", 320)};
        auto& ne = tables_[Network]->columns;
        ne = {Col("proto", L"Protocol", 70), Col("local", L"Local address", 220), Col("remote", L"Remote address", 220), Col("state", L"State", 110),
              Col("since", L"Created", 150)};
        for (int i = 0; i < TabCount; ++i) {
            tables_[i]->showIcons = false;
            tables_[i]->compact = true;
        }
    }

    void SwitchTab(int t) {
        tab_ = t;
        if (!loaded_[t]) LoadTab(t);
        pacer_.Invalidate(hwnd_);
    }

    void Reload() {
        loaded_[tab_] = false;
        LoadTab(tab_);
    }

    void LoadTab(int t) {
        if (loading_[t]) return;
        loading_[t] = true;
        errors_[t].clear();
        uint32_t pid = pid_;
        HWND hwnd = hwnd_;
        auto meta = meta_;
        auto gen = ++gen_[t];
        auto done = [this, t, gen, hwnd](auto fill) {
            return [this, t, gen, hwnd, fill]() {
                if (!g_windows.count(hwnd) || gen != gen_[t]) return;
                fill();
                loading_[t] = false;
                loaded_[t] = true;
                grids_[t].MarkDirty();
                pacer_.Invalidate(hwnd_);
            };
        };
        switch (t) {
        case General: {
            auto out = std::make_shared<ProcessGeneral>();
            worker::Run([out, pid, meta] { *out = QueryGeneral(pid, meta.get()); }, done([this, out] { general_ = *out; }));
            break;
        }
        case Performance: loading_[t] = false; loaded_[t] = true; break;
        case Threads: {
            auto out = std::make_shared<std::vector<ThreadInfo>>();
            worker::Run([out, pid] { *out = QueryThreads(pid); }, done([this, out] { FillThreads(*out); }));
            break;
        }
        case Modules: {
            auto out = std::make_shared<std::pair<std::vector<ModuleInfo>, std::wstring>>();
            worker::Run([out, pid] { out->first = QueryModules(pid, out->second); }, done([this, out] {
                auto& g = grids_[Modules];
                g.rows.clear();
                errors_[Modules] = out->first.empty() ? out->second : L"";
                for (auto& m : out->first) {
                    GridRow r;
                    r.key = m.base;
                    r.cells = {m.name, FormatHex(m.base, 12), FormatBytes(m.size), m.description, m.company, m.version, m.path};
                    r.nums = {NoNum(), static_cast<double>(m.base), static_cast<double>(m.size)};
                    g.rows.push_back(std::move(r));
                }
            }));
            break;
        }
        case Memory: {
            auto out = std::make_shared<std::pair<MemSummary, std::wstring>>();
            worker::Run([out, pid] { out->first = QueryMemoryMap(pid, out->second); }, done([this, out] {
                memSummary_ = out->first;
                errors_[Memory] = out->second;
                auto& g = grids_[Memory];
                g.rows.clear();
                for (auto& m : out->first.list) {
                    GridRow r;
                    r.key = m.base;
                    const wchar_t* type = m.type == MEM_IMAGE ? L"Image" : m.type == MEM_MAPPED ? L"Mapped" : L"Private";
                    const wchar_t* state = m.state == MEM_COMMIT ? L"Commit" : L"Reserve";
                    r.cells = {FormatHex(m.base, 12), FormatBytes(m.size), type, state, ProtectionText(m.protect), m.mappedFile};
                    r.nums = {static_cast<double>(m.base), static_cast<double>(m.size)};
                    g.rows.push_back(std::move(r));
                }
                memSummary_.list.clear();
                memSummary_.list.shrink_to_fit();
            }));
            break;
        }
        case Handles: {
            auto out = std::make_shared<std::pair<std::vector<HandleInfo>, std::wstring>>();
            worker::Run([out, pid] { out->first = QueryHandles(pid, out->second); }, done([this, out] {
                errors_[Handles] = out->second;
                auto& g = grids_[Handles];
                g.rows.clear();
                handleTypes_.clear();
                for (auto& hi : out->first) {
                    GridRow r;
                    r.key = hi.value;
                    r.cells = {FormatHex(hi.value), hi.type, hi.name, FormatHex(hi.access, 8)};
                    r.nums = {static_cast<double>(hi.value)};
                    g.rows.push_back(std::move(r));
                    ++handleTypes_[hi.type];
                }
            }));
            break;
        }
        case Environment: {
            auto out = std::make_shared<std::pair<std::vector<std::pair<std::wstring, std::wstring>>, std::wstring>>();
            worker::Run([out, pid] { out->first = QueryEnvironment(pid, out->second); }, done([this, out] {
                errors_[Environment] = out->second;
                auto& g = grids_[Environment];
                g.rows.clear();
                uint64_t k = 1;
                for (auto& [n, v] : out->first) {
                    GridRow r;
                    r.key = k++;
                    r.cells = {n, v};
                    g.rows.push_back(std::move(r));
                }
            }));
            break;
        }
        case Security: {
            auto out = std::make_shared<std::pair<TokenDetails, std::wstring>>();
            worker::Run([out, pid] { out->first = QueryToken(pid, out->second); }, done([this, out] {
                token_ = out->first;
                errors_[Security] = out->second;
                auto& g = grids_[Security];
                g.rows.clear();
                uint64_t k = 1;
                GridRow hdr;
                hdr.key = k++;
                hdr.group = true;
                hdr.cells = {L"Groups (" + std::to_wstring(token_.groups.size()) + L")"};
                g.rows.push_back(hdr);
                for (auto& gr : token_.groups) g.rows.push_back({k++, {gr.name, gr.flags, L""}});
                GridRow ph;
                ph.key = k++;
                ph.group = true;
                ph.cells = {L"Privileges (" + std::to_wstring(token_.privileges.size()) + L")"};
                g.rows.push_back(ph);
                for (auto& pr : token_.privileges)
                    g.rows.push_back({k++, {pr.name, pr.enabled ? (pr.enabledByDefault ? L"Enabled (default)" : L"Enabled") : L"Disabled", pr.description}});
            }));
            break;
        }
        case Network: {
            auto out = std::make_shared<std::vector<Connection>>();
            worker::Run([out, pid] {
                for (auto& c : QueryConnections())
                    if (c.pid == pid) out->push_back(c);
            }, done([this, out] {
                auto& g = grids_[Network];
                g.rows.clear();
                for (auto& c : *out) {
                    GridRow r;
                    r.key = c.Key();
                    auto ep = [&](const std::wstring& a, uint16_t port) { return (c.v6 ? L"[" + a + L"]" : a) + L":" + std::to_wstring(port); };
                    FILETIME lc, ft;
                    ft.dwLowDateTime = static_cast<DWORD>(c.createTime);
                    ft.dwHighDateTime = static_cast<DWORD>(c.createTime >> 32);
                    FileTimeToLocalFileTime(&ft, &lc);
                    r.cells = {std::wstring(c.tcp ? L"TCP" : L"UDP") + (c.v6 ? L"v6" : L""), ep(c.localAddr, c.localPort),
                               c.tcp ? ep(c.remoteAddr, c.remotePort) : L"*:*", c.tcp ? TcpStateName(c.state) : L"", c.createTime ? FormatFileTime(FileTimeToU64(lc)) : L""};
                    g.rows.push_back(std::move(r));
                }
            }));
            break;
        }
        }
    }

    void FillThreads(const std::vector<ThreadInfo>& list) {
        uint64_t now = QpcNow();
        double dt = threadsQpc_ ? QpcToSeconds(now - threadsQpc_) : 0;
        threadsQpc_ = now;
        auto& g = grids_[Threads];
        g.rows.clear();
        std::map<uint32_t, uint64_t> times;
        unsigned cpus = std::max<unsigned>(1, app_.Collector().Cpu().logical);
        for (auto& t : list) {
            uint64_t total = t.kernelTime + t.userTime;
            times[t.tid] = total;
            float cpu = 0;
            auto it = threadTimes_.find(t.tid);
            if (it != threadTimes_.end() && dt > 0 && total >= it->second) cpu = static_cast<float>((total - it->second) / (dt * 1e7 * cpus) * 100.0);
            GridRow r;
            r.key = t.tid;
            r.cells = {std::to_wstring(t.tid), dt > 0 ? FormatPercent(cpu, 2) : L"—", FormatDuration100ns(total),
                       t.startModule.empty() ? FormatHex(t.startAddress, 12) : t.startModule, std::to_wstring(t.priority), ThreadStateName(t.state),
                       t.state == 5 ? WaitReasonName(t.waitReason) : L"", FormatCount(t.contextSwitches), t.description};
            r.nums = {static_cast<double>(t.tid), cpu, static_cast<double>(total), NoNum(), static_cast<double>(t.priority), NoNum(), NoNum(),
                      static_cast<double>(t.contextSwitches)};
            g.rows.push_back(std::move(r));
        }
        threadTimes_ = std::move(times);
    }

    void Mouse(UINT m, WPARAM wp, LPARAM lp) {
        float s = gfx_.Scale();
        if (m != WM_MOUSEWHEEL && m != WM_MOUSELEAVE) { in_.mx = GET_X_LPARAM(lp) / s; in_.my = GET_Y_LPARAM(lp) / s; }
        in_.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool now = true;
        switch (m) {
        case WM_MOUSEMOVE: {
            if (!tracking_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&t); tracking_ = true; }
            WidgetId hot = ui_ ? ui_->HitTest(in_.mx, in_.my) : 0;
            if (hot != lastHot_ || in_.lDown || in_.tooltipReady) {
                if (hot != lastHot_) { in_.tooltipReady = false; KillTimer(hwnd_, 1); if (hot) SetTimer(hwnd_, 1, 550, nullptr); }
                lastHot_ = hot;
                pacer_.Invalidate(hwnd_);
            }
            now = false;
            break;
        }
        case WM_MOUSELEAVE: tracking_ = false; in_.mx = in_.my = -1; lastHot_ = 0; pacer_.Invalidate(hwnd_); now = false; break;
        case WM_LBUTTONDOWN: SetCapture(hwnd_); in_.lDown = in_.lPressed = true; break;
        case WM_LBUTTONDBLCLK: in_.lDown = in_.lPressed = in_.dblClick = true; break;
        case WM_LBUTTONUP: ReleaseCapture(); in_.lDown = false; in_.lReleased = true; break;
        case WM_RBUTTONDOWN: in_.rDown = in_.rPressed = true; break;
        case WM_RBUTTONUP: in_.rDown = false; in_.rReleased = true; break;
        case WM_MOUSEWHEEL: in_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA); break;
        }
        if (now) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    void RunDeferred() {
        auto d = std::move(deferred_);
        deferred_.clear();
        for (auto& f : d) f();
        if (hwnd_) pacer_.Invalidate(hwnd_);
    }

    void Paint() {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        if (!gfx_.Begin()) { EndPaint(hwnd_, &ps); return; }
        const Theme& t = app_.Th();
        if (!ui_) ui_ = std::make_unique<Ui>(gfx_, t, in_);
        Ui& ui = *ui_;
        ui.BeginFrame();
        Gfx& g = gfx_;
        g.Clear(t.window);
        float W = g.Width(), H = g.Height();
        // Header
        ID2D1Bitmap* icon = meta_ ? app_.Icons().Get(meta_->path) : nullptr;
        if (icon) g.DrawBitmap(icon, {16, 16, 32, 32});
        g.Text(name_, {58, 10, W - 360, 26}, Font::Subtitle, t.text);
        std::wstring sub = L"PID " + std::to_wstring(pid_) + (meta_ && !meta_->description.empty() ? L"  ·  " + meta_->description : L"") +
                           (exited_ ? L"  ·  exited" : L"");
        g.Text(sub, {58, 34, W - 360, 18}, Font::Small, exited_ ? t.critical : t.textSecondary);
        float bx = W - 16;
        auto button = [&](const char* id, const wchar_t* label, wchar_t glyph, float w, ButtonStyle st, bool en) {
            bx -= w;
            bool r = ui.Button(Id(id), {bx, 14, w, 30}, label, glyph, st, en);
            bx -= 8;
            return r;
        };
        auto snap = app_.Snap();
        const ProcSample* p = snap ? snap->FindProc(pid_) : nullptr;
        if (p && p->createTime != createTime_) p = nullptr;
        if (button("pw.end", L"End process", 0xE711, 120, ButtonStyle::Danger, p != nullptr) && p) {
            ProcSample copy = *p;
            deferred_.push_back([this, copy] { actions::EndTask(app_, copy); });
        }
        if (button("pw.loc", L"Open location", 0xE838, 130, ButtonStyle::Standard, meta_ && !meta_->path.empty())) OpenFileLocation(meta_->path);
        if (button("pw.refresh", L"Refresh", 0xE72C, 100, ButtonStyle::Standard, true)) Reload();

        // Tabs
        std::vector<std::wstring> labels(kTabNames, kTabNames + TabCount);
        int clicked = ui.Tabs(Id("pw.tabs"), {16, 60, W - 32, 34}, labels, tab_);
        if (clicked >= 0) SwitchTab(clicked);
        g.Line(16, 96, W - 16, 96, t.divider);
        Rect body{16, 104, W - 32, H - 116};
        if (loading_[tab_]) g.Text(L"Loading…", {body.x, body.y, 200, 24}, Font::Body, t.textSecondary);
        switch (tab_) {
        case General: DrawGeneral(ui, body, p); break;
        case Performance: DrawPerformance(ui, body, p); break;
        default: DrawList(ui, body); break;
        }
        ui.EndFrame();
        g.End();
        EndPaint(hwnd_, &ps);
        in_.ClearEvents();
        for (auto& f : ui.TakeDeferred()) deferred_.push_back(std::move(f));
        if (!deferred_.empty()) PostMessageW(hwnd_, WM_APP + 9, 0, 0);
        lastHot_ = ui.HitTest(in_.mx, in_.my);
    }

    void DrawGeneral(Ui& ui, const Rect& r, const ProcSample* p) {
        Gfx& g = gfx_;
        const Theme& t = app_.Th();
        const ProcessGeneral& gi = general_;
        float colW = std::min(560.f, r.w);
        float y = r.y - scroll_;
        float kw = 170;
        g.PushClip(r);
        auto kv = [&](const std::wstring& k, const std::wstring& v, Color c = Color::None()) {
            if (y > r.y - 24 && y < r.B()) ui.KeyValue(r.x, y, kw, colW - kw, k, v, c);
            y += 24;
        };
        auto section = [&](const wchar_t* s) {
            y += 8;
            if (y > r.y - 24 && y < r.B()) g.Text(s, {r.x, y, colW, 24}, Font::BodySemi, t.text);
            y += 28;
        };
        section(L"Image");
        kv(L"Name", name_);
        kv(L"Description", gi.description);
        kv(L"Company", gi.company);
        kv(L"Version", gi.version);
        kv(L"Path", gi.path);
        kv(L"Command line", gi.commandLine);
        kv(L"Current directory", gi.currentDirectory);
        Color sigC = gi.signature.state == SignatureInfo::Signed ? t.good : gi.signature.state == SignatureInfo::Unknown ? t.textSecondary : t.warning;
        kv(L"Signature", std::wstring(gi.signature.StateName()) + (gi.signature.catalog ? L" (catalog)" : L"") +
                             (gi.signature.signer.empty() ? L"" : L" — " + gi.signature.signer), sigC);
        if (!gi.packageName.empty()) kv(L"Package", gi.packageName);
        section(L"Process");
        kv(L"PID", std::to_wstring(pid_));
        if (p) {
            const ProcSample* parent = app_.Snap()->FindProc(p->ppid);
            kv(L"Parent", (parent && parent->createTime <= p->createTime ? parent->Name() + L" " : std::wstring(L"<exited> ")) + L"(" + std::to_wstring(p->ppid) + L")");
            kv(L"Session", std::to_wstring(p->session));
            kv(L"User", meta_ ? meta_->user : L"");
        }
        kv(L"Started", gi.startTime);
        kv(L"Architecture", gi.architecture);
        kv(L"Integrity level", gi.integrity);
        kv(L"Elevated", meta_ ? (meta_->elevated ? L"Yes" : L"No") : L"");
        kv(L"Protection", gi.protection);
        kv(L"Critical process", meta_ && meta_->isCritical ? L"Yes (terminating it stops Windows)" : L"No");
        kv(L"Priority", gi.priority);
        wchar_t aff[64];
        swprintf(aff, 64, L"0x%llX of 0x%llX", static_cast<unsigned long long>(gi.affinity), static_cast<unsigned long long>(gi.systemAffinity));
        kv(L"Affinity", gi.accessible ? aff : L"");
        section(L"Mitigations");
        kv(L"DEP", gi.dep);
        kv(L"ASLR", gi.aslr);
        kv(L"Control Flow Guard", gi.cfg);
        kv(L"Hardware stack protection", gi.cet);
        if (!gi.accessible && loaded_[General]) {
            y += 8;
            g.Text(L"Some fields are unavailable: Windows denied access to this process. Run NEXUS as administrator for more detail.", {r.x, y, colW, 22},
                   Font::Small, t.warning);
            y += 26;
        }
        g.PopClip();
        float content = y + scroll_ - r.y;
        ui.Scroll(Id("pw.gen.scroll"), r, content, scroll_);
    }

    void DrawPerformance(Ui& ui, const Rect& r, const ProcSample* p) {
        Gfx& g = gfx_;
        const Theme& t = app_.Th();
        auto& ph = app_.ProcHistory();
        auto it = ph.find(key_);
        if (it == ph.end()) { g.Text(L"No history yet.", r, Font::Body, t.textSecondary, Align::Center); return; }
        const ProcHist& h = it->second;
        double iv = app_.Snap() ? app_.Snap()->interval : 1.0;
        float cw = (r.w - 16) / 2, ch = (r.h - 60) / 2;
        auto chart = [&](int idx, const wchar_t* title, const std::wstring& value, const RingBuffer<float>& data, float maxV, Color c,
                         std::function<std::wstring(float)> fmt) {
            float x = r.x + (idx % 2) * (cw + 16), y = r.y + (idx / 2) * (ch + 30);
            g.Text(title, {x, y, cw / 2, 22}, Font::BodySemi, t.text);
            g.Text(value, {x + cw / 2, y, cw / 2, 22}, Font::Body, t.textSecondary, Align::Right);
            graphs_[idx].Draw(ui, IdMix(Id("pw.graph"), idx), {x, y + 24, cw, ch - 4}, {{&data, c, true, title}}, maxV, iv, fmt);
        };
        unsigned cores = std::max<unsigned>(1, app_.Collector().Cpu().logical);
        chart(0, L"CPU", p ? FormatPercent(p->cpu, 1) + L" (" + FormatPercent(p->cpu * cores, 0) + L" of one core)" : L"", h.cpu,
              std::max(5.f, std::ceil(h.cpu.Max() / 10.f) * 10.f), t.cpu, [](float v) { return FormatPercent(v, 1); });
        chart(1, L"Private memory", p ? FormatMB(p->privateBytes) : L"", h.mem, std::max(1.f, h.mem.Max() * 1.2f), t.memory,
              [](float v) { return FormatMB(static_cast<uint64_t>(v * 1024 * 1024)); });
        chart(2, L"I/O", p ? FormatRateBytes(p->ioReadRate + p->ioWriteRate) : L"", h.io, std::max(1024.f, h.io.Max() * 1.2f), t.disk,
              [](float v) { return FormatRateBytes(v); });
        bool net = app_.Snap() && app_.Snap()->netPerProcess;
        if (net)
            chart(3, L"Network", p ? FormatNetRate(p->netSend + p->netRecv, app_.NetUnits()) : L"", h.net, std::max(1024.f, h.net.Max() * 1.2f), t.network,
                  [this](float v) { return FormatNetRate(v, app_.NetUnits()); });
        else
            chart(3, L"GPU", p ? FormatPercent(p->gpu, 1) : L"", h.gpu, 100.f, t.gpu, [](float v) { return FormatPercent(v, 1); });
        if (p) {
            std::wstring stats = L"Working set " + FormatMB(p->workingSet) + L"   Peak " + FormatMB(p->peakWorkingSet) + L"   Threads " + std::to_wstring(p->threads) +
                                 L"   Handles " + FormatCount(p->handles) + L"   Page faults " + FormatCount(p->pageFaults) + L"   CPU time " +
                                 FormatDuration100ns(p->cpuTime) + L"   I/O read " + FormatBytes(p->ioReadBytes) + L"   I/O write " + FormatBytes(p->ioWriteBytes);
            g.Text(stats, {r.x, r.B() - 22, r.w, 22}, Font::Small, t.textSecondary);
        }
    }

    void DrawList(Ui& ui, const Rect& r) {
        Gfx& g = gfx_;
        const Theme& t = app_.Th();
        Rect body = r;
        if (!errors_[tab_].empty()) {
            g.Text(errors_[tab_], {r.x, r.y, r.w, 22}, Font::Small, t.warning);
            body.y += 26;
            body.h -= 26;
        }
        if (tab_ == Memory && loaded_[Memory]) {
            std::wstring s = L"Committed: image " + FormatBytes(memSummary_.imageCommit) + L", mapped " + FormatBytes(memSummary_.mappedCommit) + L", private " +
                             FormatBytes(memSummary_.privateCommit) + L"   Reserved " + FormatBytes(memSummary_.reserved) + L"   Regions " +
                             FormatCount(memSummary_.regions);
            g.Text(s, {body.x, body.y, body.w, 22}, Font::Small, t.textSecondary);
            body.y += 26;
            body.h -= 26;
        }
        if (tab_ == Handles && loaded_[Handles]) {
            std::vector<std::pair<int, std::wstring>> v;
            for (auto& [k, n] : handleTypes_) v.push_back({n, k});
            std::sort(v.rbegin(), v.rend());
            std::wstring s = FormatCount(grids_[Handles].rows.size()) + L" handles:";
            for (size_t i = 0; i < v.size() && i < 8; ++i) s += L"  " + v[i].second + L" " + std::to_wstring(v[i].first);
            g.Text(s, {body.x, body.y, body.w, 22}, Font::Small, t.textSecondary);
            body.y += 26;
            body.h -= 26;
        }
        if (tab_ == Security && loaded_[Security]) {
            std::wstring s = token_.user + L"   Integrity " + token_.integrity + L"   Elevation " + token_.elevationType + L"   Session " +
                             std::to_wstring(token_.session) + (token_.appContainer ? L"   AppContainer" : L"") +
                             (token_.virtualizationEnabled ? L"   UAC virtualization on" : L"");
            g.Text(s, {body.x, body.y, body.w, 22}, Font::Small, t.textSecondary);
            body.y += 26;
            body.h -= 26;
        }
        Table& tbl = *tables_[tab_];
        SimpleGrid& grid = grids_[tab_];
        if (grid.Dirty() || tbl.sortChanged) { grid.Refresh(tbl); tbl.sortChanged = false; }
        tbl.Draw(ui, body, grid);
        if (tbl.contextMenu) {
            tbl.contextMenu = false;
            int row = tbl.SelectedRow(grid);
            if (row >= 0) {
                std::wstring line;
                std::wstring cell;
                for (size_t c = 0; c < tbl.columns.size(); ++c) {
                    grid.CellText(row, static_cast<int>(c), cell);
                    if (c) line += L"\t";
                    line += cell;
                }
                deferred_.push_back([this, line] {
                    if (app_.ShowMenu({{1, L"Copy row"}}) == 1) CopyTextToClipboard(hwnd_, line);
                });
            }
        }
        if (tab_ == Threads && loaded_[Threads])
            g.Text(L"CPU column shows usage since the previous refresh (F5).", {body.x, body.B() + 2, body.w, 14}, Font::Caption, t.textTertiary);
    }

    App& app_;
    uint32_t pid_;
    uint64_t createTime_, key_;
    std::shared_ptr<const ProcMeta> meta_;
    std::wstring name_;
    HWND hwnd_ = nullptr;
    Gfx gfx_;
    FramePacer pacer_{7};
    InputState in_;
    std::unique_ptr<Ui> ui_;
    int tab_ = General;
    bool loaded_[TabCount] = {}, loading_[TabCount] = {};
    uint32_t gen_[TabCount] = {};
    std::wstring errors_[TabCount];
    std::unique_ptr<Table> tables_[TabCount];
    SimpleGrid grids_[TabCount];
    ProcessGeneral general_;
    MemSummary memSummary_;
    TokenDetails token_;
    std::map<std::wstring, int> handleTypes_;
    std::map<uint32_t, uint64_t> threadTimes_;
    uint64_t threadsQpc_ = 0;
    GraphView graphs_[4];
    float scroll_ = 0;
    bool exited_ = false, tracking_ = false, closed_ = false;
    WidgetId lastHot_ = 0;
    std::vector<std::function<void()>> deferred_;
};

LRESULT CALLBACK WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto it = g_windows.find(h);
    if (it != g_windows.end()) return it->second->Proc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void Collect() {
    g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(), [](const std::unique_ptr<Window>& w) { return w->Closed(); }), g_owned.end());
}

} // namespace

void Open(App& app, uint32_t pid) {
    Collect();
    auto snap = app.Snap();
    const ProcSample* p = snap ? snap->FindProc(pid) : nullptr;
    if (!p) { app.ShowStatus(L"The process has exited.", true); return; }
    for (auto& w : g_owned)
        if (!w->Closed() && w->Pid() == pid && w->CreateTime() == p->createTime) { w->Activate(); return; }
    auto w = std::make_unique<Window>(app, *p);
    if (w->Create()) g_owned.push_back(std::move(w));
}

void OnSnapshotAll() {
    Collect();
    for (auto& w : g_owned)
        if (!w->Closed()) w->OnSnapshot();
}

void CloseAll() {
    for (auto& w : g_owned)
        if (!w->Closed() && w->Hwnd()) DestroyWindow(w->Hwnd());
    g_owned.clear();
}

} // namespace nx::ProcessWindow
