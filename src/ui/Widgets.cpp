#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace nx {

void Ui::BeginFrame() {
    prevHits_.swap(hits_);
    hits_.clear();
    hot_ = HitTest(in.mx, in.my);
    if (in.lPressed) active_ = hot_;
    tooltipText_.clear();
}

void Ui::EndFrame() {
    if (!tooltipText_.empty() && in.tooltipReady) {
        float w = std::min(360.f, g.Measure(tooltipText_, Font::Small) + 18.f);
        float h = 26.f;
        bool multi = g.Measure(tooltipText_, Font::Small) + 18.f > 360.f;
        if (multi) h = g.MeasureParagraph(tooltipText_, w - 18.f, Font::Small) + 12.f;
        float x = std::clamp(in.mx + 12.f, 4.f, g.Width() - w - 4.f);
        float y = in.my + 20.f;
        if (y + h > g.Height() - 4) y = in.my - h - 8.f;
        Rect r{x, y, w, h};
        g.FillRound(r, 6.f, t.tooltipBg);
        g.StrokeRound(r, 6.f, t.cardBorder);
        if (multi) g.Paragraph(tooltipText_, r.Inset(9.f, 6.f), Font::Small, t.tooltipText);
        else g.Text(tooltipText_, r.Inset(9.f, 0), Font::Small, t.tooltipText);
    }
    if (in.lReleased) active_ = 0;
    if (!in.lDown && drag_.id) drag_.id = 0;
}

WidgetId Ui::HitTest(float x, float y) const {
    // Last registered wins (drawn on top).
    for (auto it = prevHits_.rbegin(); it != prevHits_.rend(); ++it)
        if (it->first.Contains(x, y)) return it->second;
    return 0;
}

bool Ui::Hit(WidgetId id, const Rect& r) {
    hits_.emplace_back(r, id);
    return hot_ == id;
}

float Ui::Wheel(const Rect& r) {
    if (in.wheel == 0 || !r.Contains(in.mx, in.my)) return 0;
    float w = in.wheel;
    in.wheel = 0;
    return w;
}

bool Ui::Button(WidgetId id, const Rect& r, const std::wstring& label, wchar_t glyph, ButtonStyle style, bool enabled) {
    bool hot = enabled && Hit(id, r);
    bool down = hot && active_ == id && in.lDown;
    Color bg, fg = t.text, border = t.cardBorder;
    switch (style) {
    case ButtonStyle::Accent:
        bg = enabled ? (down ? t.accent.Alpha(0.8f) : hot ? t.accent.Alpha(0.9f) : t.accent) : t.textDisabled.Alpha(0.2f);
        fg = enabled ? t.onAccent : t.textDisabled;
        border = Color{0, 0, 0, 0};
        break;
    case ButtonStyle::Subtle:
        bg = down ? t.surfacePressed : hot ? t.surfaceHover : Color{0, 0, 0, 0};
        border = Color{0, 0, 0, 0};
        break;
    case ButtonStyle::Danger:
        bg = down ? t.critical.Alpha(0.25f) : hot ? t.critical.Alpha(0.18f) : t.surfaceAlt;
        fg = enabled ? t.critical : t.textDisabled;
        break;
    default:
        bg = down ? t.surfacePressed : hot ? t.surfaceAlt.Mix(t.text, 0.04f) : t.surfaceAlt;
        break;
    }
    if (!enabled && style != ButtonStyle::Accent) fg = t.textDisabled;
    g.FillRound(r, 4.f, bg);
    if (border.a > 0) g.StrokeRound(r, 4.f, border);
    float tw = label.empty() ? 0 : g.Measure(label, Font::Body);
    float gw = glyph ? 16.f : 0;
    float gap = glyph && !label.empty() ? 8.f : 0;
    float total = tw + gw + gap;
    float x = r.x + std::max(8.f, (r.w - total) / 2);
    if (glyph) g.Glyph(glyph, {x, r.y, gw, r.h}, Font::IconSmall, fg);
    if (!label.empty()) g.Text(label, {x + gw + gap, r.y, r.R() - (x + gw + gap) - 4, r.h}, Font::Body, fg);
    return enabled && Clicked(id);
}

bool Ui::IconButton(WidgetId id, const Rect& r, wchar_t glyph, const std::wstring& tooltip, bool enabled, bool toggled) {
    bool hot = enabled && Hit(id, r);
    bool down = hot && active_ == id && in.lDown;
    if (toggled) g.FillRound(r, 4.f, t.selection);
    else if (down) g.FillRound(r, 4.f, t.surfacePressed);
    else if (hot) g.FillRound(r, 4.f, t.surfaceHover);
    g.Glyph(glyph, r, Font::Icon, enabled ? (toggled ? t.accentText : t.text) : t.textDisabled);
    if (!tooltip.empty()) Tooltip(id, tooltip);
    return enabled && Clicked(id);
}

bool Ui::Toggle(WidgetId id, const Rect& r, bool& value, const std::wstring& label) {
    bool hot = Hit(id, r);
    float tw = 40.f, th = 20.f;
    Rect sw{r.R() - tw, r.y + (r.h - th) / 2, tw, th};
    if (value) g.FillRound(sw, th / 2, hot ? t.accent.Alpha(0.9f) : t.accent);
    else {
        g.FillRound(sw, th / 2, hot ? t.surfaceHover : Color{0, 0, 0, 0});
        g.StrokeRound(sw, th / 2, t.textSecondary, 1.f);
    }
    float knob = value ? 14.f : 12.f;
    float cx = value ? sw.R() - th / 2 : sw.x + th / 2;
    g.FillEllipse(cx, sw.y + th / 2, knob / 2, knob / 2, value ? t.onAccent : t.textSecondary);
    if (!label.empty()) g.Text(label, {r.x, r.y, r.w - tw - 12, r.h}, Font::Body, t.text);
    if (Clicked(id)) { value = !value; return true; }
    return false;
}

