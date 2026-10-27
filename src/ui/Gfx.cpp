#include "ui/Gfx.h"

#include <algorithm>
#include <cmath>

namespace nx {

namespace {
bool HasFamily(IDWriteFactory* f, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> coll;
    if (FAILED(f->GetSystemFontCollection(coll.Put(), FALSE))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(coll->FindFamilyName(name, &idx, &exists)) && exists;
}

struct FontSpec { float size; DWRITE_FONT_WEIGHT weight; int family; }; // family: 0 text, 1 display, 2 icon, 3 mono
const FontSpec kSpecs[] = {
    {13.f, DWRITE_FONT_WEIGHT_NORMAL, 0},   // Body
    {13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0}, // BodySemi
    {12.f, DWRITE_FONT_WEIGHT_NORMAL, 0},   // Small
    {12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0}, // SmallSemi
    {11.f, DWRITE_FONT_WEIGHT_NORMAL, 0},   // Caption
    {15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1}, // Subtitle
    {20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1}, // Title
    {28.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1}, // Display
    {17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1}, // StatValue
    {16.f, DWRITE_FONT_WEIGHT_NORMAL, 2},   // Icon
    {12.f, DWRITE_FONT_WEIGHT_NORMAL, 2},   // IconSmall
    {20.f, DWRITE_FONT_WEIGHT_NORMAL, 2},   // IconLarge
    {12.f, DWRITE_FONT_WEIGHT_NORMAL, 3},   // Mono
};
std::wstring g_text = L"Segoe UI", g_display = L"Segoe UI", g_icon = L"Segoe MDL2 Assets";
} // namespace

bool Gfx::Init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts, factory_.PutVoid()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.Put())))) return false;
    if (HasFamily(dwrite_.Get(), L"Segoe UI Variable Text")) g_text = L"Segoe UI Variable Text";
    if (HasFamily(dwrite_.Get(), L"Segoe UI Variable Display")) g_display = L"Segoe UI Variable Display";
    if (HasFamily(dwrite_.Get(), L"Segoe Fluent Icons")) g_icon = L"Segoe Fluent Icons";
    if (!HasFamily(dwrite_.Get(), monoFamily_.c_str())) monoFamily_ = HasFamily(dwrite_.Get(), L"Consolas") ? L"Consolas" : L"Courier New";
    RECT rc;
    GetClientRect(hwnd, &rc);
    widthPx_ = std::max<LONG>(1, rc.right - rc.left);
    heightPx_ = std::max<LONG>(1, rc.bottom - rc.top);
    return CreateTarget();
}

void Gfx::Shutdown() {
    brush_.Reset();
    rt_.Reset();
    formats_.clear();
    for (auto& e : ellipsis_) e.Reset();
    dwrite_.Reset();
    factory_.Reset();
}

bool Gfx::CreateTarget() {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, transparent_ ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(widthPx_, heightPx_), D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(factory_->CreateHwndRenderTarget(props, hp, rt_.Put()))) {
        // Hardware unavailable (e.g. remote session quirks): fall back to software rendering.
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        if (FAILED(factory_->CreateHwndRenderTarget(props, hp, rt_.Put()))) return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.Put()))) return false;
    ++generation_;
    return true;
}

void Gfx::Resize(UINT w, UINT h) {
    widthPx_ = std::max<UINT>(1, w);
    heightPx_ = std::max<UINT>(1, h);
    if (rt_) rt_->Resize(D2D1::SizeU(widthPx_, heightPx_));
}

void Gfx::SetDpi(float dpi) {
    dpi_ = dpi;
    if (rt_) rt_->SetDpi(dpi, dpi);
}

bool Gfx::Begin() {
    if (!rt_ && !CreateTarget()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::End() {
    while (!clips_.empty()) PopClip();
    HRESULT hr = rt_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        brush_.Reset();
        rt_.Reset();
        CreateTarget();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Gfx::Clear(Color c) { rt_->Clear(c.D2D()); }

ID2D1SolidColorBrush* Gfx::Brush(Color c) {
    brush_->SetColor(c.D2D());
    return brush_.Get();
}

void Gfx::Fill(const Rect& r, Color c) { if (r.w > 0 && r.h > 0) rt_->FillRectangle(r.D2D(), Brush(c)); }

void Gfx::FillRound(const Rect& r, float rad, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), rad, rad), Brush(c));
}

void Gfx::StrokeRound(const Rect& r, float rad, Color c, float w) {
    D2D1_RECT_F d{r.x + w / 2, r.y + w / 2, r.R() - w / 2, r.B() - w / 2};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(d, rad, rad), Brush(c), w);
}

void Gfx::Stroke(const Rect& r, Color c, float w) {
    D2D1_RECT_F d{r.x + w / 2, r.y + w / 2, r.R() - w / 2, r.B() - w / 2};
    rt_->DrawRectangle(d, Brush(c), w);
}

void Gfx::Line(float x1, float y1, float x2, float y2, Color c, float w) { rt_->DrawLine({x1, y1}, {x2, y2}, Brush(c), w); }

void Gfx::FillEllipse(float cx, float cy, float rx, float ry, Color c) { rt_->FillEllipse(D2D1::Ellipse({cx, cy}, rx, ry), Brush(c)); }

