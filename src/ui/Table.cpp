#include "ui/Table.h"
#include "core/Format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nx {

void Table::EnsureOrder() {
    if (order_.size() == columns.size()) return;
    order_.clear();
    for (size_t i = 0; i < columns.size(); ++i) order_.push_back(static_cast<int>(i));
}

int Table::ColumnIndex(const std::string& key) const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].key == key) return static_cast<int>(i);
    return -1;
}

const std::string& Table::SortKey() const {
    static const std::string none;
    return sortCol >= 0 && sortCol < static_cast<int>(columns.size()) ? columns[sortCol].key : none;
}

int Table::SelectedRow(const TableSource& src) const {
    if (!selectedKey) return -1;
    size_t n = src.RowCount();
    for (size_t i = 0; i < n; ++i)
        if (src.RowKey(i) == selectedKey) return static_cast<int>(i);
    return -1;
}

void Table::SelectRow(size_t row, const TableSource& src) {
    if (row >= src.RowCount()) return;
    uint64_t k = src.RowKey(row);
    if (k != selectedKey) { selectedKey = k; selectionChanged = true; }
    ensureVisible_ = true;
}

bool Table::OnKey(UINT vk, TableSource& src) {
    size_t n = src.RowCount();
    if (!n) return false;
    int cur = SelectedRow(src);
    int page = std::max(1, static_cast<int>(viewH_ / RowHeight()) - 1);
    int next = cur;
    switch (vk) {
    case VK_UP: next = cur <= 0 ? 0 : cur - 1; break;
    case VK_DOWN: next = cur < 0 ? 0 : std::min<int>(static_cast<int>(n) - 1, cur + 1); break;
    case VK_PRIOR: next = std::max(0, cur - page); break;
    case VK_NEXT: next = std::min<int>(static_cast<int>(n) - 1, (cur < 0 ? 0 : cur) + page); break;
    case VK_HOME: next = 0; break;
    case VK_END: next = static_cast<int>(n) - 1; break;
    case VK_LEFT:
        if (cur >= 0 && src.RowExpander(cur) == 2) { src.ToggleExpand(cur); return true; }
        return false;
    case VK_RIGHT:
        if (cur >= 0 && src.RowExpander(cur) == 1) { src.ToggleExpand(cur); return true; }
        return false;
    case VK_RETURN:
        if (cur >= 0) { activated = true; return true; }
        return false;
    default: return false;
    }
    if (next >= 0) SelectRow(static_cast<size_t>(next), src);
    return true;
}

bool Table::HasPills() const {
    return std::any_of(columns.begin(), columns.end(), [](const TableColumn& c) { return c.visible && c.icon != MetricIcon::None && !c.header2.empty(); });
}

// Narrowest width at which the header title still fits in the smaller header font: on one line, or
// in the tall (pill) header on two lines split at the best space. Plus box inset, text padding and,
// for the sorted column, the arrow.
float Table::MinWidth(Gfx& g, const TableColumn& c, bool sorted, bool tall) {
    if (c.icon != MetricIcon::None) return 84.f;
    auto it = titleMinW_.find(c.title);
    if (it == titleMinW_.end()) {
        float one = g.Measure(c.title, Font::Caption), two = one;
        for (size_t sp = c.title.find(L' '); sp != std::wstring::npos; sp = c.title.find(L' ', sp + 1))
            two = std::min(two, std::max(g.Measure(c.title.substr(0, sp), Font::Caption), g.Measure(c.title.substr(sp + 1), Font::Caption)));
        it = titleMinW_.emplace(c.title, std::make_pair(one, two)).first;
    }
    float text = tall ? it->second.second : it->second.first;
    return std::max(40.f, std::ceil(text) + 6 + 12 + (sorted ? 12 : 0) + 1);
}

