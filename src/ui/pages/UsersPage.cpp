// Users page: signed-in sessions with per-user resource totals and their processes.
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

#include <shellapi.h>

#include <map>
#include <set>

namespace nx {

namespace {

enum { XUser, XStatus, XSession, XClient, XCpu, XMem, XDisk, XNet, XGpu, XLogon };

struct URow {
    bool user = true;
    uint64_t key = 0;
    const SessionInfo* session = nullptr;
    const ProcSample* proc = nullptr;
    std::wstring name;
    float cpu = 0, io = 0, net = -1, gpu = 0;
    uint64_t mem = 0;
    int count = 0;
};

class UsersPage final : public Page, public TableSource {
public:
    explicit UsersPage(App& a) : Page(a), table_(Id("users.table")) {
        table_.columns = {Col("user", L"User", 260), Col("status", L"Status", 110), Col("id", L"ID", 50, Align::Right, false),
                          Col("client", L"Client name", 120, Align::Left, false), HeatCol("cpu", L"CPU", 80), HeatCol("mem", L"Memory", 100),
                          HeatCol("disk", L"Disk I/O", 100), HeatCol("net", L"Network", 100), HeatCol("gpu", L"GPU", 80),
                          Col("logon", L"Signed in", 150, Align::Left, false)};
        table_.LoadLayout(app.Config().GetString("users.layout"));
    }
    const wchar_t* Title() const override { return L"Users"; }
    wchar_t Glyph() const override { return 0xE716; }
    uint32_t Demand() const override { return demand::GpuProcess | demand::GpuSystem; }
    void OnShow() override { Load(); }
    void OnSnapshot() override {
        if (app.Current() != PageId::Users) { rows_.clear(); return; }
        if (GetTickCount64() - lastLoad_ > 5000) Load();
        Rebuild();
    }
    void SaveState() override { app.Config().SetString("users.layout", table_.SaveLayout()); }
    std::wstring AccessibleSummary() const override { return L"Users, " + std::to_wstring(sessions_.size()) + L" signed-in sessions."; }
    bool OnKey(UINT vk, bool, bool shift) override {
        if (vk == VK_APPS || (vk == VK_F10 && shift)) { Menu(); return true; }
        return table_.OnKey(vk, *this);
    }

