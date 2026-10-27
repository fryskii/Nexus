// Network: live throughput, per-process traffic (ETW, admin), TCP/UDP endpoints, connection
// history, adapter details/counters and latency / DNS diagnostics.
#include <winsock2.h>
#include <iphlpapi.h>

#include "data/Connections.h"
#include "data/Worker.h"
#include "ui/pages/Common.h"

#include <map>
#include <set>
#include <unordered_map>

namespace nx {

namespace {

enum Tab { TOverview, TApps, TConnections, THistory, TAdapters, TTools };

class NetworkPage final : public Page {
public:
    explicit NetworkPage(App& a) : Page(a), apps_(Id("net.apps")), conns_(Id("net.conns")), history_(Id("net.hist")), adapters_(Id("net.adapters")) {
        apps_.columns = {Col("name", L"Process", 260), Col("pid", L"PID", 70, Align::Right), Col("send", L"Send", 110, Align::Right),
                         Col("recv", L"Receive", 110, Align::Right), Col("total", L"Total", 110, Align::Right), Col("conns", L"Connections", 100, Align::Right),
                         Col("tcp", L"Established TCP", 120, Align::Right)};
        apps_.sortCol = 4;
        conns_.columns = {Col("proc", L"Process", 200), Col("pid", L"PID", 70, Align::Right), Col("proto", L"Protocol", 80), Col("local", L"Local address", 220),
                          Col("remote", L"Remote address", 220), Col("host", L"Remote host", 220), Col("state", L"State", 110), Col("since", L"Created", 150)};
        conns_.sortCol = 0;
        conns_.sortDesc = false;
        history_.columns = {Col("proc", L"Process", 200), Col("proto", L"Protocol", 80), Col("remote", L"Remote address", 240), Col("host", L"Remote host", 220),
                            Col("first", L"First seen", 150), Col("last", L"Last seen", 150), Col("status", L"Status", 90)};
        history_.sortCol = 5;
        adapters_.columns = {Col("name", L"Adapter", 200), Col("desc", L"Description", 260), Col("status", L"Status", 100), Col("speed", L"Link speed", 110, Align::Right),
                             Col("sent", L"Bytes sent", 110, Align::Right), Col("recv", L"Bytes received", 120, Align::Right), Col("pkts", L"Packets", 110, Align::Right),
                             Col("errors", L"Errors", 80, Align::Right), Col("discards", L"Discards", 80, Align::Right), Col("ipv4", L"IPv4", 160),
                             Col("gw", L"Gateway", 140), Col("dns", L"DNS servers", 200), Col("mac", L"MAC address", 150), Col("mtu", L"MTU", 60, Align::Right)};
        for (Table* t : {&apps_, &conns_, &history_, &adapters_}) { t->heatMap = false; t->compact = true; }
        conns_.showIcons = history_.showIcons = adapters_.showIcons = false;
        tab_ = static_cast<int>(app.Config().GetInt("network.tab", 0));
        resolve_ = app.Config().GetBool("network.resolve", false);
    }
    const wchar_t* Title() const override { return L"Network"; }
    wchar_t Glyph() const override { return 0xE839; }
    bool HasSearch() const override { return tab_ == TApps || tab_ == TConnections || tab_ == THistory; }
    const wchar_t* SearchHint() const override { return L"Filter by process, address or port"; }
    void OnSearch(const std::wstring& s) override {
        filter_ = s;
        appsGrid_.SetFilter(s);
        connGrid_.SetFilter(s);
        histGrid_.SetFilter(s);
    }
    void OnShow() override {
        app.EnsureNicInfo(true);
        LoadConnections();
    }
    void OnHide() override { tools_.Hide(); }
    void OnSnapshot() override {
        if (app.Current() != PageId::Network) return;
        if (GetTickCount64() - lastConn_ > 2000) LoadConnections();
        FillApps();
        FillAdapters();
        app.EnsureNicInfo();
    }
    void SaveState() override {
        app.Config().SetInt("network.tab", tab_);
        app.Config().SetBool("network.resolve", resolve_);
    }
    std::wstring AccessibleSummary() const override { return L"Network, " + std::to_wstring(connections_.size()) + L" active endpoints."; }
    bool OnKey(UINT vk, bool, bool) override {
        Table* t = tab_ == TApps ? &apps_ : tab_ == TConnections ? &conns_ : tab_ == THistory ? &history_ : tab_ == TAdapters ? &adapters_ : nullptr;
        SimpleGrid* g = tab_ == TApps ? &appsGrid_ : tab_ == TConnections ? &connGrid_ : tab_ == THistory ? &histGrid_ : &adGrid_;
        return t && t->OnKey(vk, *g);
    }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        int t = ui.Tabs(Id("net.tabs"), {bar.x, bar.y + 4, bar.w, 32}, {L"Overview", L"Apps", L"Connections", L"History", L"Adapters", L"Tools"}, tab_);
        if (t >= 0 && t != tab_) {
            tab_ = t;
            if (tab_ != TTools) tools_.Hide();
            app.Invalidate();
        }
        r.y += 4;
        switch (tab_) {
        case TOverview: DrawOverview(ui, r); break;
        case TApps: DrawTable(ui, r, apps_, appsGrid_, true); break;
        case TConnections: DrawConnections(ui, r); break;
        case THistory: DrawTable(ui, r, history_, histGrid_, false); break;
        case TAdapters: DrawTable(ui, r, adapters_, adGrid_, false); break;
        case TTools: DrawTools(ui, r); break;
        }
    }

private:
    void DrawOverview(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        auto snap = app.Snap();
        if (!snap) return;
        auto& h = app.Hist();
        double send = 0, recv = 0;
        for (auto& n : snap->nics) { send += n.sendBps; recv += n.recvBps; }
        float half = (r.w - 16) / 2;
        Rect c1{r.x, r.y, half, 78}, c2{r.x + half + 16, r.y, half, 78};
        ui.Card(c1);
        ui.Card(c2);
        g.Text(L"Download", {c1.x + 16, c1.y + 10, 200, 20}, Font::Small, t.textSecondary);
        g.Text(FormatNetRate(recv, app.NetUnits()), {c1.x + 16, c1.y + 30, half - 32, 36}, Font::Display, t.network);
        g.Text(L"Upload", {c2.x + 16, c2.y + 10, 200, 20}, Font::Small, t.textSecondary);
        g.Text(FormatNetRate(send, app.NetUnits()), {c2.x + 16, c2.y + 30, half - 32, 36}, Font::Display, t.network.Mix(t.text, 0.4f));
        float gy = c1.B() + 30;
        float gh = std::max(120.f, r.B() - gy - 150);
        Rect gr{r.x, gy, r.w, gh};
        float mx = std::max(1024.f, std::max(h.netTotalRecv.Max(), h.netTotalSend.Max()) * 1.25f);
        g.Text(L"Total throughput (all adapters)", {gr.x, gr.y - 20, gr.w / 2, 18}, Font::Caption, t.textSecondary);
        g.Text(FormatNetRate(mx, app.NetUnits()), {gr.x + gr.w / 2, gr.y - 20, gr.w / 2, 18}, Font::Caption, t.textSecondary, Align::Right);
        graph_.Draw(ui, Id("net.graph"), gr, {{&h.netTotalRecv, t.network, true, L"Receive"}, {&h.netTotalSend, t.network.Mix(t.text, 0.45f), false, L"Send"}}, mx,
                    snap->interval, [this](float v) { return FormatNetRate(v, app.NetUnits()); }, true, true);
        float y = gr.B() + 26;
        size_t tcpEst = 0, listen = 0, udp = 0;
        std::set<std::wstring> remotes;
        for (auto& c : connections_) {
            if (!c.tcp) ++udp;
            else if (c.state == MIB_TCP_STATE_ESTAB) { ++tcpEst; remotes.insert(c.remoteAddr); }
            else if (c.state == MIB_TCP_STATE_LISTEN) ++listen;
        }
        std::wstring stats = L"Established TCP connections: " + std::to_wstring(tcpEst) + L"    Listening TCP ports: " + std::to_wstring(listen) +
                             L"    UDP endpoints: " + std::to_wstring(udp) + L"    Distinct remote hosts: " + std::to_wstring(remotes.size());
        g.Text(stats, {r.x, y, r.w, 22}, Font::Body, t.text);
        y += 26;
        std::wstring etw = snap->netPerProcess ? L"Per-process throughput: measured from kernel network events (TCP/UDP payload bytes)."
                                               : app.Collector().NetTraceStatus();
        InfoText(g, t, {r.x, y, r.w, 40},
                 L"Throughput is measured per adapter by Windows interface counters. " + etw +
                     L" Connection counts come from the TCP/UDP endpoint tables and say nothing about bandwidth on their own.");
    }

