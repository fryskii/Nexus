// Window chrome: custom title bar with integrated search and caption buttons, navigation sidebar,
// page header, insights rail (live metric cards, system information, quick actions) and toasts.
#include "app/App.h"
#include "ui/Graph.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace nx {

namespace {
constexpr Color kCloseRed = Color::Hex(0xC42B1C);
}

bool App::InsightsVisible(float windowWidth) const {
    const Page* p = pages_[static_cast<int>(current_)].get();
    return p && p->ShowsInsights() && settings_.GetBool("ui.insights", true) && windowWidth >= 1060.f;
}

void App::ToggleInsights() {
    settings_.SetBool("ui.insights", !settings_.GetBool("ui.insights", true));
    UpdateDemand();
    Invalidate();
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
void App::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (!sampler_ || !pages_[static_cast<int>(current_)]) { EndPaint(hwnd_, &ps); return; } // still creating
    if (!gfx_.Begin()) { EndPaint(hwnd_, &ps); return; }
    if (!ui_) ui_ = std::make_unique<Ui>(gfx_, theme_, input_);
    Ui& ui = *ui_;
    ui.BeginFrame();
    const float W = gfx_.Width(), H = gfx_.Height();
    DrawBackground(W, H);

    const float m = 12.f;
    float navW = navCollapsed_ ? 64.f : 232.f;
    Rect nav{m, titleH_, navW, H - titleH_ - m};
    bool rail = InsightsVisible(W);
    float railW = rail ? std::clamp(W * 0.23f, 286.f, 330.f) : 0.f;
    Rect content{nav.R() + m, titleH_, W - nav.R() - 2 * m - (rail ? railW + m : 0.f), H - titleH_ - m};

    // Title bar first: it positions the search box relative to the content panel.
    Rect title{0, 0, W, titleH_};
    titleDrag_ = title;
    DrawTitleBar(ui, title);
    float searchX = content.x;
    float searchW = std::clamp(capMin_.x - searchX - 48.f, 160.f, 620.f);
    Rect sr{searchX, 9, searchW, 34};
    gfx_.FillRound(sr, 10.f, theme_.searchBg);
    bool focused = GetFocus() == search_;
    gfx_.StrokeRound(sr, 10.f, focused ? theme_.accent.Alpha(0.8f) : theme_.searchBorder);
    // Magnifier drawn as geometry (no icon font dependency).
    gfx_.StrokeRound({sr.x + 13.5f, sr.y + 10.5f, 11, 11}, 5.5f, theme_.textSecondary, 1.4f);
    gfx_.Line(sr.x + 22.5f, sr.y + 19.5f, sr.x + 26.5f, sr.y + 23.5f, theme_.textSecondary, 1.4f);
    LayoutSearch({sr.x + 38, sr.y + 8, sr.w - 48, sr.h - 15}, true);

    DrawNav(ui, nav);

    // Content panel.
    gfx_.FillRound(content, 16.f, theme_.panel);
    gfx_.Line(content.x + 16, content.y + 0.5f, content.R() - 16, content.y + 0.5f, theme_.panelHighlight);
    gfx_.StrokeRound(content, 16.f, theme_.panelBorder);
    Page* page = pages_[static_cast<int>(current_)].get();
    Rect header{content.x + 20, content.y + 8, content.w - 36, 56};
    DrawHeader(ui, header);
    Rect body{content.x + 14, header.B() + 2, content.w - 28, content.B() - header.B() - 14};
    gfx_.PushClip({content.x + 1, header.B(), content.w - 2, content.B() - header.B() - 1});
    page->Draw(ui, body);
    gfx_.PopClip();
    if (rail) DrawInsights(ui, {content.R() + m, content.y, railW, content.h});
    DrawToast(content);

    ui.EndFrame();
    gfx_.End();
    EndPaint(hwnd_, &ps);
    input_.ClearEvents();
    auto d = ui.TakeDeferred();
    for (auto& f : d) deferred_.push_back(std::move(f));
    // Actions that open menus/dialogs run outside WM_PAINT.
    if (!deferred_.empty()) PostMessageW(hwnd_, WM_NX_DEFERRED, 0, 0);
    lastHot_ = ui.HitTest(input_.mx, input_.my);
}