    size_t RowCount() const override { return rows_.size(); }
    uint64_t RowKey(size_t r) const override { return rows_[r].key; }
    int RowDepth(size_t r) const override { return rows_[r].user ? 0 : 1; }
    int RowExpander(size_t r) const override { return rows_[r].user ? (expanded_.count(rows_[r].key) ? 2 : 1) : 0; }
    void ToggleExpand(size_t r) override {
        if (!rows_[r].user) return;
        if (!expanded_.erase(rows_[r].key)) expanded_.insert(rows_[r].key);
        Rebuild();
        app.Invalidate();
    }
    ID2D1Bitmap* RowIcon(size_t r) override { return rows_[r].user ? nullptr : app.Icons().Get(rows_[r].proc && rows_[r].proc->meta ? rows_[r].proc->meta->path : L""); }
    float CellHeat(size_t r, int col) const override {
        const URow& u = rows_[r];
        auto s = app.Snap();
        switch (col) {
        case XCpu: return std::min(1.f, u.cpu / 60.f);
        case XMem: return s && s->mem.total ? std::min(1.f, static_cast<float>(u.mem) / s->mem.total * 3) : 0;
        case XDisk: return std::min(1.f, u.io / (20.f * 1024 * 1024));
        case XNet: return u.net > 0 ? std::min(1.f, u.net / (5.f * 1024 * 1024)) : 0.f;
        case XGpu: return std::min(1.f, u.gpu / 60.f);
        }
        return -1;
    }
    void CellText(size_t r, int col, std::wstring& out) const override {
        const URow& u = rows_[r];
        out.clear();
        switch (col) {
        case XUser: out = u.name; break;
        case XStatus: if (u.session) out = u.session->state; break;
        case XSession: if (u.session) out = std::to_wstring(u.session->id); break;
        case XClient: if (u.session) out = u.session->client; break;
        case XCpu: out = FormatPercent(u.cpu, 1); break;
        case XMem: out = FormatMB(u.mem); break;
        case XDisk: out = FormatRateBytes(u.io); break;
        case XNet: out = u.net >= 0 ? FormatNetRate(u.net, app.NetUnits()) : L"—"; break;
        case XGpu: out = FormatPercent(u.gpu, 1); break;
        case XLogon:
            if (u.session && u.session->logonTime) {
                FILETIME ft{static_cast<DWORD>(u.session->logonTime), static_cast<DWORD>(u.session->logonTime >> 32)}, lt;
                FileTimeToLocalFileTime(&ft, &lt);
                out = FormatFileTime(FileTimeToU64(lt));
            }
            break;
        }
    }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        const URow* s = SelectedRow();
        bool userSel = s && s->user && s->session;
        if (ui.Button(Id("users.manage"), {bar.R() - 190, bar.y + 4, 190, 32}, L"Manage user accounts", 0xE77B))
            ShellExecuteW(app.Hwnd(), L"open", L"ms-settings:otherusers", nullptr, nullptr, SW_SHOWNORMAL);
        if (ui.Button(Id("users.signoff"), {bar.R() - 302, bar.y + 4, 104, 32}, L"Sign off", 0, ButtonStyle::Standard, userSel)) ui.Defer([this] { SignOff(); });
        if (ui.Button(Id("users.disc"), {bar.R() - 424, bar.y + 4, 114, 32}, L"Disconnect", 0, ButtonStyle::Standard, userSel && s->session->state == L"Active"))
            ui.Defer([this] { Disconnect(); });
        ui.g.Text(std::to_wstring(sessions_.size()) + L" signed-in user session(s)", {bar.x, bar.y, 300, 40}, Font::Small, ui.t.textTertiary);
        if (table_.sortChanged) { table_.sortChanged = false; Rebuild(); }
        table_.Draw(ui, r, *this);
        if (table_.contextMenu) { table_.contextMenu = false; ui.Defer([this] { Menu(); }); }
        if (table_.activated) {
            table_.activated = false;
            const URow* sr = SelectedRow();
            if (sr && sr->proc) { uint32_t pid = sr->proc->pid; ui.Defer([this, pid] { app.OpenProcessWindow(pid); }); }
        }
        if (table_.headerMenu) { table_.headerMenu = false; ui.Defer([this] { ColumnMenu(app, table_); }); }
    }

private:
    const URow* SelectedRow() const {
        int r = table_.SelectedRow(*this);
        return r >= 0 ? &rows_[r] : nullptr;
    }

    void Load() {
        if (loading_) return;
        loading_ = true;
        lastLoad_ = GetTickCount64();
        auto out = std::make_shared<std::vector<SessionInfo>>();
        worker::Run([out] { *out = QuerySessions(); }, [this, out] {
            sessions_ = std::move(*out);
            loading_ = false;
            Rebuild();
            app.Invalidate();
        });
    }