    void DrawConnections(Ui& ui, Rect r) {
        Rect bar = r.TakeTop(40);
        bool res = resolve_;
        if (ui.Toggle(Id("net.resolve"), {bar.x, bar.y + 4, 260, 30}, res, L"Resolve remote host names")) {
            resolve_ = res;
            LoadConnections();
        }
        ui.g.Text(std::to_wstring(connections_.size()) + L" endpoints · refreshed every 2 s", {bar.x + 280, bar.y, 400, 36}, Font::Small, ui.t.textTertiary);
        DrawTable(ui, r, conns_, connGrid_, false);
    }

    void DrawTable(Ui& ui, const Rect& r, Table& t, SimpleGrid& g, bool processRows) {
        g.icons = processRows ? &app.Icons() : nullptr;
        if (g.Dirty() || t.sortChanged) { g.Refresh(t); t.sortChanged = false; }
        t.Draw(ui, r, g);
        if (&t == &apps_ && !app.Snap()->netPerProcess)
            ui.g.Text(app.Collector().NetTraceStatus() + L" Connection counts are shown without throughput.", {r.x, r.B() - 22, r.w, 20}, Font::Small, ui.t.warning);
        if (t.headerMenu) { t.headerMenu = false; Table* tp = &t; ui.Defer([this, tp] { ColumnMenu(app, *tp); }); }
        if (t.contextMenu || t.activated) {
            t.contextMenu = t.activated = false;
            int row = t.SelectedRow(g);
            const GridRow* gr = row >= 0 ? g.RowAt(row) : nullptr;
            if (!gr) return;
            std::wstring line;
            for (auto& c : gr->cells) { if (!line.empty()) line += L"\t"; line += c; }
            uint32_t pid = 0;
            if (&t == &apps_ || &t == &conns_) pid = static_cast<uint32_t>(_wtoi(gr->cells[1].c_str()));
            std::wstring remote = &t == &conns_ ? gr->cells[4] : &t == &history_ ? gr->cells[2] : L"";
            ui.Defer([this, line, pid, remote] {
                UINT c = app.ShowMenu({{1, L"Copy row"}, {2, L"Inspect process...", false, pid != 0}, {3, L"Ping remote host", false, !remote.empty()},
                                       {4, L"Search remote address online", false, !remote.empty()}});
                std::wstring host = remote.substr(0, remote.rfind(L':'));
                if (!host.empty() && host[0] == L'[') host = host.substr(1, host.size() - 2);
                if (c == 1) CopyTextToClipboard(app.Hwnd(), line);
                if (c == 2) app.OpenProcessWindow(pid);
                if (c == 3) { tab_ = TTools; host_.SetText(host); RunPing(host); }
                if (c == 4) SearchOnline(host);
            });
        }
    }