void Table::HeaderTitle(Gfx& g, const Theme& t, int ci, const Rect& r, bool sorted, bool tall) {
    const TableColumn& c = columns[ci];
    if (headerFit_.size() != columns.size()) headerFit_.assign(columns.size(), {});
    HeaderFit& fit = headerFit_[ci];
    if (fit.w != r.w || fit.sorted != sorted || fit.tall != tall || fit.title != c.title) {
        // Largest font that fits on one line; otherwise (tall header) two lines split at the space
        // that balances them best; otherwise the small font, cut off.
        fit = {c.title, {c.title, L""}, r.w, 0, sorted, tall, Font::Caption};
        for (Font cand : {sorted ? Font::SmallSemi : Font::Small, Font::Caption}) {
            float one = g.Measure(c.title, cand);
            if (one <= r.w) { fit.font = cand; fit.textW = one; break; }
            if (!tall) continue;
            float best = 1e9f;
            for (size_t sp = c.title.find(L' '); sp != std::wstring::npos; sp = c.title.find(L' ', sp + 1)) {
                std::wstring a = c.title.substr(0, sp), b = c.title.substr(sp + 1);
                float w = std::max(g.Measure(a, cand), g.Measure(b, cand));
                if (w <= r.w && w < best) { best = w; fit.lines[0] = a; fit.lines[1] = b; }
            }
            if (best < 1e9f) { fit.font = cand; fit.textW = best; break; }
        }
        if (fit.textW == 0) fit.textW = r.w; // nothing fits: cut off in the small font
    }
    Color col = sorted ? t.text : t.textSecondary;
    if (fit.lines[1].empty()) {
        g.Text(fit.lines[0], r, fit.font, col, c.align);
    } else {
        float lh = fit.font == Font::Caption ? 14.f : 15.f;
        float y = r.y + (r.h - 2 * lh) / 2;
        for (auto& l : fit.lines) {
            g.Text(l, {r.x, y, r.w, lh}, fit.font, col, c.align);
            y += lh;
        }
    }
    if (sorted)
        g.Chevron(c.align == Align::Right ? r.x - 2 : std::min(r.R() + 2, r.x + fit.textW + 10), r.y + r.h / 2, 7.f, true, t.textTertiary, 1.2f);
}

void Table::Draw(Ui& ui, const Rect& r, TableSource& src) {
    EnsureOrder();
    bool tall = HasPills();
    auto soft = [&](int ci) { return MinWidth(ui.g, columns[ci], ci == sortCol, tall); };
    // Keep every header title readable at the saved width.
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].visible) columns[i].width = std::max(columns[i].width, soft(static_cast<int>(i)));
    if (!fillFirst) { DrawImpl(ui, r, src); return; }
    // When the panel is too narrow, squeeze the other columns for this frame only so the saved
    // widths survive a temporary resize. A width changed by the user during the frame is kept.
    // First down to where both the title and typical values fit (64 px keeps values such as PIDs
    // readable), then (too many columns for the window) wide titles give way down to 64 px, and as
    // a last resort everything shrinks further, so no column is pushed out of view.
    int firstCol = -1;
    float others = 0;
    for (int ci : order_) {
        if (!columns[ci].visible) continue;
        if (firstCol < 0) { firstCol = ci; continue; }
        others += columns[ci].width;
    }
    float excess = others - (r.w - 16.f - 4.f - 160.f);
    if (firstCol < 0 || excess <= 0) { DrawImpl(ui, r, src); return; }
    std::vector<std::pair<float, float>> saved(columns.size()); // original, squeezed
    for (size_t i = 0; i < columns.size(); ++i) saved[i] = {columns[i].width, columns[i].width};
    auto squeeze = [&](auto floorOf) {
        float shrinkable = 0;
        for (int ci : order_)
            if (columns[ci].visible && ci != firstCol) shrinkable += std::max(0.f, columns[ci].width - floorOf(ci));
        if (excess <= 0 || shrinkable <= 0) return;
        float k = std::min(1.f, excess / shrinkable);
        for (int ci : order_) {
            if (!columns[ci].visible || ci == firstCol) continue;
            float cut = std::max(0.f, columns[ci].width - floorOf(ci)) * k;
            columns[ci].width -= cut;
            excess -= cut;
        }
    };
    auto plainFloor = [&](int ci, float plain) { return columns[ci].icon != MetricIcon::None ? 84.f : plain; };
    squeeze([&](int ci) { return std::max(soft(ci), plainFloor(ci, 64.f)); });
    squeeze([&](int ci) { return plainFloor(ci, 64.f); });
    squeeze([&](int ci) { return plainFloor(ci, 40.f); });
    for (size_t i = 0; i < columns.size(); ++i) saved[i].second = columns[i].width;
    DrawImpl(ui, r, src);
    for (size_t i = 0; i < columns.size(); ++i)
        if (static_cast<int>(i) != firstCol && columns[i].width == saved[i].second) columns[i].width = saved[i].first;
}