void App::DrawBackground(float W, float H) {
    gfx_.FillGradient({0, 0, W, H}, 0, theme_.bgTop, theme_.bgBottom);
    gfx_.Glow(W * 0.20f, -H * 0.12f, W * 0.55f, H * 0.45f, theme_.glowA);
    gfx_.Glow(W * 0.98f, H * 0.10f, W * 0.30f, H * 0.40f, theme_.glowB);
    gfx_.Glow(W * 0.55f, H * 1.08f, W * 0.65f, H * 0.35f, theme_.glowA.Alpha(theme_.glowA.a * 0.55f));
}

void App::DrawTitleBar(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    g.Logo({20, 13, 26, 26}, t.accent.Mix(Color{1, 1, 1, 1}, 0.15f), Color::Hex(0x7C3AED));
    g.Text(L"NEXUS", {56, 0, 120, r.h}, Font::BodySemi, t.text);
    if (elevated_) ui.Badge({112, r.h / 2 - 10, 96, 20}, L"Administrator", t.warning.Alpha(0.16f), t.warning);
    const float bw = 46.f, bh = 34.f, top = 9.f;
    capClose_ = {r.R() - bw - 10, top, bw, bh};
    capMax_ = {capClose_.x - bw - 2, top, bw, bh};
    capMin_ = {capMax_.x - bw - 2, top, bw, bh};
    DrawCaptionButtons(r);
}

void App::DrawCaptionButtons(Rect) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    auto bg = [&](const Rect& b, int code) {
        bool hot = ncHover_ == code, down = ncPressed_ == code && hot;
        if (code == HTCLOSE && hot) g.FillRound(b, 8.f, down ? kCloseRed.Mix(Color{0, 0, 0, 1}, 0.2f) : kCloseRed);
        else if (hot) g.FillRound(b, 8.f, down ? t.surfacePressed : t.surfaceHover);
        return hot && code == HTCLOSE ? Color{1, 1, 1, 1} : t.text;
    };
    Color c = bg(capMin_, HTMINBUTTON);
    float cx = capMin_.x + capMin_.w / 2, cy = capMin_.y + capMin_.h / 2;
    g.Line(cx - 5, cy + 0.5f, cx + 5, cy + 0.5f, c, 1.f);
    c = bg(capMax_, HTMAXBUTTON);
    cx = capMax_.x + capMax_.w / 2;
    if (IsZoomed(hwnd_)) {
        g.StrokeRound({cx - 5, cy - 3, 8, 8}, 1.5f, c, 1.f);
        g.Line(cx - 2.5f, cy - 5.5f, cx + 5.5f, cy - 5.5f, c, 1.f);
        g.Line(cx + 5.5f, cy - 5.5f, cx + 5.5f, cy + 2.5f, c, 1.f);
    } else {
        g.StrokeRound({cx - 5, cy - 5, 10, 10}, 1.5f, c, 1.f);
    }
    c = bg(capClose_, HTCLOSE);
    cx = capClose_.x + capClose_.w / 2;
    g.Line(cx - 5, cy - 5, cx + 5, cy + 5, c, 1.1f);
    g.Line(cx - 5, cy + 5, cx + 5, cy - 5, c, 1.1f);
}