void Gfx::Arc(float cx, float cy, float radius, float startDeg, float sweepDeg, Color c, float width) {
    if (sweepDeg <= 0.01f) return;
    if (sweepDeg >= 359.9f) {
        rt_->DrawEllipse(D2D1::Ellipse({cx, cy}, radius, radius), Brush(c), width);
        return;
    }
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory_->CreatePathGeometry(geo.Put()))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(sink.Put()))) return;
    const float k = 3.14159265f / 180.f;
    D2D1_POINT_2F p0{cx + radius * std::cos(startDeg * k), cy + radius * std::sin(startDeg * k)};
    D2D1_POINT_2F p1{cx + radius * std::cos((startDeg + sweepDeg) * k), cy + radius * std::sin((startDeg + sweepDeg) * k)};
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweepDeg > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> style;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    factory_->CreateStrokeStyle(sp, nullptr, 0, style.Put());
    rt_->DrawGeometry(geo.Get(), Brush(c), width, style.Get());
}

IDWriteTextFormat* Gfx::Format(Font f, Align a, bool vcenter, bool wrap) {
    uint32_t key = static_cast<uint32_t>(f) | (static_cast<uint32_t>(a) << 8) | (vcenter ? 1u << 10 : 0) | (wrap ? 1u << 11 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const FontSpec& s = kSpecs[static_cast<int>(f)];
    const std::wstring& fam = s.family == 0 ? g_text : s.family == 1 ? g_display : s.family == 2 ? g_icon : monoFamily_;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(fam.c_str(), nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"", fmt.Put())))
        return nullptr;
    fmt->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        auto& el = ellipsis_[static_cast<int>(f)];
        if (!el) dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), el.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, el.Get());
    }
    IDWriteTextFormat* raw = fmt.Get();
    formats_[key] = std::move(fmt);
    return raw;
}

void Gfx::Text(const std::wstring& s, const Rect& r, Font f, Color c, Align a, bool vcenter) { Text(s.c_str(), s.size(), r, f, c, a, vcenter); }

void Gfx::Text(const wchar_t* s, size_t len, const Rect& r, Font f, Color c, Align a, bool vcenter) {
    if (!len || r.w <= 1 || r.h <= 1) return;
    IDWriteTextFormat* fmt = Format(f, a, vcenter);
    if (!fmt) return;
    rt_->DrawText(s, static_cast<UINT32>(len), fmt, r.D2D(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::MeasureParagraph(const std::wstring& s, float width, Font f) {
    IDWriteTextFormat* fmt = Format(f, Align::Left, false, true);
    ComPtr<IDWriteTextLayout> lay;
    if (!fmt || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, width, 10000.f, lay.Put()))) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.height;
}

float Gfx::Paragraph(const std::wstring& s, const Rect& r, Font f, Color c) {
    IDWriteTextFormat* fmt = Format(f, Align::Left, false, true);
    ComPtr<IDWriteTextLayout> lay;
    if (!fmt || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, r.w, std::max(r.h, 1.f), lay.Put()))) return 0;
    rt_->DrawTextLayout({r.x, r.y}, lay.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.height;
}

float Gfx::Measure(const std::wstring& s, Font f) {
    IDWriteTextFormat* fmt = Format(f, Align::Left, false);
    ComPtr<IDWriteTextLayout> lay;
    if (!fmt || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 10000.f, 100.f, lay.Put()))) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Gfx::Series(const Rect& r, const float* v, size_t n, size_t capacity, float maxValue, Color line, Color fill, float width) {
    if (n < 2 || capacity < 2 || maxValue <= 0 || r.w <= 2) return;
    n = std::min(n, capacity);
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory_->CreatePathGeometry(geo.Put()))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(sink.Put()))) return;
    const float dx = r.w / static_cast<float>(capacity - 1);
    const float x0 = r.R() - dx * static_cast<float>(n - 1);
    auto y = [&](float val) { return r.B() - std::clamp(val / maxValue, 0.f, 1.f) * r.h; };
    // Closed area for the fill.
    sink->BeginFigure({x0, r.B()}, D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 0; i < n; ++i) sink->AddLine({x0 + dx * static_cast<float>(i), y(v[i])});
    sink->AddLine({r.R(), r.B()});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (fill.a > 0) rt_->FillGeometry(geo.Get(), Brush(fill));
    // Separate open figure for the line so it isn't drawn along the baseline.
    ComPtr<ID2D1PathGeometry> lg;
    if (FAILED(factory_->CreatePathGeometry(lg.Put()))) return;
    ComPtr<ID2D1GeometrySink> ls;
    if (FAILED(lg->Open(ls.Put()))) return;
    ls->BeginFigure({x0, y(v[0])}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < n; ++i) ls->AddLine({x0 + dx * static_cast<float>(i), y(v[i])});
    ls->EndFigure(D2D1_FIGURE_END_OPEN);
    ls->Close();
    rt_->DrawGeometry(lg.Get(), Brush(line), width);
}

void Gfx::PushClip(const Rect& r) {
    clips_.push_back(r);
    rt_->PushAxisAlignedClip(r.D2D(), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Gfx::PopClip() {
    if (clips_.empty()) return;
    clips_.pop_back();
    rt_->PopAxisAlignedClip();
}

void Gfx::DrawBitmap(ID2D1Bitmap* bmp, const Rect& r, float opacity) {
    if (bmp) rt_->DrawBitmap(bmp, r.D2D(), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

} // namespace nx