bool Ui::Checkbox(WidgetId id, const Rect& r, bool& value, const std::wstring& label) {
    bool hot = Hit(id, r);
    Rect box{r.x, r.y + (r.h - 18) / 2, 18, 18};
    if (value) {
        g.FillRound(box, 3.f, hot ? t.accent.Alpha(0.9f) : t.accent);
        g.Glyph(0xE73E, box, Font::IconSmall, t.onAccent);
    } else {
        g.FillRound(box, 3.f, hot ? t.surfaceHover : Color{0, 0, 0, 0});
        g.StrokeRound(box, 3.f, t.textSecondary);
    }
    g.Text(label, {box.R() + 8, r.y, r.R() - box.R() - 8, r.h}, Font::Body, t.text);
    if (Clicked(id)) { value = !value; return true; }
    return false;
}

int Ui::Tabs(WidgetId id, const Rect& r, const std::vector<std::wstring>& labels, int selected) {
    int clicked = -1;
    float x = r.x;
    for (size_t i = 0; i < labels.size(); ++i) {
        float w = g.Measure(labels[i], Font::BodySemi) + 24.f;
        Rect tr{x, r.y, w, r.h};
        WidgetId tid = IdMix(id, i);
        bool hot = Hit(tid, tr);
        bool sel = static_cast<int>(i) == selected;
        if (sel) g.FillRound(tr, 4.f, t.surfaceAlt.Mix(t.text, 0.03f));
        else if (hot) g.FillRound(tr, 4.f, t.surfaceHover);
        g.Text(labels[i], tr, sel ? Font::BodySemi : Font::Body, sel ? t.text : t.textSecondary, Align::Center);
        if (sel) g.FillRound({tr.x + w / 2 - 8, tr.B() - 3, 16, 3}, 1.5f, t.accent);
        if (Clicked(tid)) clicked = static_cast<int>(i);
        x += w + 2;
    }
    return clicked;
}

bool Ui::ComboButton(WidgetId id, const Rect& r, const std::wstring& value, bool enabled) {
    bool hot = enabled && Hit(id, r);
    g.FillRound(r, 4.f, hot ? t.surfaceAlt.Mix(t.text, 0.04f) : t.surfaceAlt);
    g.StrokeRound(r, 4.f, t.cardBorder);
    g.Text(value, {r.x + 10, r.y, r.w - 36, r.h}, Font::Body, enabled ? t.text : t.textDisabled);
    g.Glyph(0xE70D, {r.R() - 28, r.y, 20, r.h}, Font::IconSmall, t.textSecondary);
    return enabled && Clicked(id);
}

float Ui::Scroll(WidgetId id, const Rect& vp, float contentH, float& offset) {
    float maxOff = std::max(0.f, contentH - vp.h);
    float w = Wheel(vp);
    if (w != 0) offset -= w * 48.f;
    if (maxOff > 0) {
        float trackH = vp.h - 4;
        float thumbH = std::max(24.f, trackH * vp.h / contentH);
        float thumbY = vp.y + 2 + (trackH - thumbH) * (maxOff > 0 ? offset / maxOff : 0);
        Rect thumb{vp.R() - 6, thumbY, 4, thumbH};
        Rect hitR{vp.R() - 10, vp.y, 10, vp.h};
        bool hot = Hit(id, hitR);
        if (in.lPressed && hot) drag_ = {id, in.my, offset};
        if (drag_.id == id && in.lDown) offset = drag_.startOffset + (in.my - drag_.startY) * (contentH / vp.h);
        g.FillRound(hot || drag_.id == id ? Rect{vp.R() - 8, thumbY, 6, thumbH} : thumb, 3.f, t.textTertiary.Alpha(hot ? 0.8f : 0.45f));
    }
    offset = std::clamp(offset, 0.f, maxOff);
    return offset;
}

void Ui::Card(const Rect& r, bool hover) {
    g.FillRound(r, 8.f, hover ? t.card.Mix(t.text, 0.03f) : t.card);
    g.StrokeRound(r, 8.f, t.cardBorder);
}

void Ui::Tooltip(WidgetId id, const std::wstring& text) {
    if (hot_ == id && !text.empty()) tooltipText_ = text;
}

float Ui::KeyValue(float x, float y, float keyW, float valW, const std::wstring& key, const std::wstring& value, Color vc) {
    g.Text(key, {x, y, keyW - 8, 22}, Font::Small, t.textSecondary);
    g.Text(value.empty() ? L"—" : value, {x + keyW, y, valW, 22}, Font::Body, vc.a > 0 ? vc : t.text);
    return y + 22;
}

void Ui::Badge(const Rect& r, const std::wstring& text, Color bg, Color fg) {
    g.FillRound(r, r.h / 2, bg);
    g.Text(text, r, Font::Caption, fg, Align::Center);
}

void Ui::Meter(const Rect& r, float f, Color fill) {
    g.FillRound(r, r.h / 2, t.divider);
    f = std::clamp(f, 0.f, 1.f);
    if (f > 0) g.FillRound({r.x, r.y, std::max(r.h, r.w * f), r.h}, r.h / 2, fill);
}

void Ui::Ring(float cx, float cy, float radius, float f, Color fill, float width) {
    g.Arc(cx, cy, radius, 0, 359.99f, t.divider, width);
    g.Arc(cx, cy, radius, -90.f, 360.f * std::clamp(f, 0.f, 1.f), fill, width);
}

} // namespace nx