// ------------------------------------------------------------------------------------------
// Sidebar
// ------------------------------------------------------------------------------------------
void App::DrawNav(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    float itemH = compact_ ? 40.f : 46.f, gap = compact_ ? 3.f : 5.f;
    Rect list{r.x, r.y + 4, r.w, r.h - itemH - 16};
    float contentH = 0;
    for (const auto& e : kNavEntries)
        if (e.id != PageId::Settings && PageVisibleInNav(e.id)) contentH += itemH + gap;
    float off = navScroll_;
    ui.Scroll(Id("nav.scroll"), list, contentH, off);
    navScroll_ = off;
    auto item = [&](const NavEntry& e, Rect ir) {
        WidgetId id = IdMix(Id("nav.item"), static_cast<int>(e.id));
        bool hot = ui.Hit(id, ir);
        bool sel = current_ == e.id;
        if (sel) {
            if (t.dark) g.Glow(ir.x + ir.w * 0.25f, ir.y + ir.h / 2, ir.w * 0.55f, ir.h, t.accent.Alpha(0.08f));
            g.FillGradient(ir, 12.f, t.accent.Alpha(t.dark ? 0.42f : 0.20f), t.accent.Alpha(t.dark ? 0.12f : 0.06f), false);
            g.GlowStroke(ir, 12.f, t.accent.Alpha(t.dark ? 0.60f : 0.45f), t.dark ? 0.8f : 0.4f);
        } else if (hot) {
            g.FillRound(ir, 12.f, t.surfaceHover);
        }
        Color fg = sel ? (t.dark ? Color{1, 1, 1, 1} : t.accentText) : t.text;
        g.Glyph(e.glyph, {ir.x + 12, ir.y, 28, ir.h}, Font::IconLarge, fg);
        if (!navCollapsed_) g.Text(e.name, {ir.x + 52, ir.y, ir.w - 56, ir.h}, sel ? Font::BodySemi : Font::Body, fg);
        else ui.Tooltip(id, e.name);
        if (ui.Clicked(id)) Navigate(e.id);
        if (ui.RightClicked(id) && e.id != PageId::Settings) {
            PageId pid = e.id;
            ui.Defer([this, pid] {
                UINT c = ShowMenu({{1, L"Hide from navigation"}, {2, L"Set as start page"}, MenuItem::Sep(),
                                   {3, navCollapsed_ ? L"Expand sidebar\tCtrl+B" : L"Collapse sidebar\tCtrl+B"}});
                if (c == 1) SetPageHidden(pid, true);
                if (c == 2) settings_.SetInt("ui.startPage", static_cast<int>(pid));
                if (c == 3) { navCollapsed_ = !navCollapsed_; settings_.SetBool("ui.nav.collapsed", navCollapsed_); }
            });
        }
    };
    g.PushClip(list);
    float iy = list.y - navScroll_;
    for (const auto& e : kNavEntries) {
        if (e.id == PageId::Settings || !PageVisibleInNav(e.id)) continue;
        item(e, {r.x, iy, r.w, itemH});
        iy += itemH + gap;
    }
    g.PopClip();
    // Settings at the bottom, with the collapse toggle beside it.
    float tw = navCollapsed_ ? 0.f : 40.f;
    item(kNavEntries[static_cast<int>(PageId::Settings)], {r.x, r.B() - itemH, r.w - tw, itemH});
    if (!navCollapsed_) {
        Rect tb{r.R() - 36, r.B() - itemH + (itemH - 32) / 2, 32, 32};
        WidgetId id = Id("nav.collapse");
        bool hot = ui.Hit(id, tb);
        if (hot) g.FillRound(tb, 8.f, t.surfaceHover);
        g.Chevron(tb.x + 16, tb.y + 16, 8.f, false, t.textSecondary, 1.4f);
        ui.Tooltip(id, L"Collapse sidebar (Ctrl+B)");
        if (ui.Clicked(id)) { navCollapsed_ = true; settings_.SetBool("ui.nav.collapsed", true); }
    } else {
        Rect tb{r.x + 16, r.B() - itemH - 40, 32, 32};
        WidgetId id = Id("nav.expand");
        bool hot = ui.Hit(id, tb);
        if (hot) g.FillRound(tb, 8.f, t.surfaceHover);
        g.Chevron(tb.x + 16, tb.y + 16, 8.f, false, t.textSecondary, 1.4f);
        ui.Tooltip(id, L"Expand sidebar (Ctrl+B)");
        if (ui.Clicked(id)) { navCollapsed_ = false; settings_.SetBool("ui.nav.collapsed", false); }
    }
}