    void DrawTools(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        g.Text(L"Latency and DNS diagnostics", {r.x, r.y, r.w, 26}, Font::Subtitle, t.text);
        Rect in{r.x, r.y + 36, std::min(360.f, r.w - 280), 34};
        tools_.Place(app, in, L"Host name or IP address, e.g. bing.com");
        float bx = in.R() + 10;
        if (ui.Button(Id("net.ping"), {bx, in.y, 120, 34}, L"Ping", 0, ButtonStyle::Accent, !busy_)) RunPing(tools_.Text());
        if (ui.Button(Id("net.dns"), {bx + 128, in.y, 130, 34}, L"DNS lookup", 0, ButtonStyle::Standard, !busy_)) RunDns(tools_.Text());
        float y = in.B() + 20;
        if (busy_) g.Text(L"Running…", {r.x, y, 300, 22}, Font::Body, t.textSecondary);
        for (auto& line : results_) {
            g.Text(line.first, {r.x, y, 180, 22}, Font::Small, t.textSecondary);
            g.Text(line.second, {r.x + 180, y, r.w - 180, 22}, Font::Body, t.text);
            y += 24;
        }
        y += 16;
        InfoText(g, t, {r.x, y, std::min(r.w, 760.f), 80},
                 L"Ping sends 8 ICMP echo requests (1 s timeout each) and reports round-trip latency and packet loss. Many hosts and firewalls block ICMP, in which case "
                 L"loss does not necessarily mean the host is unreachable for other traffic. DNS lookup uses the system resolver and reports the time it took, "
                 L"which includes cache hits.");
    }

