#include "ui/Graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nx {

void GraphView::Spark(Gfx& g, const Rect& r, const RingBuffer<float>& data, float maxValue, Color c, bool fill) {
    static thread_local std::vector<float> buf;
    data.Tail(data.Size(), buf);
    g.Series(r, buf.data(), buf.size(), data.Capacity(), maxValue, c, fill ? c.Alpha(0.18f) : Color{0, 0, 0, 0}, 1.f);
}

void GraphView::Draw(Ui& ui, WidgetId id, const Rect& r, const std::vector<GraphLine>& lines, float maxValue, double interval,
                     const std::function<std::wstring(float)>& fmt, bool grid, bool timeAxis) {
    Gfx& g = ui.g;
    const Theme& t = ui.t;
    size_t cap = 0;
    for (auto& l : lines) if (l.data) cap = std::max(cap, l.data->Capacity());
    if (cap < 2) return;
    size_t visible = zoom ? std::clamp<size_t>(zoom, 10, cap) : cap;

    bool hot = ui.Hit(id, r);
    // Interaction: Ctrl+wheel zooms, drag pans, double-click resets.
    if (hot && ui.in.ctrl) {
        float w = ui.Wheel(r);
        if (w > 0) visible = std::max<size_t>(10, visible * 2 / 3);
        else if (w < 0) visible = std::min(cap, visible * 3 / 2 + 1);
        zoom = visible == cap ? 0 : visible;
    }
    if (ui.Pressed(id)) { dragX_ = ui.in.mx; dragPan_ = pan; }
    if (ui.IsActive(id) && ui.in.lDown) {
        float dx = ui.in.mx - dragX_;
        long long np = static_cast<long long>(dragPan_) + static_cast<long long>(dx / r.w * visible);
        pan = static_cast<size_t>(std::max(0LL, np));
    }
    if (ui.DoubleClicked(id)) { zoom = 0; pan = 0; visible = cap; }
    size_t maxPan = 0;
    for (auto& l : lines) if (l.data && l.data->Size() > visible) maxPan = std::max(maxPan, l.data->Size() - visible);
    pan = std::min(pan, maxPan);

    g.FillRound(r, 4.f, t.graphBg);
    if (grid) {
        for (int i = 1; i < 4; ++i) g.Line(r.x, r.y + r.h * i / 4.f, r.R(), r.y + r.h * i / 4.f, t.graphGrid);
        for (int i = 1; i < 10; ++i) g.Line(r.x + r.w * i / 10.f, r.y, r.x + r.w * i / 10.f, r.B(), t.graphGrid);
    }
    g.PushClip(r);
    for (auto& l : lines) {
        if (!l.data || l.data->Size() < 2) continue;
        size_t avail = l.data->Size();
        size_t end = avail - std::min(pan, avail);
        size_t start = end > visible ? end - visible : 0;
        tmp_.resize(end - start);
        for (size_t i = start; i < end; ++i) tmp_[i - start] = l.data->At(i);
        g.Series(r, tmp_.data(), tmp_.size(), visible, maxValue, l.color, l.fill ? l.color.Alpha(0.16f) : Color{0, 0, 0, 0}, 1.25f);
    }
    g.PopClip();
    g.StrokeRound(r, 4.f, t.cardBorder);

    // Hover read-out.
    if (hot && !ui.in.lDown && fmt) {
        float dx = r.w / static_cast<float>(visible - 1);
        size_t fromRight = static_cast<size_t>(std::round((r.R() - ui.in.mx) / dx));
        float lx = r.R() - fromRight * dx;
        std::wstring tip;
        bool any = false;
        for (auto& l : lines) {
            if (!l.data) continue;
            size_t avail = l.data->Size();
            size_t back = fromRight + pan;
            if (back >= avail) continue;
            float v = l.data->At(avail - 1 - back);
            if (!tip.empty()) tip += L"\n";
            tip += (l.name.empty() ? L"" : l.name + L": ") + fmt(v);
            float y = r.B() - std::clamp(v / maxValue, 0.f, 1.f) * r.h;
            g.FillEllipse(lx, y, 3.f, 3.f, l.color);
            any = true;
        }
        if (any) {
            g.Line(lx, r.y, lx, r.B(), t.textTertiary.Alpha(0.5f));
            double secs = (fromRight + pan) * interval;
            wchar_t ago[48];
            if (secs < 0.5) swprintf(ago, 48, L"now");
            else if (secs < 120) swprintf(ago, 48, L"%.0f s ago", secs);
            else swprintf(ago, 48, L"%.1f min ago", secs / 60.0);
            ui.Tooltip(id, tip + L"\n" + ago);
        }
    }
    if (timeAxis) {
        double span = visible * interval;
        wchar_t left[48];
        if (span >= 120) swprintf(left, 48, L"%.0f minutes", span / 60.0);
        else swprintf(left, 48, L"%.0f seconds", span);
        g.Text(left, {r.x, r.B() + 2, r.w / 2, 16}, Font::Caption, t.textTertiary);
        g.Text(pan ? L"(paused view — double-click to reset)" : L"0", {r.x + r.w / 2, r.B() + 2, r.w / 2, 16}, Font::Caption, t.textTertiary, Align::Right);
    }
}

} // namespace nx