// ------------------------------------------------------------------------------------------
// Page header
// ------------------------------------------------------------------------------------------
void App::DrawHeader(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    Page* page = pages_[static_cast<int>(current_)].get();
    float titleW = g.Measure(page->Title(), Font::Title) + 4;
    narrowHeader_ = r.w < 700.f;
    g.Text(page->Title(), {r.x, r.y, titleW + 10, r.h}, Font::Title, t.text);
    const float bh = 40.f, by = r.y + (r.h - bh) / 2;
    float right = r.R();
    right -= 52;
    if (ui.Button(Id("hdr.more"), {right, by, 52, bh}, L"•••", 0)) ui.Defer([this] { OverflowMenu(); });
    ui.Tooltip(Id("hdr.more"), L"More options");
    right -= 10;
    page->HeaderActions(ui, right, by, bh);
    float runW = narrowHeader_ ? 48.f : 160.f;
    right -= runW;
    if (ui.Button(Id("hdr.run"), {right, by, runW, bh}, narrowHeader_ ? L"" : L"Run new task", 0xF001, ButtonStyle::Accent)) ui.Defer([this] { RunNewTaskDialog(); });
    if (narrowHeader_) ui.Tooltip(Id("hdr.run"), L"Run new task (Ctrl+N)");
    right -= 16;
    Rect extras{r.x + titleW + 20, r.y, right - (r.x + titleW + 20), r.h};
    if (extras.w > 250) {
        page->HeaderExtras(ui, extras);
        std::wstring info = page->HeaderInfo();
        if (!info.empty()) {
            float iw = g.Measure(info, Font::Small) + 8;
            if (iw < extras.w - 250) g.Text(info, {extras.R() - iw, r.y, iw, r.h}, Font::Small, t.textTertiary, Align::Right);
        }
    }
}

void App::OverflowMenu() {
    int ms = static_cast<int>(settings_.GetInt("refresh.ms", 1000));
    bool paused = sampler_->Paused();
    bool top = settings_.GetBool("window.topmost", false);
    MenuItem speed{0, L"Update speed"};
    speed.sub = {{11, L"High (0.5 s)", ms == 500, true, false, true},
                 {12, L"Normal (1 s)", ms == 1000, true, false, true},
                 {13, L"Low (2 s)", ms == 2000, true, false, true},
                 {14, L"Very low (4 s)", ms == 4000, true, false, true}};
    std::vector<MenuItem> items = {{1, paused ? L"Resume updates\tCtrl+P" : L"Pause updates\tCtrl+P"}, {2, L"Refresh now\tF5"}, speed, MenuItem::Sep(),
                                   {3, L"Always on top\tCtrl+T", top}, {4, L"Mini monitor\tCtrl+M", MiniMonitorVisible()},
                                   {5, L"Insights panel", settings_.GetBool("ui.insights", true), pages_[static_cast<int>(current_)]->ShowsInsights()},
                                   {6, L"Compact mode", compact_}, MenuItem::Sep()};
    std::vector<MenuItem> pageItems;
    pages_[static_cast<int>(current_)]->PageMenu(pageItems);
    for (auto& pi : pageItems) items.push_back(pi);
    if (!pageItems.empty()) items.push_back(MenuItem::Sep());
    if (!elevated_) items.push_back({7, L"Restart as administrator..."});
    items.push_back({8, L"Export diagnostic report...\tCtrl+E"});
    items.push_back({9, L"Export performance history (CSV)..."});
    items.push_back({10, L"Settings\tCtrl+,"});
    UINT c = ShowMenu(items);
    switch (c) {
    case 1:
        settings_.SetBool("refresh.paused", !paused);
        sampler_->SetPaused(!paused);
        ShowStatus(!paused ? L"Updates paused" : L"Updates resumed");
        break;
    case 2: sampler_->RefreshNow(); break;
    case 3: SetTopmost(!top); break;
    case 4: ToggleMiniMonitor(); break;
    case 5: ToggleInsights(); break;
    case 6: SetCompact(!compact_); break;
    case 7: RequestElevatedRestart(L"Some information and actions require administrator rights."); break;
    case 8: ExportReport(); break;
    case 9: ExportHistory(false); break;
    case 10: Navigate(PageId::Settings); break;
    case 11: case 12: case 13: case 14: {
        int nms[] = {500, 1000, 2000, 4000};
        settings_.SetInt("refresh.ms", nms[c - 11]);
        settings_.SetBool("refresh.paused", false);
        ApplyRefreshRate();
        UpdateDemand();
        break;
    }
    default:
        if (c >= 500 && c < 600) pages_[static_cast<int>(current_)]->OnPageMenu(c);
    }
}

