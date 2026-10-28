#include "ui/Gfx.h"

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>

namespace nx {

namespace {
bool HasFamily(IDWriteFactory* f, const wchar_t* name) {
    if (!f) return false;
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
bool g_inter = false;       // bundled Inter in use: the Display cut is only used from 20 DIPs up
bool g_fontsResolved = false;
uint32_t g_fontGen = 1;     // bumped when the families change so cached text formats are rebuilt
ComPtr<IDWriteFontCollection> g_bundled; // private collection with the embedded Inter faces
bool g_bundledTried = false;

ComPtr<IDWriteFactory> SharedDWrite() {
    ComPtr<IDWriteFactory> f;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.Put()));
    return f;
}

// Inter (SIL OFL 1.1) is embedded as RCDATA 201-203. The resources stay mapped for the life of the
// process, so the in-memory font files can reference them without copying.
constexpr int kInterResources[] = {201, 202, 203};

bool ResourceBytes(int id, const void*& data, UINT32& size) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(mod, res) : nullptr;
    data = h ? LockResource(h) : nullptr;
    size = res ? SizeofResource(mod, res) : 0;
    return data && size;
}

void LoadBundledFonts() {
    if (g_bundledTried) return;
    g_bundledTried = true;
    ComPtr<IDWriteFactory> base = SharedDWrite();
    ComPtr<IDWriteFactory5> f;
    if (!base || FAILED(base->QueryInterface(__uuidof(IDWriteFactory5), f.PutVoid()))) return;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(f->CreateInMemoryFontFileLoader(loader.Put())) || FAILED(f->RegisterFontFileLoader(loader.Get()))) return;
    ComPtr<IDWriteFontSetBuilder1> builder1;
    if (FAILED(f->CreateFontSetBuilder(builder1.Put()))) return;
    int added = 0;
    for (int id : kInterResources) {
        const void* data;
        UINT32 size;
        if (!ResourceBytes(id, data, size)) continue;
        ComPtr<IDWriteFontFile> file;
        if (FAILED(loader->CreateInMemoryFontFileReference(f.Get(), data, size, nullptr, file.Put()))) continue;
        if (SUCCEEDED(builder1->AddFontFile(file.Get()))) ++added;
        // GDI copy for the native search box and dialog templates (private to this process).
        DWORD installed = 0;
        AddFontMemResourceEx(const_cast<void*>(data), size, nullptr, &installed);
    }
    if (!added) return;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> coll;
    if (FAILED(builder1->CreateFontSet(set.Put())) || FAILED(f->CreateFontCollectionFromFontSet(set.Get(), coll.Put()))) return;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(coll->FindFamilyName(L"Inter", &idx, &exists)) || !exists) return;
    g_bundled.Attach(coll.Get());
    g_bundled->AddRef();
}
} // namespace

bool Gfx::BundledFontAvailable() {
    LoadBundledFonts();
    return static_cast<bool>(g_bundled);
}
const std::wstring& Gfx::TextFamily() { return g_text; }

bool Gfx::SetUiFont(const std::wstring& pref) {
    ComPtr<IDWriteFactory> f = SharedDWrite();
    auto pick = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const wchar_t* n : names)
            if (HasFamily(f.Get(), n)) return n;
        return L"";
    };
    std::wstring text, display;
    bool wantInter = pref != L"segoe"; // "inter" (default); older "sf" settings map here too
    g_inter = wantInter && BundledFontAvailable();
    if (g_inter) {
        text = L"Inter";
        display = L"Inter Display";
    } else {
        text = pick({L"Segoe UI Variable Text", L"Segoe UI"});
        display = pick({L"Segoe UI Variable Display", L"Segoe UI"});
        if (text.empty()) text = L"Segoe UI";
        if (display.empty()) display = text;
    }
    std::wstring icon = HasFamily(f.Get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    if (text != g_text || display != g_display || icon != g_icon) {
        g_text = text;
        g_display = display;
        g_icon = icon;
        ++g_fontGen;
    }
    g_fontsResolved = true;
    return g_inter || !wantInter;
}