void Table::DrawImpl(Ui& ui, const Rect& r, TableSource& src) {
    Gfx& g = ui.g;
    const Theme& t = ui.t;
    bool pills = HasPills();
    const float headerH = columns.empty() ? 0 : (pills ? 66.f : 36.f);
    const float gap = 8.f;
    const float rowH = RowHeight();
    Rect header{r.x, r.y, r.w, headerH};
    Rect body{r.x, r.y + headerH + gap, r.w, r.h - headerH - gap};
    viewH_ = body.h;
    const float pad = 8.f; // inner padding of the panels
    // The first visible column (Name) flexes so the columns always fill the panel width.
    if (fillFirst) {
        int firstCol = -1;
        float others = 0;
        for (int ci : order_) {
            if (!columns[ci].visible) continue;
            if (firstCol < 0) firstCol = ci;
            else others += columns[ci].width;
        }
        if (firstCol >= 0) columns[firstCol].width = std::max(160.f, r.w - 2 * pad - others - 4);
    }

    // ---------------- Header ----------------
    g.FillRound(header, 12.f, t.panel);
    g.StrokeRound(header, 12.f, t.panelBorder);
    g.PushClip(header);
    float x = r.x + pad;
    for (size_t di = 0; di < order_.size(); ++di) {
        int ci = order_[di];
        auto& c = columns[ci];
        if (!c.visible) continue;
        Rect cr{x, header.y, c.width, headerH};
        WidgetId hid = IdMix(id_, 1000 + ci);
        WidgetId gid = IdMix(id_, 5000 + ci);
        Rect grip{cr.R() - 4, cr.y, 8, cr.h};
        bool gripHot = ui.Hit(gid, grip);
        bool hot = ui.Hit(hid, {cr.x, cr.y, cr.w - 4, cr.h});
        if (ui.Pressed(gid)) { resizing_ = ci; resizeStartX_ = ui.in.mx; resizeStartW_ = c.width; ui.SetActive(gid); }
        if (ui.Pressed(hid)) { dragging_ = static_cast<int>(di); dragStartX_ = ui.in.mx; }
        bool sorted = sortCol == ci;
        if (c.icon != MetricIcon::None && !c.header2.empty() && pills) {
            // Metric pill: icon tile, total value and label.
            Rect pr{cr.x + 3, cr.y + 7, cr.w - 6, cr.h - 14};
            Color tint = c.tint.a > 0 ? c.tint : t.accent;
            g.FillGradient(pr, 12.f, t.surfaceAlt.Mix(tint, t.dark ? 0.10f : 0.04f), t.surface);
            g.StrokeRound(pr, 12.f, sorted ? tint.Alpha(0.75f) : hot && !gripHot ? tint.Alpha(0.5f) : t.panelBorder);
            float tile = std::min(30.f, pr.h - 18);
            bool showTile = pr.w >= 96;
            Rect tr{pr.x + 9, pr.y + (pr.h - tile) / 2, tile, tile};
            if (showTile) g.IconTile(c.icon, tr, tint, t.dark);
            float tx = showTile ? tr.R() + 8 : pr.x + 8;
            Rect vr{tx, pr.y + 5, pr.R() - tx - 8, pr.h * 0.55f};
            g.Text(c.header2, vr, Font::StatValue, t.text, showTile ? Align::Center : Align::Right);
            g.Text(c.title, {tx, vr.B() - 2, vr.w, pr.h * 0.40f}, sorted ? Font::SmallSemi : Font::Caption, sorted ? t.text : t.textSecondary,
                   showTile ? Align::Center : Align::Right);
        } else {
            // Plain column: the same panel as the metric pills, with the title only.
            float inset = pills ? 7.f : 4.f, rad = pills ? 12.f : 8.f;
            Rect pr{cr.x + 3, cr.y + inset, cr.w - 6, cr.h - 2 * inset};
            g.FillGradient(pr, rad, t.surfaceAlt.Mix(t.accent, t.dark ? 0.06f : 0.02f), t.surface);
            g.StrokeRound(pr, rad, sorted ? t.accent.Alpha(0.75f) : hot && !gripHot ? t.accent.Alpha(0.5f) : t.panelBorder);
            Rect tr{pr.x + 6, pr.y, pr.w - 12, pr.h};
            if (sorted) { // room for the chevron
                tr.w -= 12;
                if (c.align == Align::Right) tr.x += 12;
            }
            HeaderTitle(g, t, ci, tr, sorted, pills);
        }
        if (ui.Clicked(hid) && std::fabs(ui.in.mx - dragStartX_) < 4) {
            if (sortCol == ci) sortDesc = !sortDesc;
            else { sortCol = ci; sortDesc = c.align == Align::Right; } // numbers default to descending
            sortChanged = true;
            ui.RequestFrame(); // pages re-sort before drawing, so show the new order now, not at the next sample
        }
        if (ui.RightClicked(hid) || ui.RightClicked(gid)) { headerMenu = true; headerMenuColumn = ci; }
        if (!c.tooltip.empty()) ui.Tooltip(hid, c.tooltip);
        if (gripHot || resizing_ == ci) g.Line(cr.R() - 0.5f, cr.y + 10, cr.R() - 0.5f, cr.B() - 10, t.accent.Alpha(0.6f));
        x += c.width;
    }
    // Column resize / reorder dragging.
    if (resizing_ >= 0) {
        if (ui.in.lDown) columns[resizing_].width = std::max(MinWidth(g, columns[resizing_], resizing_ == sortCol, pills), resizeStartW_ + (ui.in.mx - resizeStartX_));
        else resizing_ = -1;
    }
    if (dragging_ >= 0) {
        if (!ui.in.lDown) {
            float dx = ui.in.mx - dragStartX_;
            if (std::fabs(dx) > 24) {
                float xx = r.x + pad;
                int target = dragging_;
                for (size_t di = 0; di < order_.size(); ++di) {
                    auto& c = columns[order_[di]];
                    if (!c.visible) continue;
                    if (ui.in.mx >= xx && ui.in.mx < xx + c.width) { target = static_cast<int>(di); break; }
                    xx += c.width;
                }
                if (target != dragging_) {
                    int col = order_[dragging_];
                    order_.erase(order_.begin() + dragging_);
                    order_.insert(order_.begin() + target, col);
                }
            }
            dragging_ = -1;
        }
    }
    g.PopClip();

    // ---------------- Body ----------------
    size_t n = src.RowCount();
    float contentH = n * rowH + 4;
    int sel = SelectedRow(src);
    if (ensureVisible_ && sel >= 0) {
        float top = sel * rowH;
        if (top < scrollY) scrollY = top;
        else if (top + rowH > scrollY + body.h) scrollY = top + rowH - body.h;
        ensureVisible_ = false;
    }
    WidgetId bodyId = IdMix(id_, 2);
    ui.Hit(bodyId, body);
    // Only take the wheel when there is something to scroll, so enclosing views can scroll.
    if (contentH > body.h) {
        float wh = ui.Wheel(body);
        if (wh != 0) scrollY -= wh * rowH * 3;
    }
    scrollY = std::clamp(scrollY, 0.f, std::max(0.f, contentH - body.h));

    g.PushClip(body);
    // Rounded panels: one per group (from its header to the row before the next header), or one for all rows.
    auto rowY = [&](size_t row) { return body.y + row * rowH - scrollY; };
    std::vector<std::pair<size_t, size_t>> panels;
    for (size_t i = 0; i < n; ++i) {
        if (src.RowIsGroup(i) || panels.empty()) {
            if (!panels.empty()) panels.back().second = i;
            panels.push_back({i, n});
        }
    }
    for (auto& [a, b] : panels) {
        float top = rowY(a) + (a ? 3.f : 0.f), bottom = rowY(b) - 3.f;
        if (bottom < body.y || top > body.B()) continue;
        Rect pr{r.x, top, r.w, bottom - top};
        g.FillRound(pr, 12.f, t.panel);
        g.StrokeRound(pr, 12.f, t.panelBorder);
    }
    size_t first = static_cast<size_t>(scrollY / rowH);
    size_t last = std::min(n, first + static_cast<size_t>(body.h / rowH) + 2);
    for (size_t row = first; row < last; ++row) {
        float y = rowY(row);
        Rect rr{r.x + 6, y, r.w - 12, rowH};
        WidgetId rid = IdMix(id_, 100000 + src.RowKey(row));
        bool hot = ui.Hit(rid, rr);
        bool isSel = static_cast<int>(row) == sel;
        if (src.RowIsGroup(row)) {
            std::wstring label;
            src.CellText(row, 0, label);
            bool open = src.RowExpander(row) != 1;
            float cy = y + rowH / 2 + 2;
            if (hot) g.FillRound(rr.Inset(2, 4), 8.f, t.hover);
            g.Chevron(r.x + pad + 14, cy, 8.f, open, t.textSecondary, 1.5f);
            g.Text(label, {r.x + pad + 34, y + 2, r.w - 60, rowH}, Font::Subtitle, t.text);
            if (ui.Clicked(rid)) src.ToggleExpand(row);
            continue;
        }
        int hl = src.RowHighlight(row);
        if (hl == 1) g.FillRound(rr.Inset(2, 1), 8.f, t.good.Alpha(0.14f));
        else if (hl == 2) g.FillRound(rr.Inset(2, 1), 8.f, t.critical.Alpha(0.16f));
        if (isSel) {
            g.FillRound(rr.Inset(2, 1), 8.f, t.selection);
            g.StrokeRound(rr.Inset(2, 1), 8.f, t.selectionBorder);
        } else if (hot) {
            g.FillRound(rr.Inset(2, 1), 8.f, t.hover);
        }
        Color textC = hl == 3 ? t.textTertiary : t.text;

        float cx = r.x + pad;
        bool firstVisible = true;
        for (int ci : order_) {
            auto& c = columns[ci];
            if (!c.visible) continue;
            Rect cr{cx, y, c.width, rowH};
            if (heatMap && c.heat) {
                float h = src.CellHeat(row, ci);
                if (h >= 0 && (c.heatAlways || h > 0.02f)) g.FillRound(cr.Inset(3, 3), 7.f, t.Heat(h));
            }
            src.CellText(row, ci, cell_);
            Rect tr = cr.Inset(10, 0);
            if (firstVisible) {
                int depth = src.RowDepth(row);
                float indent = depth * 20.f;
                tr.x += indent;
                tr.w -= indent;
                int ex = src.RowExpander(row);
                if (ex) {
                    Rect er{tr.x - 4, y + 3, 20, rowH - 6};
                    WidgetId eid = IdMix(id_, 900000 + src.RowKey(row));
                    bool eh = ui.Hit(eid, er);
                    if (eh) g.FillRound(er, 5.f, t.surfaceHover);
                    g.Chevron(er.x + 10, y + rowH / 2, 8.f, ex == 2, t.textSecondary, 1.4f);
                    if (ui.Clicked(eid)) src.ToggleExpand(row);
                }
                tr.x += 22;
                tr.w -= 22;
                if (showIcons) {
                    float is = compact ? 18.f : 22.f;
                    if (ID2D1Bitmap* icon = src.RowIcon(row)) g.DrawBitmap(icon, {tr.x, y + (rowH - is) / 2, is, is});
                    tr.x += is + 10;
                    tr.w -= is + 10;
                }
                firstVisible = false;
            }
            if (src.CellBadge(row, ci) == 1) {
                g.Leaf(tr.x, y + rowH / 2 - 7, 14.f, t.good);
                tr.x += 22;
                tr.w -= 22;
            }
            Color cc = src.CellColor(row, ci, t);
            g.Text(cell_, tr, Font::Body, cc.a > 0 ? cc : textC, c.align);
            cx += c.width;
        }
        if (ui.Pressed(rid) || (ui.in.rPressed && hot)) {
            if (src.RowKey(row) != selectedKey) { selectedKey = src.RowKey(row); selectionChanged = true; }
        }
        if (ui.DoubleClicked(rid)) activated = true;
        if (ui.RightClicked(rid)) {
            if (src.RowKey(row) != selectedKey) { selectedKey = src.RowKey(row); selectionChanged = true; }
            contextMenu = true;
        }
    }
    g.PopClip();
    if (n == 0) g.Text(L"No items", body, Font::Body, t.textTertiary, Align::Center);

    // Scrollbar.
    if (contentH > body.h) {
        float trackH = body.h - 12;
        float thumbH = std::max(28.f, trackH * body.h / contentH);
        float thumbY = body.y + 6 + (trackH - thumbH) * scrollY / (contentH - body.h);
        WidgetId sid = IdMix(id_, 3);
        Rect hitR{body.R() - 14, body.y, 14, body.h};
        bool hot = ui.Hit(sid, hitR);
        if (ui.Pressed(sid)) { sbDragY_ = ui.in.my; sbDragOff_ = scrollY; }
        if (ui.IsActive(sid) && ui.in.lDown) scrollY = std::clamp(sbDragOff_ + (ui.in.my - sbDragY_) * contentH / body.h, 0.f, contentH - body.h);
        g.FillRound({body.R() - (hot ? 9.f : 7.f), thumbY, hot ? 6.f : 4.f, thumbH}, 3.f, t.textTertiary.Alpha(hot ? 0.8f : 0.45f));
    }
}