    void Rebuild() {
        rows_.clear();
        auto snap = app.Snap();
        if (!snap) return;
        for (auto& se : sessions_) {
            URow u;
            u.session = &se;
            u.key = 0xC0000000ULL + se.id;
            u.name = se.user + (se.current ? L" (this session)" : L"");
            std::vector<URow> kids;
            for (auto& p : snap->procs) {
                if (p.session != se.id || p.pid == 0) continue;
                // Only processes running as this user (session 0-style services in the session are excluded).
                if (p.meta && !p.meta->userShort.empty() && _wcsicmp(p.meta->userShort.c_str(), se.user.c_str()) != 0) continue;
                URow k;
                k.user = false;
                k.proc = &p;
                k.key = p.Key();
                k.name = p.meta ? p.meta->DisplayName() : L"";
                k.cpu = p.cpu;
                k.mem = p.privateWorkingSet;
                k.io = p.ioReadRate + p.ioWriteRate;
                k.net = p.netSend >= 0 ? p.netSend + p.netRecv : -1;
                k.gpu = p.gpu;
                u.cpu += k.cpu;
                u.mem += k.mem;
                u.io += k.io;
                if (k.net >= 0) u.net = std::max(0.f, u.net) + k.net;
                u.gpu = std::max(u.gpu, k.gpu);
                ++u.count;
                kids.push_back(k);
            }
            u.name += L" (" + std::to_wstring(u.count) + L")";
            rows_.push_back(u);
            if (expanded_.count(u.key)) {
                int col = table_.sortCol;
                bool desc = table_.sortDesc;
                std::stable_sort(kids.begin(), kids.end(), [&](const URow& a, const URow& b) {
                    double x, y;
                    switch (col) {
                    case XCpu: x = a.cpu; y = b.cpu; break;
                    case XMem: x = static_cast<double>(a.mem); y = static_cast<double>(b.mem); break;
                    case XDisk: x = a.io; y = b.io; break;
                    case XNet: x = a.net; y = b.net; break;
                    case XGpu: x = a.gpu; y = b.gpu; break;
                    default: return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                    }
                    return desc ? x > y : x < y;
                });
                for (auto& k : kids) rows_.push_back(k);
            }
        }
    }

    void Disconnect() {
        const URow* s = SelectedRow();
        if (!s || !s->session) return;
        uint32_t id = s->session->id;
        std::wstring user = s->session->user;
        if (!app.Confirm(L"Disconnect " + user + L"?", L"The user's programs keep running, but they will need to sign in again to see their desktop.", L"Disconnect user")) return;
        DWORD e = DisconnectUserSession(id);
        if (e) app.ShowStatus(L"Could not disconnect " + user + L": " + LastErrorMessage(e), true);
        Load();
    }

    void SignOff() {
        const URow* s = SelectedRow();
        if (!s || !s->session) return;
        uint32_t id = s->session->id;
        std::wstring user = s->session->user;
        bool self = s->session->current;
        if (!app.Confirm(L"Sign off " + user + L"?", self ? L"You are signing yourself out. Unsaved data in all your open programs will be lost."
                                                         : L"Unsaved data in the user's open programs will be lost.",
                         L"Sign off user", true))
            return;
        DWORD e = LogoffUserSession(id);
        if (e == ERROR_ACCESS_DENIED && !app.Elevated()) { app.RequestElevatedRestart(L"Signing off another user requires administrator rights."); return; }
        if (e) app.ShowStatus(L"Could not sign off " + user + L": " + LastErrorMessage(e), true);
        Load();
    }

    void Menu() {
        const URow* s = SelectedRow();
        if (!s) return;
        if (s->proc) { actions::ProcessContextMenu(app, *s->proc, false); return; }
        bool active = s->session && s->session->state == L"Active";
        UINT c = app.ShowMenu({{1, L"Expand / collapse"}, {2, L"Disconnect", false, active}, {3, L"Sign off"}, MenuItem::Sep(), {4, L"Manage user accounts"}});
        if (c == 1) { int r = table_.SelectedRow(*this); if (r >= 0) ToggleExpand(r); }
        if (c == 2) Disconnect();
        if (c == 3) SignOff();
        if (c == 4) ShellExecuteW(app.Hwnd(), L"open", L"ms-settings:otherusers", nullptr, nullptr, SW_SHOWNORMAL);
    }

    Table table_;
    std::vector<SessionInfo> sessions_;
    std::vector<URow> rows_;
    std::set<uint64_t> expanded_;
    uint64_t lastLoad_ = 0;
    bool loading_ = false;
};

} // namespace

std::unique_ptr<Page> MakeUsersPage(App& app) { return std::make_unique<UsersPage>(app); }

} // namespace nx