bool Gfx::Init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts, factory_.PutVoid()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.Put())))) return false;
    if (!g_fontsResolved) SetUiFont(L"inter");
    if (!HasFamily(dwrite_.Get(), monoFamily_.c_str())) monoFamily_ = HasFamily(dwrite_.Get(), L"Consolas") ? L"Consolas" : L"Courier New";
    RECT rc;
    GetClientRect(hwnd, &rc);
    widthPx_ = std::max<LONG>(1, rc.right - rc.left);
    heightPx_ = std::max<LONG>(1, rc.bottom - rc.top);
    return CreateTarget();
}

void Gfx::Shutdown() {
    brush_.Reset();
    layers_.clear();
    gradients_.clear();
    rt_ = nullptr;
    hwndRt_.Reset();
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
    layers_.clear();
    gradients_.clear();
    rt_ = nullptr;
    if (FAILED(factory_->CreateHwndRenderTarget(props, hp, hwndRt_.Put()))) {
        // Hardware unavailable (e.g. remote session quirks): fall back to software rendering.
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        if (FAILED(factory_->CreateHwndRenderTarget(props, hp, hwndRt_.Put()))) return false;
    }
    rt_ = hwndRt_.Get();
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.Put()))) return false;
    ++generation_;
    return true;
}

void Gfx::Resize(UINT w, UINT h) {
    widthPx_ = std::max<UINT>(1, w);
    heightPx_ = std::max<UINT>(1, h);
    if (hwndRt_) hwndRt_->Resize(D2D1::SizeU(widthPx_, heightPx_));
}

void Gfx::SetDpi(float dpi) {
    dpi_ = dpi;
    if (hwndRt_) hwndRt_->SetDpi(dpi, dpi);
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
        layers_.clear();
        gradients_.clear();
        rt_ = nullptr;
        hwndRt_.Reset();
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
    if (fontGen_ != g_fontGen) {
        formats_.clear();
        for (auto& e : ellipsis_) e.Reset();
        fontGen_ = g_fontGen;
    }
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const FontSpec& s = kSpecs[static_cast<int>(f)];
    int family = s.family == 1 && g_inter && s.size < 20.f ? 0 : s.family;
    const std::wstring& fam = family == 0 ? g_text : family == 1 ? g_display : family == 2 ? g_icon : monoFamily_;
    IDWriteFontCollection* coll = g_inter && family <= 1 ? g_bundled.Get() : nullptr;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(fam.c_str(), coll, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"", fmt.Put())))
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

ID2D1GradientStopCollection* Gfx::Gradient(Color from, Color to) {
    // Gradient stops are device resources; creating them per draw call is costly, so they are
    // cached by colour pair (bounded: themes use a few dozen pairs).
    auto q = [](float v) { return static_cast<uint64_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    uint64_t key = q(from.r) | q(from.g) << 8 | q(from.b) << 16 | q(from.a) << 24 | q(to.r) << 32 | q(to.g) << 40 | q(to.b) << 48 | q(to.a) << 56;
    auto it = gradients_.find(key);
    if (it != gradients_.end()) return it->second.Get();
    if (gradients_.size() > 256) gradients_.clear();
    D2D1_GRADIENT_STOP stops[2] = {{0.f, from.D2D()}, {1.f, to.D2D()}};
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, coll.Put()))) return nullptr;
    ID2D1GradientStopCollection* raw = coll.Get();
    gradients_[key] = std::move(coll);
    return raw;
}