std::wstring Table::SaveLayout() const {
    // "key:width:visible,...|sortKey|desc"
    std::wstring s;
    std::vector<int> ord = order_;
    if (ord.size() != columns.size()) { ord.clear(); for (size_t i = 0; i < columns.size(); ++i) ord.push_back(static_cast<int>(i)); }
    for (int ci : ord) {
        auto& c = columns[ci];
        if (!s.empty()) s += L',';
        s += FromUtf8(c.key) + L':' + std::to_wstring(static_cast<int>(c.width)) + L':' + (c.visible ? L'1' : L'0');
    }
    s += L'|' + (sortCol >= 0 ? FromUtf8(columns[sortCol].key) : L"") + L'|' + (sortDesc ? L'1' : L'0');
    return s;
}

void Table::LoadLayout(const std::wstring& s) {
    if (s.empty()) return;
    size_t bar = s.find(L'|');
    std::wstring cols = s.substr(0, bar);
    std::vector<int> ord;
    size_t p = 0;
    while (p < cols.size()) {
        size_t comma = cols.find(L',', p);
        if (comma == std::wstring::npos) comma = cols.size();
        std::wstring item = cols.substr(p, comma - p);
        p = comma + 1;
        size_t c1 = item.find(L':'), c2 = item.rfind(L':');
        if (c1 == std::wstring::npos || c2 == c1) continue;
        int ci = ColumnIndex(ToUtf8(item.substr(0, c1)));
        if (ci < 0) continue;
        columns[ci].width = std::clamp(static_cast<float>(_wtoi(item.c_str() + c1 + 1)), 36.f, 2000.f);
        columns[ci].visible = item[c2 + 1] == L'1';
        if (std::find(ord.begin(), ord.end(), ci) == ord.end()) ord.push_back(ci);
    }
    for (size_t i = 0; i < columns.size(); ++i)
        if (std::find(ord.begin(), ord.end(), static_cast<int>(i)) == ord.end()) ord.push_back(static_cast<int>(i));
    order_ = ord;
    if (bar != std::wstring::npos) {
        size_t bar2 = s.find(L'|', bar + 1);
        std::wstring sk = s.substr(bar + 1, bar2 == std::wstring::npos ? std::wstring::npos : bar2 - bar - 1);
        sortCol = sk.empty() ? -1 : ColumnIndex(ToUtf8(sk));
        if (bar2 != std::wstring::npos && bar2 + 1 < s.size()) sortDesc = s[bar2 + 1] == L'1';
    }
}

} // namespace nx