    void RunPing(const std::wstring& host) {
        if (Trim(host).empty() || busy_) return;
        busy_ = true;
        results_.clear();
        auto out = std::make_shared<PingResult>();
        std::wstring h = Trim(host);
        worker::Run([out, h] { *out = Ping(h, 8, 1000); }, [this, out, h] {
            busy_ = false;
            results_.clear();
            results_.push_back({L"Host", h + (out->address.empty() ? L"" : L" [" + out->address + L"]")});
            if (!out->error.empty() && out->received == 0) results_.push_back({L"Result", out->error});
            wchar_t b[128];
            swprintf(b, 128, L"%d sent, %d received, %.0f%% loss", out->sent, out->received, out->sent ? 100.0 * (out->sent - out->received) / out->sent : 0.0);
            results_.push_back({L"Packets", b});
            if (out->received) {
                swprintf(b, 128, L"min %.1f ms   avg %.1f ms   max %.1f ms", out->minMs, out->avgMs, out->maxMs);
                results_.push_back({L"Round-trip latency", b});
            }
            app.Invalidate();
        });
    }

    void RunDns(const std::wstring& host) {
        if (Trim(host).empty() || busy_) return;
        busy_ = true;
        results_.clear();
        auto out = std::make_shared<DnsResult>();
        std::wstring h = Trim(host);
        worker::Run([out, h] { *out = DnsLookup(h); }, [this, out, h] {
            busy_ = false;
            results_.clear();
            results_.push_back({L"Query", h});
            wchar_t b[64];
            swprintf(b, 64, L"%.1f ms", out->ms);
            results_.push_back({L"Resolution time", b});
            if (!out->error.empty()) results_.push_back({L"Error", out->error});
            if (!out->canonicalName.empty()) results_.push_back({L"Canonical name", out->canonicalName});
            for (size_t i = 0; i < out->addresses.size(); ++i) results_.push_back({i ? L"" : L"Addresses", out->addresses[i]});
            app.Invalidate();
        });
    }

    std::wstring ProcName(uint32_t pid) const {
        auto s = app.Snap();
        if (!s) return L"";
        if (pid == 0) return L"System Idle Process";
        const ProcSample* p = s->FindProc(pid);
        return p ? p->Name() : L"<exited>";
    }

    void LoadConnections() {
        if (loadingConn_) return;
        loadingConn_ = true;
        lastConn_ = GetTickCount64();
        auto out = std::make_shared<std::vector<Connection>>();
        bool resolve = resolve_;
        auto cache = dnsCache_;
        auto resolved = std::make_shared<std::map<std::wstring, std::wstring>>();
        worker::Run([out, resolve, cache, resolved] {
            *out = QueryConnections();
            if (!resolve) return;
            int budget = 24; // bounded reverse lookups per refresh
            for (auto& c : *out) {
                if (!c.tcp || c.remoteAddr.empty() || c.remoteAddr == L"0.0.0.0" || c.remoteAddr == L"::" || c.remoteAddr == L"127.0.0.1" || c.remoteAddr == L"::1") continue;
                if (cache.count(c.remoteAddr) || resolved->count(c.remoteAddr) || budget-- <= 0) continue;
                (*resolved)[c.remoteAddr] = ReverseLookup(c.remoteAddr);
            }
        }, [this, out, resolved] {
            loadingConn_ = false;
            for (auto& [k, v] : *resolved) dnsCache_[k] = v;
            UpdateHistory(*out);
            connections_ = std::move(*out);
            FillConnections();
            FillApps();
            app.Invalidate();
        });
    }

