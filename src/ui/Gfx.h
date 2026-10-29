#pragma once
// Direct2D / DirectWrite drawing layer. All coordinates are device-independent pixels (DIPs);
// the render target's DPI is set from the window's monitor DPI times the user UI scale.
#include "core/Win.h"

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nx {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static constexpr Color Hex(uint32_t rgb, float a = 1.f) {
        return {((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a};
    }
    static constexpr Color None() { return {0, 0, 0, 0}; } // "use the default colour" (Color{} is opaque black)
    Color Alpha(float na) const { return {r, g, b, na}; }
    Color Mix(const Color& o, float t) const { return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t}; }
    D2D1_COLOR_F D2D() const { return {r, g, b, a}; }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float R() const { return x + w; }
    float B() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect TakeTop(float th) { Rect r{x, y, w, th}; y += th; h -= th; return r; }
    Rect TakeLeft(float tw) { Rect r{x, y, tw, h}; x += tw; w -= tw; return r; }
    Rect TakeRight(float tw) { Rect r{R() - tw, y, tw, h}; w -= tw; return r; }
    Rect TakeBottom(float th) { Rect r{x, B() - th, w, th}; h -= th; return r; }
    D2D1_RECT_F D2D() const { return {x, y, x + w, y + h}; }
};

enum class Font : uint8_t { Body, BodySemi, Small, SmallSemi, Caption, Subtitle, Title, Display, StatValue, Icon, IconSmall, IconLarge, Mono, Count };
enum class Align : uint8_t { Left, Center, Right };
enum class MetricIcon : int8_t { None = -1, Cpu, Memory, Disk, Network, Gpu, Info, Bolt };

class Gfx {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void Resize(UINT widthPx, UINT heightPx);
    void SetDpi(float dpi);       // effective dpi (monitor dpi * user scale)
    float Scale() const { return dpi_ / 96.f; }
    float Width() const { return widthPx_ / Scale(); }
    float Height() const { return heightPx_ / Scale(); }

    bool Begin();
    void End();                   // recreates device resources on D2DERR_RECREATE_TARGET
    void Clear(Color c);

    void Fill(const Rect& r, Color c);
    void FillRound(const Rect& r, float radius, Color c);
    void StrokeRound(const Rect& r, float radius, Color c, float width = 1.f);
    void Stroke(const Rect& r, Color c, float width = 1.f);
    void Line(float x1, float y1, float x2, float y2, Color c, float width = 1.f);
    void FillEllipse(float cx, float cy, float rx, float ry, Color c);
    void Arc(float cx, float cy, float radius, float startDeg, float sweepDeg, Color c, float width);

    void Text(const std::wstring& s, const Rect& r, Font f, Color c, Align a = Align::Left, bool vcenter = true);
    void Text(const wchar_t* s, size_t len, const Rect& r, Font f, Color c, Align a = Align::Left, bool vcenter = true);
    void Glyph(wchar_t glyph, const Rect& r, Font f, Color c) { Text(&glyph, 1, r, f, c, Align::Center, true); }
    // Multi-line text with word wrapping; returns the height used.
    float Paragraph(const std::wstring& s, const Rect& r, Font f, Color c);
    float MeasureParagraph(const std::wstring& s, float width, Font f);
    float Measure(const std::wstring& s, Font f);

    // Line/area chart of `n` values scaled to [0, maxValue]; values are oldest-first, right-aligned
    // to `capacity` slots so graphs scroll like Task Manager's.
    void Series(const Rect& r, const float* values, size_t n, size_t capacity, float maxValue, Color line, Color fill, float width = 1.f);

    // Gradient fills. vertical=false gives a left-to-right gradient.
    void FillGradient(const Rect& r, float radius, Color from, Color to, bool vertical = true);
    // Soft radial glow (transparent at the edge).
    void Glow(float cx, float cy, float rx, float ry, Color center);
    // Rounded outline with a soft outer glow in the same colour.
    void GlowStroke(const Rect& r, float radius, Color c, float strength = 1.f);
    // Area chart whose fill fades from `fillTop` to transparent.
    void SeriesGradient(const Rect& r, const float* values, size_t n, size_t capacity, float maxValue, Color line, Color fillTop, float width = 1.5f);
    // Small vector glyphs that must not depend on icon fonts.
    void Leaf(float x, float y, float size, Color c);
    void Logo(const Rect& r, Color from, Color to);
    void Chevron(float cx, float cy, float size, bool down, Color c, float width = 1.4f);
    // Line-art hardware icons drawn in a square `r`.
    void DrawIcon(MetricIcon icon, const Rect& r, Color c, float width = 1.5f);
    // Rounded tile with a tinted gradient and the icon centred in it.
    void IconTile(MetricIcon icon, const Rect& r, Color tint, bool dark);

    void PushClip(const Rect& r);
    void PopClip();
    void DrawBitmap(ID2D1Bitmap* bmp, const Rect& r, float opacity = 1.f);

    ID2D1RenderTarget* RT() const { return rt_; }
    // Paints static content once into an offscreen bitmap (slot = caller-chosen id) and reuses it
    // while `key`, the rectangle size and the DPI stay the same. Falls back to drawing directly.
    void CachedLayer(int slot, uint64_t key, const Rect& r, const std::function<void()>& paint);
    ID2D1Factory* Factory() const { return factory_.Get(); }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    // Called after the device was recreated so caches (icon bitmaps) can be dropped.
    uint32_t DeviceGeneration() const { return generation_; }
    void SetMonoFamily(const wchar_t* f) { monoFamily_ = f; }
    // UI typeface for every Gfx in the process: "inter" (bundled Inter, the default) or "segoe".
    // Falls back to Segoe UI Variable / Segoe UI if the embedded fonts cannot be loaded.
    static bool SetUiFont(const std::wstring& pref);
    static bool BundledFontAvailable();
    static const std::wstring& TextFamily();
    void SetTransparentBackground(bool on) { transparent_ = on; }

private:
    bool CreateTarget();
    IDWriteTextFormat* Format(Font f, Align a, bool vcenter, bool wrap = false);
    ID2D1SolidColorBrush* Brush(Color c);

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> hwndRt_;
    ID2D1RenderTarget* rt_ = nullptr; // current target: the window, or an offscreen layer while it is painted
    struct Layer { ComPtr<ID2D1BitmapRenderTarget> rt; ComPtr<ID2D1Bitmap> bmp; uint64_t key = 0; float w = 0, h = 0, dpi = 0; uint32_t gen = 0; };
    std::unordered_map<int, Layer> layers_;
    std::unordered_map<uint64_t, ComPtr<ID2D1GradientStopCollection>> gradients_;
    ID2D1GradientStopCollection* Gradient(Color from, Color to);
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteInlineObject> ellipsis_[static_cast<int>(Font::Count)];
    std::unordered_map<uint32_t, ComPtr<IDWriteTextFormat>> formats_;
    std::vector<Rect> clips_;
    UINT widthPx_ = 1, heightPx_ = 1;
    float dpi_ = 96.f;
    uint32_t generation_ = 0;
    uint32_t fontGen_ = 0;
    std::wstring iconFamily_, monoFamily_ = L"Cascadia Mono";
    bool transparent_ = false;
};

} // namespace nx
