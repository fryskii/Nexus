#pragma once
// Minimal immediate-mode widget layer on top of Gfx. Widgets are drawn every frame; interaction
// state (hover/active/focus) is resolved against the previous frame's hit regions so that
// overlapping elements behave correctly. Frames are only produced when something changes.
#include "ui/Gfx.h"
#include "ui/Theme.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nx {

using WidgetId = uint64_t;
inline constexpr WidgetId Id(const char* s, uint64_t salt = 0) {
    uint64_t h = 1469598103934665603ULL ^ salt;
    while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 1099511628211ULL; }
    return h ? h : 1;
}
// Distinct b values give distinct ids (forcing bit 0 instead would merge neighbours such as 4 and 5).
inline WidgetId IdMix(WidgetId a, uint64_t b) {
    WidgetId h = a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
    return h ? h : 1;
}

struct InputState {
    float mx = -1, my = -1;
    bool lDown = false, rDown = false;
    bool lPressed = false, lReleased = false, rPressed = false, rReleased = false, dblClick = false;
    float wheel = 0;          // notches (+ up)
    bool ctrl = false, shift = false;
    bool tooltipReady = false; // set by the app after the hover delay
    void ClearEvents() { lPressed = lReleased = rPressed = rReleased = dblClick = false; wheel = 0; }
};

enum class ButtonStyle { Standard, Accent, Subtle, Danger };

class Ui {
public:
    Ui(Gfx& g, const Theme& t, InputState& in) : g(g), t(t), in(in) {}
    Gfx& g;
    const Theme& t;
    InputState& in;

    void BeginFrame();
    void EndFrame();

    // Registers an interactive region; returns true if it is the hovered region.
    bool Hit(WidgetId id, const Rect& r);
    bool IsHot(WidgetId id) const { return hot_ == id; }
    bool IsActive(WidgetId id) const { return active_ == id; }
    bool Pressed(WidgetId id) const { return in.lPressed && hot_ == id; }
    bool Clicked(WidgetId id) const { return in.lReleased && active_ == id && hot_ == id; }
    bool RightClicked(WidgetId id) const { return in.rReleased && hot_ == id; }
    bool DoubleClicked(WidgetId id) const { return in.dblClick && hot_ == id; }
    WidgetId HotId() const { return hot_; }
    WidgetId ActiveId() const { return active_; }
    void SetActive(WidgetId id) { active_ = id; }
    // Consumes the wheel delta if the mouse is inside r.
    float Wheel(const Rect& r);

    bool Button(WidgetId id, const Rect& r, const std::wstring& label, wchar_t glyph = 0, ButtonStyle style = ButtonStyle::Standard, bool enabled = true);
    bool IconButton(WidgetId id, const Rect& r, wchar_t glyph, const std::wstring& tooltip, bool enabled = true, bool toggled = false);
    bool Toggle(WidgetId id, const Rect& r, bool& value, const std::wstring& label = L"");
    bool Checkbox(WidgetId id, const Rect& r, bool& value, const std::wstring& label);
    // Pill-style tab strip; returns the clicked index or -1.
    int Tabs(WidgetId id, const Rect& r, const std::vector<std::wstring>& labels, int selected);
    // Dropdown-looking button; the caller shows a native menu when it returns true.
    bool ComboButton(WidgetId id, const Rect& r, const std::wstring& value, bool enabled = true);
    // Vertical scroll helper: returns the clamped offset after wheel/drag; draws a thin scrollbar.
    float Scroll(WidgetId id, const Rect& viewport, float contentHeight, float& offset);

    void Card(const Rect& r, bool hover = false);
    void Panel(const Rect& r, Color tint, bool glow);
    void Tooltip(WidgetId id, const std::wstring& text);
    void Defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    // Draw another frame soon (windows redraw after running deferred actions), e.g. when state
    // changed during drawing that the page only picks up at the start of a frame.
    void RequestFrame() { Defer([] {}); }
    std::vector<std::function<void()>> TakeDeferred() { return std::move(deferred_); }

    // Key/value line helpers for info panels: returns next y.
    float KeyValue(float x, float y, float keyW, float valW, const std::wstring& key, const std::wstring& value, Color valueColor = Color::None());
    void Badge(const Rect& r, const std::wstring& text, Color bg, Color fg);
    void Meter(const Rect& r, float fraction, Color fill);
    void Ring(float cx, float cy, float radius, float fraction, Color fill, float width);

    const std::vector<std::pair<Rect, WidgetId>>& Hits() const { return hits_; }
    WidgetId HitTest(float x, float y) const; // against the last completed frame

private:
    std::vector<std::pair<Rect, WidgetId>> hits_, prevHits_;
    std::vector<std::function<void()>> deferred_;
    WidgetId hot_ = 0, active_ = 0;
    std::wstring tooltipText_;
    Rect tooltipAnchor_;
    struct DragState { WidgetId id = 0; float startY = 0, startOffset = 0; } drag_;
};

} // namespace nx
