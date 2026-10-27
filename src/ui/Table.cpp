#include "ui/Table.h"
#include "core/Format.h"

#include <algorithm>
#include <cmath>

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

void Table::Draw(Ui& ui, const Rect& r, TableSource& src) {
    EnsureOrder();
    Gfx& g = ui.g;
    const Theme& t = ui.t;
    const float headerH = columns.empty() ? 0 : (std::any_of(columns.begin(), columns.end(), [](const TableColumn& c) { return !c.header2.empty(); }) ? 46.f : 32.f);
    const float rowH = RowHeight();
    Rect header{r.x, r.y, r.w, headerH};
    Rect body{r.x, r.y + headerH, r.w, r.h - headerH};
    viewH_ = body.h;

    // ---------------- Header ----------------
    g.PushClip(header);
    float x = r.x - 0;
    float totalW = 0;
    for (int ci : order_) if (columns[ci].visible) totalW += columns[ci].width;
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
        if (hot && !gripHot) g.Fill(cr, t.surfaceHover);
        if (ui.Pressed(gid)) { resizing_ = ci; resizeStartX_ = ui.in.mx; resizeStartW_ = c.width; ui.SetActive(gid); }
        if (ui.Pressed(hid)) { dragging_ = static_cast<int>(di); dragStartX_ = ui.in.mx; }
        Rect tr = cr.Inset(8, 0);
        bool sorted = sortCol == ci;
        if (!c.header2.empty()) {
            g.Text(c.header2, {tr.x, tr.y + 4, tr.w, 20}, Font::StatValue, t.text, c.align);
            g.Text(c.title, {tr.x, tr.y + 24, tr.w, 18}, sorted ? Font::SmallSemi : Font::Small, sorted ? t.text : t.textSecondary, c.align);
        } else {
            g.Text(c.title, tr, sorted ? Font::SmallSemi : Font::Small, sorted ? t.text : t.textSecondary, c.align);
        }
        if (sorted) g.Glyph(sortDesc ? 0xE70D : 0xE70E, {cr.x + cr.w / 2 - 6, cr.y - 1, 12, 10}, Font::IconSmall, t.textTertiary);
        if (ui.Clicked(hid) && std::fabs(ui.in.mx - dragStartX_) < 4) {
            if (sortCol == ci) sortDesc = !sortDesc;
            else { sortCol = ci; sortDesc = c.align == Align::Right; } // numbers default to descending
            sortChanged = true;
        }
        if (ui.RightClicked(hid) || ui.RightClicked(gid)) { headerMenu = true; headerMenuColumn = ci; }
        if (!c.tooltip.empty()) ui.Tooltip(hid, c.tooltip);
        g.Line(cr.R() - 0.5f, cr.y + 8, cr.R() - 0.5f, cr.B() - 8, t.divider);
        x += c.width;
    }
    // Column resize / reorder dragging.
    if (resizing_ >= 0) {
        if (ui.in.lDown) columns[resizing_].width = std::max(36.f, resizeStartW_ + (ui.in.mx - resizeStartX_));
        else resizing_ = -1;
    }
    if (dragging_ >= 0) {
        if (!ui.in.lDown) {
            float dx = ui.in.mx - dragStartX_;
            if (std::fabs(dx) > 24) {
                // Drop: find display slot under the mouse.
                float xx = r.x;
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
    g.Line(r.x, header.B() - 0.5f, r.R(), header.B() - 0.5f, t.divider);

    // ---------------- Body ----------------
    size_t n = src.RowCount();
    float contentH = n * rowH;
    int sel = SelectedRow(src);
    if (ensureVisible_ && sel >= 0) {
        float top = sel * rowH;
        if (top < scrollY) scrollY = top;
        else if (top + rowH > scrollY + body.h) scrollY = top + rowH - body.h;
        ensureVisible_ = false;
    }
    WidgetId bodyId = IdMix(id_, 2);
    ui.Hit(bodyId, body);
    float wh = ui.Wheel(body);
    if (wh != 0) scrollY -= wh * rowH * 3;
    scrollY = std::clamp(scrollY, 0.f, std::max(0.f, contentH - body.h));

    g.PushClip(body);
    size_t first = static_cast<size_t>(scrollY / rowH);
    size_t last = std::min(n, first + static_cast<size_t>(body.h / rowH) + 2);
    for (size_t row = first; row < last; ++row) {
        float y = body.y + row * rowH - scrollY;
        Rect rr{r.x, y, r.w, rowH};
        WidgetId rid = IdMix(id_, 100000 + src.RowKey(row));
        bool hot = ui.Hit(rid, rr);
        bool isSel = static_cast<int>(row) == sel;
        if (src.RowIsGroup(row)) {
            std::wstring label;
            src.CellText(row, 0, label);
            g.Text(label, {r.x + 12, y + 6, r.w - 24, rowH - 6}, Font::BodySemi, t.text);
            if (ui.Clicked(rid)) src.ToggleExpand(row);
            continue;
        }
        int hl = src.RowHighlight(row);
        if (hl == 1) g.Fill(rr, t.good.Alpha(0.16f));
        else if (hl == 2) g.Fill(rr, t.critical.Alpha(0.18f));
        if (isSel) g.FillRound(rr.Inset(2, 1), 4.f, t.selection);
        else if (hot) g.FillRound(rr.Inset(2, 1), 4.f, t.hover);
        Color textC = hl == 3 ? t.textTertiary : t.text;

        float cx = r.x;
        bool firstVisible = true;
        for (int ci : order_) {
            auto& c = columns[ci];
            if (!c.visible) continue;
            Rect cr{cx, y, c.width, rowH};
            if (heatMap && c.heat) {
                float h = src.CellHeat(row, ci);
                if (h > 0.001f) g.Fill({cr.x, cr.y, cr.w, cr.h}, t.Heat(h));
            }
            src.CellText(row, ci, cell_);
            Rect tr = cr.Inset(8, 0);
            if (firstVisible) {
                int depth = src.RowDepth(row);
                float indent = depth * 18.f;
                tr.x += indent;
                tr.w -= indent;
                int ex = src.RowExpander(row);
                if (ex) {
                    Rect er{tr.x - 2, y, 18, rowH};
                    WidgetId eid = IdMix(id_, 900000 + src.RowKey(row));
                    bool eh = ui.Hit(eid, er);
                    if (eh) g.FillRound(er.Inset(1, 5), 3.f, t.surfaceHover);
                    g.Glyph(ex == 2 ? 0xE70D : 0xE76C, er, Font::IconSmall, t.textSecondary);
                    if (ui.Clicked(eid)) src.ToggleExpand(row);
                }
                tr.x += 18;
                tr.w -= 18;
                if (showIcons) {
                    if (ID2D1Bitmap* icon = src.RowIcon(row)) g.DrawBitmap(icon, {tr.x, y + (rowH - 16) / 2, 16, 16});
                    tr.x += 24;
                    tr.w -= 24;
                }
                firstVisible = false;
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
        float trackH = body.h - 4;
        float thumbH = std::max(28.f, trackH * body.h / contentH);
        float thumbY = body.y + 2 + (trackH - thumbH) * scrollY / (contentH - body.h);
        WidgetId sid = IdMix(id_, 3);
        Rect hitR{body.R() - 12, body.y, 12, body.h};
        bool hot = ui.Hit(sid, hitR);
        if (ui.Pressed(sid)) { sbDragY_ = ui.in.my; sbDragOff_ = scrollY; }
        if (ui.IsActive(sid) && ui.in.lDown) scrollY = std::clamp(sbDragOff_ + (ui.in.my - sbDragY_) * contentH / body.h, 0.f, contentH - body.h);
        g.FillRound({body.R() - (hot ? 8.f : 6.f), thumbY, hot ? 6.f : 4.f, thumbH}, 3.f, t.textTertiary.Alpha(hot ? 0.8f : 0.4f));
    }
    (void)totalW;
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