// ------------------------------------------------------------------------------------------
// Insights rail
// ------------------------------------------------------------------------------------------
void App::DrawInsights(Ui& ui, Rect r) {
    Gfx& g = gfx_;
    const Theme& t = theme_;
    auto snap = snap_;
    const float gap = 10.f;
    float sysH = 116.f, quickH = 124.f;
    bool showQuick = true, showSys = true;
    auto cardH = [&] { return (r.h - (showSys ? sysH + gap : 0) - (showQuick ? quickH + gap : 0) - 4 * gap) / 5.f; };
    if (cardH() < 92) showQuick = false;
    if (cardH() < 92) showSys = false;
    float ch = std::clamp(cardH(), 70.f, 128.f);
    float y = r.y;

    struct Card { std::wstring title; MetricIcon icon; Color tint; std::wstring value, sub1, sub2; const RingBuffer<float>* data; float maxV; std::wstring device; };
    std::vector<Card> cards;
    auto& h = hist_;
    if (snap) {
        cards.push_back({L"CPU", MetricIcon::Cpu, t.cpu, FormatPercent(snap->cpu.total), snap->cpu.currentMHz > 0 ? FormatFreqMHz(snap->cpu.currentMHz) : L"", L"", &h.cpu,
                         100.f, L"cpu"});
        float mp = snap->mem.total ? 100.f * snap->mem.InUse() / snap->mem.total : 0.f;
        wchar_t mb[64];
        swprintf(mb, 64, L"%.1f / %.1f GB", snap->mem.InUse() / 1073741824.0, snap->mem.total / 1073741824.0);
        cards.push_back({L"Memory", MetricIcon::Memory, t.memory, FormatPercent(mp), mb, L"", &h.memPct, 100.f, L"mem"});
        // Disk: the system disk if known, otherwise the busiest.
        const DiskSnap* ds = nullptr;
        int sysIdx = -1;
        for (auto& d : static_.disks) if (d.isSystem) sysIdx = d.index;
        for (auto& d : snap->disks) if (d.index == sysIdx) ds = &d;
        if (!ds) for (auto& d : snap->disks) if (!ds || d.activePct > ds->activePct) ds = &d;
        if (ds) {
            std::wstring type;
            for (auto& d : static_.disks) if (d.index == ds->index) type = d.ssd ? L"SSD" : d.seekPenaltyKnown ? L"HDD" : d.busType;
            auto it = h.disks.find(ds->index);
            std::wstring title = L"Disk " + std::to_wstring(ds->index) + (ds->letters.empty() ? L"" : L" (" + ds->letters.substr(0, 2) + L")");
            cards.push_back({title, MetricIcon::Disk, t.disk, FormatPercent(ds->activePct), type, FormatRateBytes(ds->readBps + ds->writeBps),
                             it != h.disks.end() ? &it->second.active : nullptr, 100.f, L"disk:" + std::to_wstring(ds->index)});
        } else {
            cards.push_back({L"Disk", MetricIcon::Disk, t.disk, L"—", L"Collecting…", L"", nullptr, 100.f, L""});
        }
        // Network: the busiest connected adapter.
        const NicSnap* ns = nullptr;
        for (auto& n : snap->nics)
            if (n.connected && (!ns || n.sendBps + n.recvBps > ns->sendBps + ns->recvBps)) ns = &n;
        if (ns) {
            double cap = static_cast<double>(std::max(ns->speedRx, ns->speedTx));
            double util = cap > 0 ? 100.0 * (ns->sendBps + ns->recvBps) * 8 / cap : 0;
            auto it = h.nics.find(ns->luid);
            float mx = it != h.nics.end() ? std::max(1024.f, std::max(it->second.recv.Max(), it->second.send.Max()) * 1.2f) : 1024.f;
            cards.push_back({L"Network", MetricIcon::Network, t.network, FormatPercent(util), ns->wireless ? L"Wi-Fi" : L"Ethernet",
                             FormatNetRate(ns->sendBps + ns->recvBps, NetUnits()), it != h.nics.end() ? &it->second.recv : nullptr, mx,
                             L"nic:" + std::to_wstring(ns->luid)});
        } else {
            cards.push_back({L"Network", MetricIcon::Network, t.network, L"—", L"Not connected", L"", nullptr, 1.f, L""});
        }
        const GpuSnap* gs = nullptr;
        for (auto& gp : snap->gpus) if (!gs || gp.util > gs->util) gs = &gp;
        if (gs) {
            auto it = h.gpus.find(gs->luid);
            cards.push_back({L"GPU", MetricIcon::Gpu, t.gpu, FormatPercent(gs->util), gs->name, gs->temperatureC > 0 ? FormatTemp(gs->temperatureC, TempUnits()) : L"",
                             it != h.gpus.end() ? &it->second.util : nullptr, 100.f, L"gpu:" + std::to_wstring(gs->luid)});
        } else {
            cards.push_back({L"GPU", MetricIcon::Gpu, t.gpu, L"—", snap->gpuCollected ? L"No GPU detected" : L"Collecting…", L"", nullptr, 100.f, L""});
        }
    }
    for (size_t i = 0; i < cards.size(); ++i) {
        const Card& c = cards[i];
        Rect cr{r.x, y, r.w, ch};
        y += ch + gap;
        WidgetId id = IdMix(Id("rail.card"), i);
        bool hot = ui.Hit(id, cr);
        ui.Panel(cr, hot ? c.tint.Mix(Color{1, 1, 1, 1}, 0.15f) : c.tint, true);
        // Graph first so the text sits on top of it.
        if (c.data && c.data->Size() > 1) {
            static thread_local std::vector<float> buf;
            c.data->Tail(c.data->Size(), buf);
            Rect gr{cr.x + 8, cr.y + ch * 0.46f, cr.w - 16, ch * 0.54f - 8};
            g.PushClip(cr.Inset(1));
            g.SeriesGradient(gr, buf.data(), buf.size(), c.data->Capacity(), c.maxV, c.tint.Mix(Color{1, 1, 1, 1}, t.dark ? 0.15f : 0.f),
                             c.tint.Alpha(t.dark ? 0.45f : 0.25f), 1.4f);
            g.PopClip();
        }
        float tile = std::min(38.f, ch * 0.34f);
        g.IconTile(c.icon, {cr.x + 12, cr.y + 12, tile, tile}, c.tint, t.dark);
        float tx = cr.x + 12 + tile + 14;
        g.Text(c.title, {tx, cr.y + 10, cr.R() - tx - 30, 24}, Font::Subtitle, t.text);
        g.Chevron(cr.R() - 18, cr.y + 22, 8.f, false, t.textSecondary, 1.4f);
        if (ch >= 84) {
            g.Text(c.value, {tx, cr.y + 34, cr.R() - tx - 12, 32}, Font::Display, t.text);
            float sy = cr.y + 66;
            if (!c.sub1.empty() && sy + 16 < cr.B()) { g.Text(c.sub1, {tx, sy, cr.R() - tx - 12, 16}, Font::Small, t.textSecondary); sy += 16; }
            if (!c.sub2.empty() && sy + 16 < cr.B()) g.Text(c.sub2, {tx, sy, cr.R() - tx - 12, 16}, Font::Small, t.textSecondary);
        } else {
            g.Text(c.value, {cr.R() - 120, cr.y + 8, 92, 28}, Font::Subtitle, t.text, Align::Right);
        }
        if (ui.Clicked(id) && !c.device.empty()) {
            settings_.SetString("performance.device", c.device);
            ui.Defer([this] { Navigate(PageId::Performance); });
        }
        ui.Tooltip(id, L"Open in Performance");
    }
    if (showSys && snap) {
        Rect sc{r.x, y, r.w, sysH};
        y += sysH + gap;
        ui.Card(sc);
        g.DrawIcon(MetricIcon::Info, {sc.x + 14, sc.y + 12, 18, 18}, t.text, 1.3f);
        g.Text(L"System information", {sc.x + 40, sc.y + 8, sc.w - 50, 26}, Font::BodySemi, t.text);
        float ly = sc.y + 36;
        auto line = [&](const wchar_t* k, const std::wstring& v) {
            g.Text(k, {sc.x + 16, ly, sc.w / 2, 19}, Font::Small, t.textSecondary);
            g.Text(v, {sc.x + sc.w / 2, ly, sc.w / 2 - 16, 19}, Font::Small, t.text, Align::Right);
            ly += 19;
        };
        line(L"Processes", FormatCount(snap->cpu.processes));
        line(L"Threads", FormatCount(snap->cpu.threads));
        line(L"Handles", FormatCount(snap->cpu.handles));
        line(L"Uptime", FormatUptime(snap->cpu.uptimeSec));
    }
    if (showQuick) {
        Rect qc{r.x, y, r.w, quickH};
        ui.Card(qc);
        g.DrawIcon(MetricIcon::Bolt, {qc.x + 14, qc.y + 12, 18, 18}, t.text);
        g.Text(L"Quick actions", {qc.x + 40, qc.y + 8, qc.w - 50, 26}, Font::BodySemi, t.text);
        float ly = qc.y + 36;
        auto row = [&](const char* key, const wchar_t* label, const wchar_t* tip, bool value, int icon) {
            Rect rr{qc.x + 14, ly, qc.w - 28, 21};
            if (icon == 0) g.Leaf(rr.x, ly + 4, 13.f, t.textSecondary);
            else if (icon == 1) { g.StrokeRound({rr.x + 1, ly + 4, 12, 12}, 6.f, t.textSecondary, 1.2f); g.FillRound({rr.x + 7, ly + 4, 6, 12}, 3.f, t.textSecondary); }
            else if (icon == 2) { g.Line(rr.x + 7, ly + 3, rr.x + 7, ly + 18, t.textSecondary, 1.2f); g.StrokeRound({rr.x + 2, ly + 3, 10, 7}, 2.f, t.textSecondary, 1.2f); }
            else g.DrawIcon(MetricIcon::Info, {rr.x, ly + 3, 14, 14}, t.textSecondary, 1.2f);
            g.Text(label, {rr.x + 24, ly, rr.w - 80, 21}, Font::Small, t.text);
            bool v = value;
            WidgetId id = Id(key);
            bool changed = ui.Toggle(id, {rr.R() - 44, ly, 44, 21}, v);
            ui.Tooltip(id, tip);
            ly += 21;
            return changed;
        };
        if (row("qa.eff", L"Efficiency mode", L"Run NEXUS itself in efficiency mode: EcoQoS scheduling and slower updates (at least 2 s).", SelfEfficiency(), 0))
            SetSelfEfficiency(!SelfEfficiency());
        if (row("qa.dark", L"Dark mode", L"Switch between dark and light themes.", t.dark, 1)) {
            settings_.SetString("ui.theme", t.dark ? L"light" : L"dark");
            ui.Defer([this] { ApplyTheme(); });
        }
        bool top = settings_.GetBool("window.topmost", false);
        if (row("qa.top", L"Always on top", L"Keep NEXUS above other windows (Ctrl+T).", top, 2)) SetTopmost(!top);
        if (row("qa.details", L"Show details", L"Detailed layout with taller rows; turn off for compact mode.", !compact_, 3)) SetCompact(!compact_);
    }
}