    void UpdateHistory(const std::vector<Connection>& now) {
        uint64_t t = NowFileTime();
        std::set<uint64_t> live;
        for (auto& c : now) {
            if (!c.tcp || c.state == MIB_TCP_STATE_LISTEN) continue;
            uint64_t k = c.Key();
            live.insert(k);
            auto& h = connHistory_[k];
            if (!h.first) { h.first = t; h.conn = c; h.name = ProcName(c.pid); }
            h.last = t;
            h.open = true;
        }
        for (auto& [k, h] : connHistory_) if (!live.count(k)) h.open = false;
        // Bound memory: keep the most recent 5000 entries.
        if (connHistory_.size() > 5000) {
            std::vector<std::pair<uint64_t, uint64_t>> v;
            for (auto& [k, h] : connHistory_) v.push_back({h.last, k});
            std::sort(v.begin(), v.end());
            for (size_t i = 0; i < v.size() - 5000; ++i) connHistory_.erase(v[i].second);
        }
        histGrid_.rows.clear();
        for (auto& [k, h] : connHistory_) {
            GridRow r;
            r.key = k;
            auto it = dnsCache_.find(h.conn.remoteAddr);
            r.cells = {h.name, std::wstring(L"TCP") + (h.conn.v6 ? L"v6" : L""), Endpoint(h.conn.remoteAddr, h.conn.remotePort, h.conn.v6),
                       it != dnsCache_.end() ? it->second : L"", Local(h.first), Local(h.last), h.open ? L"Open" : L"Closed"};
            r.nums = {NoNum(), NoNum(), NoNum(), NoNum(), static_cast<double>(h.first), static_cast<double>(h.last)};
            r.highlight = h.open ? 0 : 3;
            histGrid_.rows.push_back(std::move(r));
        }
        histGrid_.MarkDirty();
    }

    static std::wstring Local(uint64_t ft) {
        FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, l;
        FileTimeToLocalFileTime(&f, &l);
        return FormatFileTime(FileTimeToU64(l));
    }
    static std::wstring Endpoint(const std::wstring& a, uint16_t port, bool v6) { return (v6 ? L"[" + a + L"]" : a) + L":" + std::to_wstring(port); }

    void FillConnections() {
        connGrid_.rows.clear();
        for (auto& c : connections_) {
            GridRow r;
            r.key = c.Key();
            auto it = dnsCache_.find(c.remoteAddr);
            r.cells = {ProcName(c.pid), std::to_wstring(c.pid), std::wstring(c.tcp ? L"TCP" : L"UDP") + (c.v6 ? L"v6" : L""), Endpoint(c.localAddr, c.localPort, c.v6),
                       c.tcp ? Endpoint(c.remoteAddr, c.remotePort, c.v6) : L"*:*", it != dnsCache_.end() ? it->second : L"", c.tcp ? TcpStateName(c.state) : L"",
                       c.createTime ? Local(c.createTime) : L""};
            r.nums = {NoNum(), static_cast<double>(c.pid), NoNum(), static_cast<double>(c.localPort), static_cast<double>(c.remotePort), NoNum(), NoNum(),
                      static_cast<double>(c.createTime)};
            r.highlight = c.tcp && c.state == MIB_TCP_STATE_TIME_WAIT ? 3 : 0;
            connGrid_.rows.push_back(std::move(r));
        }
        connGrid_.MarkDirty();
    }