void Gfx::CachedLayer(int slot, uint64_t key, const Rect& r, const std::function<void()>& paint) {
    if (r.w <= 0 || r.h <= 0) return;
    Layer& L = layers_[slot];
    if (!L.bmp || L.key != key || L.w != r.w || L.h != r.h || L.dpi != dpi_ || L.gen != generation_) {
        L = {};
        if (!hwndRt_ || rt_ != hwndRt_.Get() ||
            FAILED(hwndRt_->CreateCompatibleRenderTarget(D2D1::SizeF(r.w, r.h), L.rt.Put()))) {
            paint();
            return;
        }
        ID2D1RenderTarget* saved = rt_;
        std::vector<Rect> savedClips;
        savedClips.swap(clips_);
        rt_ = L.rt.Get();
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(-r.x, -r.y));
        paint();
        HRESULT hr = rt_->EndDraw();
        rt_ = saved;
        clips_.swap(savedClips);
        if (FAILED(hr) || FAILED(L.rt->GetBitmap(L.bmp.Put()))) {
            L = {};
            paint();
            return;
        }
        L.key = key;
        L.w = r.w;
        L.h = r.h;
        L.dpi = dpi_;
        L.gen = generation_;
    }
    rt_->DrawBitmap(L.bmp.Get(), r.D2D(), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

void Gfx::FillGradient(const Rect& r, float radius, Color from, Color to, bool vertical) {
    if (r.w <= 0 || r.h <= 0) return;
    ID2D1GradientStopCollection* coll = Gradient(from, to);
    if (!coll) return;
    ComPtr<ID2D1LinearGradientBrush> br;
    D2D1_POINT_2F a{r.x, r.y}, b = vertical ? D2D1_POINT_2F{r.x, r.B()} : D2D1_POINT_2F{r.R(), r.y};
    if (FAILED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll, br.Put()))) return;
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), radius, radius), br.Get());
    else rt_->FillRectangle(r.D2D(), br.Get());
}

void Gfx::Glow(float cx, float cy, float rx, float ry, Color center) {
    // Stacked translucent ellipses: a smooth falloff that renders identically on every Direct2D
    // implementation (and avoids creating gradient resources every frame).
    const int steps = 14;
    float a = center.a / steps * 1.6f;
    for (int i = 0; i < steps; ++i) {
        float f = 1.f - static_cast<float>(i) / steps;
        float k = std::sqrt(f);
        rt_->FillEllipse(D2D1::Ellipse({cx, cy}, rx * k, ry * k), Brush(center.Alpha(a * (0.35f + 0.65f * (1.f - f)))));
    }
}

void Gfx::GlowStroke(const Rect& r, float radius, Color c, float strength) {
    // Layered translucent outlines approximate a blur without requiring Direct2D effects.
    for (int i = 3; i >= 1; --i) {
        float d = i * 1.6f;
        Rect o{r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d};
        StrokeRound(o, radius + d, c.Alpha(c.a * 0.10f * strength / i), 1.6f);
    }
    StrokeRound(r, radius, c, 1.f);
}

void Gfx::SeriesGradient(const Rect& r, const float* v, size_t n, size_t capacity, float maxValue, Color line, Color fillTop, float width) {
    if (n < 2 || capacity < 2 || maxValue <= 0 || r.w <= 2) return;
    n = std::min(n, capacity);
    const float dx = r.w / static_cast<float>(capacity - 1);
    const float x0 = r.R() - dx * static_cast<float>(n - 1);
    auto y = [&](float val) { return r.B() - std::clamp(val / maxValue, 0.f, 1.f) * (r.h - 2) - 1; };
    ComPtr<ID2D1PathGeometry> fillGeo, lineGeo;
    ComPtr<ID2D1GeometrySink> s1, s2;
    if (FAILED(factory_->CreatePathGeometry(fillGeo.Put())) || FAILED(fillGeo->Open(s1.Put()))) return;
    if (FAILED(factory_->CreatePathGeometry(lineGeo.Put())) || FAILED(lineGeo->Open(s2.Put()))) return;
    s1->BeginFigure({x0, r.B()}, D2D1_FIGURE_BEGIN_FILLED);
    s2->BeginFigure({x0, y(v[0])}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 0; i < n; ++i) {
        D2D1_POINT_2F p{x0 + dx * static_cast<float>(i), y(v[i])};
        s1->AddLine(p);
        if (i) s2->AddLine(p);
    }
    s1->AddLine({r.R(), r.B()});
    s1->EndFigure(D2D1_FIGURE_END_CLOSED);
    s2->EndFigure(D2D1_FIGURE_END_OPEN);
    s1->Close();
    s2->Close();
    ID2D1GradientStopCollection* coll = Gradient(fillTop, fillTop.Alpha(0));
    ComPtr<ID2D1LinearGradientBrush> br;
    if (coll && SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.x, r.y}, {r.x, r.B()}), coll, br.Put())))
        rt_->FillGeometry(fillGeo.Get(), br.Get());
    rt_->DrawGeometry(lineGeo.Get(), Brush(line), width);
}