void App::DrawToast(Rect content) {
    if (status_.empty()) return;
    Gfx& g = gfx_;
    const Theme& t = theme_;
    float w = std::min(content.w - 40, g.Measure(status_, Font::Body) + 36);
    Rect tr{content.x + (content.w - w) / 2, content.B() - 54, w, 38};
    g.FillRound(tr, 12.f, t.tooltipBg);
    g.StrokeRound(tr, 12.f, statusError_ ? t.critical.Alpha(0.7f) : t.accent.Alpha(0.5f));
    g.Text(status_, tr.Inset(18, 0), Font::Body, statusError_ ? t.critical : t.text, Align::Center);
}

void App::LayoutSearch(const Rect& r, bool visible) {
    if (!search_) return;
    if (!visible) {
        if (IsWindowVisible(search_)) ShowWindow(search_, SW_HIDE);
        return;
    }
    float s = gfx_.Scale();
    RECT want{static_cast<LONG>(r.x * s), static_cast<LONG>(r.y * s), static_cast<LONG>(r.R() * s), static_cast<LONG>(r.B() * s)};
    RECT cur;
    GetWindowRect(search_, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    if (memcmp(&cur, &want, sizeof cur) != 0 || !IsWindowVisible(search_))
        SetWindowPos(search_, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

// ------------------------------------------------------------------------------------------
// Non-client handling for the custom title bar
// ------------------------------------------------------------------------------------------
LRESULT App::OnNcCalcSize(WPARAM wp, LPARAM lp) {
    if (!wp) return DefWindowProcW(hwnd_, WM_NCCALCSIZE, wp, lp);
    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
    LONG top = p->rgrc[0].top;
    // Let Windows compute the side and bottom resize borders, then reclaim the caption area.
    LRESULT r = DefWindowProcW(hwnd_, WM_NCCALCSIZE, wp, lp);
    p->rgrc[0].top = top;
    if (IsZoomed(hwnd_)) {
        UINT dpi = GetDpiForWindow(hwnd_);
        p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    }
    return r;
}

LRESULT App::OnNcHitTest(LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    if (!IsZoomed(hwnd_) && pt.y >= 0 && pt.y < frame) {
        if (pt.x < frame * 2) return HTTOPLEFT;
        if (pt.x > rc.right - frame * 2) return HTTOPRIGHT;
        return HTTOP;
    }
    float s = gfx_.Scale();
    float x = pt.x / s, y = pt.y / s;
    if (capClose_.Contains(x, y)) return HTCLOSE;
    if (capMax_.Contains(x, y)) return HTMAXBUTTON; // also enables Windows 11 snap layouts
    if (capMin_.Contains(x, y)) return HTMINBUTTON;
    if (y < titleH_) return HTCAPTION;
    return HTCLIENT;
}

bool App::OnNcMouse(UINT msg, WPARAM wp, LPARAM) {
    int code = static_cast<int>(wp);
    bool button = code == HTMINBUTTON || code == HTMAXBUTTON || code == HTCLOSE;
    switch (msg) {
    case WM_NCMOUSEMOVE: {
        int hover = button ? code : 0;
        if (hover != ncHover_) { ncHover_ = hover; Invalidate(); }
        if (!ncTracking_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            TrackMouseEvent(&tme);
            ncTracking_ = true;
        }
        return false;
    }
    case WM_NCMOUSELEAVE:
        ncTracking_ = false;
        if (ncHover_ || ncPressed_) { ncHover_ = ncPressed_ = 0; Invalidate(); }
        return false;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        if (!button) return false;
        ncPressed_ = code;
        Invalidate();
        return true; // suppress the classic caption-button tracking
    case WM_NCLBUTTONUP:
        if (!button) return false;
        if (ncPressed_ == code) {
            ncPressed_ = ncHover_ = 0;
            if (code == HTMINBUTTON) ShowWindow(hwnd_, SW_MINIMIZE);
            else if (code == HTMAXBUTTON) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        ncPressed_ = 0;
        Invalidate();
        return true;
    }
    return false;
}

} // namespace nx