    void FillApps() {
        auto s = app.Snap();
        if (!s) return;
        std::unordered_map<uint32_t, std::pair<int, int>> counts;
        for (auto& c : connections_) {
            auto& e = counts[c.pid];
            ++e.first;
            if (c.tcp && c.state == MIB_TCP_STATE_ESTAB) ++e.second;
        }
        appsGrid_.rows.clear();
        for (auto& p : s->procs) {
            auto ci = counts.find(p.pid);
            bool traffic = p.netSend > 0 || p.netRecv > 0;
            if (ci == counts.end() && !traffic) continue;
            GridRow r;
            r.key = p.Key();
            bool m = p.netSend >= 0;
            r.cells = {p.meta ? p.meta->DisplayName() : L"", std::to_wstring(p.pid), m ? FormatNetRate(p.netSend, app.NetUnits()) : L"—",
                       m ? FormatNetRate(p.netRecv, app.NetUnits()) : L"—", m ? FormatNetRate(p.netSend + p.netRecv, app.NetUnits()) : L"—",
                       std::to_wstring(ci != counts.end() ? ci->second.first : 0), std::to_wstring(ci != counts.end() ? ci->second.second : 0)};
            r.nums = {NoNum(), static_cast<double>(p.pid), p.netSend, p.netRecv, p.netSend + p.netRecv, static_cast<double>(ci != counts.end() ? ci->second.first : 0),
                      static_cast<double>(ci != counts.end() ? ci->second.second : 0)};
            r.iconPath = p.meta ? p.meta->path : L"";
            appsGrid_.rows.push_back(std::move(r));
        }
        appsGrid_.MarkDirty();
    }

    void FillAdapters() {
        auto s = app.Snap();
        if (!s) return;
        adGrid_.rows.clear();
        auto join = [](const std::vector<std::wstring>& v) {
            std::wstring o;
            for (auto& x : v) { if (!o.empty()) o += L", "; o += x; }
            return o;
        };
        for (auto& n : s->nics) {
            const NicStatic* st = nullptr;
            for (auto& x : app.Static().nics) if (x.luid == n.luid) st = &x;
            GridRow r;
            r.key = n.luid;
            r.cells = {n.alias, n.description, n.connected ? L"Connected" : L"Disconnected", n.connected ? FormatRateBits(static_cast<double>(std::max(n.speedRx, n.speedTx))) : L"",
                       FormatBytes(n.outOctets), FormatBytes(n.inOctets), FormatCount(n.inPackets + n.outPackets), FormatCount(n.inErrors + n.outErrors),
                       FormatCount(n.inDiscards + n.outDiscards), st ? join(st->ipv4) : L"", st ? join(st->gateways) : L"", st ? join(st->dns) : L"", st ? st->mac : L"",
                       st ? std::to_wstring(st->mtu) : L""};
            r.nums = {NoNum(), NoNum(), NoNum(), static_cast<double>(n.speedRx), static_cast<double>(n.outOctets), static_cast<double>(n.inOctets),
                      static_cast<double>(n.inPackets + n.outPackets), static_cast<double>(n.inErrors + n.outErrors), static_cast<double>(n.inDiscards + n.outDiscards)};
            if (n.inErrors + n.outErrors) { r.color = app.Th().warning; r.colorCol = 7; }
            adGrid_.rows.push_back(std::move(r));
        }
        adGrid_.MarkDirty();
    }

    struct HistEntry { uint64_t first = 0, last = 0; Connection conn; std::wstring name; bool open = false; };
    int tab_ = 0;
    Table apps_, conns_, history_, adapters_;
    SimpleGrid appsGrid_, connGrid_, histGrid_, adGrid_;
    std::vector<Connection> connections_;
    std::map<uint64_t, HistEntry> connHistory_;
    std::map<std::wstring, std::wstring> dnsCache_;
    GraphView graph_;
    NativeEdit tools_;
    NativeEdit& host_ = tools_;
    std::vector<std::pair<std::wstring, std::wstring>> results_;
    std::wstring filter_;
    uint64_t lastConn_ = 0;
    bool loadingConn_ = false, resolve_ = false, busy_ = false;
};

} // namespace

std::unique_ptr<Page> MakeNetworkPage(App& app) { return std::make_unique<NetworkPage>(app); }

} // namespace nx