void Gfx::Leaf(float x, float y, float s, Color c) {
    // A leaf pointing to the upper right, with a vein: drawn as geometry so it never depends on fonts.
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
    sink->BeginFigure({x + s * 0.12f, y + s * 0.88f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment({x + s * 0.0f, y + s * 0.30f}, {x + s * 0.55f, y + s * 0.05f}, {x + s * 0.95f, y + s * 0.05f}));
    sink->AddBezier(D2D1::BezierSegment({x + s * 0.95f, y + s * 0.50f}, {x + s * 0.70f, y + s * 0.95f}, {x + s * 0.12f, y + s * 0.88f}));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure({x + s * 0.05f, y + s * 0.98f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({x + s * 0.62f, y + s * 0.40f});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt_->DrawGeometry(geo.Get(), Brush(c), std::max(1.f, s / 11.f));
}

void Gfx::Logo(const Rect& r, Color from, Color to) {
    float rad = r.w * 0.24f;
    FillGradient(r, rad, from, to, false);
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
    auto P = [&](float fx, float fy) { return D2D1_POINT_2F{r.x + r.w * fx, r.y + r.h * fy}; };
    sink->BeginFigure(P(0.14f, 0.60f), D2D1_FIGURE_BEGIN_HOLLOW);
    const float pts[][2] = {{0.32f, 0.60f}, {0.41f, 0.35f}, {0.52f, 0.78f}, {0.62f, 0.45f}, {0.69f, 0.60f}, {0.86f, 0.60f}};
    for (auto& p : pts) sink->AddLine(P(p[0], p[1]));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> style;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0,
                                style.Put());
    rt_->DrawGeometry(geo.Get(), Brush(Color{1, 1, 1, 1}), std::max(1.5f, r.w * 0.08f), style.Get());
}

void Gfx::Chevron(float cx, float cy, float s, bool down, Color c, float width) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
    float h = s / 2;
    if (down) {
        sink->BeginFigure({cx - h, cy - h / 2}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine({cx, cy + h / 2});
        sink->AddLine({cx + h, cy - h / 2});
    } else {
        sink->BeginFigure({cx - h / 2, cy - h}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine({cx + h / 2, cy});
        sink->AddLine({cx - h / 2, cy + h});
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> style;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0,
                                style.Put());
    rt_->DrawGeometry(geo.Get(), Brush(c), width, style.Get());
}

void Gfx::DrawIcon(MetricIcon icon, const Rect& r, Color c, float w) {
    float x = r.x, y = r.y, s = std::min(r.w, r.h);
    auto P = [&](float fx, float fy) { return D2D1_POINT_2F{x + s * fx, y + s * fy}; };
    auto line = [&](float x1, float y1, float x2, float y2) { rt_->DrawLine(P(x1, y1), P(x2, y2), Brush(c), w); };
    auto rrect = [&](float x1, float y1, float x2, float y2, float rad) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect({x + s * x1, y + s * y1, x + s * x2, y + s * y2}, s * rad, s * rad);
        rt_->DrawRoundedRectangle(rr, Brush(c), w);
    };
    switch (icon) {
    case MetricIcon::Cpu:
        rrect(0.22f, 0.22f, 0.78f, 0.78f, 0.08f);
        rrect(0.38f, 0.38f, 0.62f, 0.62f, 0.03f);
        for (float f : {0.36f, 0.5f, 0.64f}) {
            line(f, 0.08f, f, 0.22f); line(f, 0.78f, f, 0.92f);
            line(0.08f, f, 0.22f, f); line(0.78f, f, 0.92f, f);
        }
        break;
    case MetricIcon::Memory:
        rrect(0.10f, 0.28f, 0.90f, 0.64f, 0.05f);
        for (float f : {0.30f, 0.50f, 0.70f}) rrect(f - 0.06f, 0.37f, f + 0.06f, 0.52f, 0.01f);
        for (float f : {0.2f, 0.35f, 0.5f, 0.65f, 0.8f}) line(f, 0.64f, f, 0.76f);
        break;
    case MetricIcon::Disk:
        rrect(0.16f, 0.18f, 0.84f, 0.82f, 0.12f);
        line(0.16f, 0.58f, 0.84f, 0.58f);
        rt_->FillEllipse(D2D1::Ellipse(P(0.68f, 0.70f), s * 0.045f, s * 0.045f), Brush(c));
        line(0.28f, 0.70f, 0.48f, 0.70f);
        break;
    case MetricIcon::Network:
        rt_->DrawEllipse(D2D1::Ellipse(P(0.5f, 0.24f), s * 0.11f, s * 0.11f), Brush(c), w);
        rt_->DrawEllipse(D2D1::Ellipse(P(0.24f, 0.74f), s * 0.11f, s * 0.11f), Brush(c), w);
        rt_->DrawEllipse(D2D1::Ellipse(P(0.76f, 0.74f), s * 0.11f, s * 0.11f), Brush(c), w);
        line(0.45f, 0.34f, 0.30f, 0.64f); line(0.55f, 0.34f, 0.70f, 0.64f); line(0.35f, 0.74f, 0.65f, 0.74f);
        break;
    case MetricIcon::Gpu:
        rrect(0.08f, 0.26f, 0.92f, 0.70f, 0.06f);
        rt_->DrawEllipse(D2D1::Ellipse(P(0.36f, 0.48f), s * 0.13f, s * 0.13f), Brush(c), w);
        rt_->DrawEllipse(D2D1::Ellipse(P(0.68f, 0.48f), s * 0.13f, s * 0.13f), Brush(c), w);
        line(0.20f, 0.70f, 0.20f, 0.82f); line(0.80f, 0.70f, 0.80f, 0.82f);
        break;
    case MetricIcon::Info:
        rt_->DrawEllipse(D2D1::Ellipse(P(0.5f, 0.5f), s * 0.40f, s * 0.40f), Brush(c), w);
        line(0.5f, 0.46f, 0.5f, 0.72f);
        rt_->FillEllipse(D2D1::Ellipse(P(0.5f, 0.32f), s * 0.05f, s * 0.05f), Brush(c));
        break;
    case MetricIcon::Bolt: {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
        sink->BeginFigure(P(0.58f, 0.06f), D2D1_FIGURE_BEGIN_FILLED);
        for (auto pt : {P(0.22f, 0.56f), P(0.48f, 0.56f), P(0.40f, 0.94f), P(0.78f, 0.42f), P(0.52f, 0.42f)}) sink->AddLine(pt);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt_->FillGeometry(geo.Get(), Brush(c));
        break;
    }
    default: break;
    }
}

void Gfx::IconTile(MetricIcon icon, const Rect& r, Color tint, bool dark) {
    FillGradient(r, r.w * 0.28f, tint.Alpha(dark ? 0.55f : 0.22f), tint.Alpha(dark ? 0.22f : 0.10f));
    StrokeRound(r, r.w * 0.28f, tint.Alpha(dark ? 0.55f : 0.35f));
    float p = r.w * 0.2f;
    DrawIcon(icon, {r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, dark ? Color{1, 1, 1, 0.95f} : tint, std::max(1.2f, r.w / 18.f));
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
