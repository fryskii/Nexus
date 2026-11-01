#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace nx {

uint32_t AccentFromWindows() {
    DWORD abgr = GetWindowsAccentColor();
    if (!abgr) return 0x0067C0; // Windows 11 default blue
    uint32_t r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

namespace {
float Luma(Color c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
Color Lighten(Color c, float t) { return c.Mix(Color{1, 1, 1, c.a}, t); }
Color Darken(Color c, float t) { return c.Mix(Color{0, 0, 0, c.a}, t); }

// WCAG relative luminance of an sRGB colour.
float Luminance(uint32_t rgb) {
    auto lin = [](uint32_t v) {
        float c = static_cast<float>(v & 0xFF) / 255.f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(rgb >> 16) + 0.7152f * lin(rgb >> 8) + 0.0722f * lin(rgb);
}

// Flat palette close to Windows Task Manager, built around the window colour `bg`: surfaces are
// small steps from it and secondary text is the main text mixed towards it, so any background
// keeps the same contrast steps.
void MakeSimple(Theme& t, Color bg, Color acc) {
    const Color white{1, 1, 1, 1}, black{0, 0, 0, 1};
    const Color up = t.dark ? white : black; // towards the text colour
    t.accent = t.dark ? (Luma(acc) < 0.25f ? Lighten(acc, 0.35f) : acc) : (Luma(acc) > 0.6f ? Darken(acc, 0.4f) : acc);
    t.accentText = t.dark ? Lighten(t.accent, 0.35f) : Darken(t.accent, 0.15f);
    t.accentDeep = Darken(t.accent, t.dark ? 0.35f : 0.25f);
    t.onAccent = white;
    t.bgTop = t.bgBottom = bg;
    t.glowA = t.glowB = Color::None();
    t.window = bg;
    t.nav = Color::None();
    t.panel = t.dark ? bg.Mix(white, 0.035f) : bg.Mix(white, 0.6f);
    t.panelBorder = up.Alpha(t.dark ? 0.09f : 0.08f);
    t.panelHighlight = Color::None();
    t.surface = t.dark ? bg.Mix(white, 0.06f) : bg.Mix(white, 0.85f);
    t.surfaceAlt = t.dark ? bg.Mix(white, 0.09f) : bg.Mix(black, 0.03f);
    t.surfaceHover = up.Alpha(0.06f);
    t.surfacePressed = up.Alpha(0.04f);
    t.card = t.dark ? bg.Mix(white, 0.05f) : bg.Mix(white, 0.75f);
    t.cardBorder = up.Alpha(0.08f);
    t.divider = up.Alpha(0.08f);
    t.text = t.dark ? Color::Hex(0xFFFFFF) : Color::Hex(0x1A1A1A);
    t.textSecondary = t.text.Mix(bg, 0.28f);
    t.textTertiary = t.text.Mix(bg, 0.45f);
    t.textDisabled = t.text.Mix(bg, 0.62f);
    t.selection = t.accent.Alpha(t.dark ? 0.25f : 0.15f);
    t.selectionBorder = t.accent.Alpha(0.60f);
    t.hover = up.Alpha(0.05f);
    t.searchBg = t.dark ? bg.Mix(white, 0.05f) : bg.Mix(white, 0.9f); // opaque: also the native edit's brush
    t.searchBorder = up.Alpha(0.16f);
    t.good = Color::Hex(t.dark ? 0x6CCB5F : 0x0F7B0F);
    t.warning = Color::Hex(t.dark ? 0xFCE100 : 0x9D5D00);
    t.critical = Color::Hex(t.dark ? 0xFF99A4 : 0xC42B1C);
    t.info = Color::Hex(t.dark ? 0x60CDFF : 0x005FB8);
    // Task Manager's yellow-to-orange usage cells.
    t.heatLow = Color::Hex(t.dark ? 0xC8A000 : 0xFFE58F, t.dark ? 0.10f : 0.35f);
    t.heatHigh = Color::Hex(t.dark ? 0xE08A00 : 0xFFB020, t.dark ? 0.55f : 0.75f);
    t.cpu = Color::Hex(t.dark ? 0x4CA3E8 : 0x1179C9);
    t.memory = Color::Hex(t.dark ? 0xA97FD6 : 0x8B12AE);
    t.disk = Color::Hex(t.dark ? 0x6CCB5F : 0x4DA60C);
    t.network = Color::Hex(t.dark ? 0xE6A35C : 0xA74F01);
    t.gpu = Color::Hex(t.dark ? 0x4CC2C4 : 0x0E8A8C);
    t.temp = Color::Hex(t.dark ? 0xFF8A80 : 0xC42B1C);
    t.graphGrid = up.Alpha(0.08f);
    t.graphBg = Color::None();
    t.tooltipBg = t.dark ? bg.Mix(white, 0.10f) : bg.Mix(white, 0.95f);
    t.tooltipText = t.text;
}
} // namespace

bool WantsDarkPalette(uint32_t rgb) {
    float l = Luminance(rgb & 0xFFFFFF);
    float onWhite = 1.05f / (l + 0.05f);                          // white text
    float onDark = (l + 0.05f) / (Luminance(0x1A1A1A) + 0.05f);   // near-black text
    return onWhite > onDark;
}

uint32_t DefaultBackground(ThemeStyle style, bool dark) {
    if (style == ThemeStyle::Simple) return dark ? 0x202020 : 0xF3F3F3;
    return dark ? 0x060C1A : 0xEEF3FC;
}

Theme Theme::Make(ThemeMode mode, uint32_t accentRgb, bool micaAvailable, ThemeStyle style, uint32_t backgroundRgb) {
    Theme t;
    const bool customBg = backgroundRgb != kNoBackground;
    backgroundRgb &= 0xFFFFFF;
    t.dark = customBg ? WantsDarkPalette(backgroundRgb) : mode == ThemeMode::Dark || (mode == ThemeMode::System && IsWindowsDarkMode());
    t.simple = style == ThemeStyle::Simple;
    t.mica = micaAvailable;
    // NEXUS's signature blue unless the user picked another accent.
    Color acc = Color::Hex(accentRgb ? accentRgb : 0x2B7BFF);
    if (t.simple) {
        MakeSimple(t, Color::Hex(customBg ? backgroundRgb : DefaultBackground(style, t.dark)), acc);
        return t;
    }
    if (t.dark) {
        t.accent = Luma(acc) < 0.25f ? Lighten(acc, 0.35f) : acc;
        t.accentText = Lighten(t.accent, 0.35f);
        t.accentDeep = Darken(t.accent, 0.35f);
        t.onAccent = Color::Hex(0xFFFFFF);
        t.bgTop = Color::Hex(0x060C1A);
        t.bgBottom = Color::Hex(0x0A1630);
        t.glowA = t.accent.Alpha(0.20f);
        t.glowB = Color::Hex(0x6D28D9, 0.10f);
        t.panel = Color::Hex(0x0E1830, 0.82f);
        t.panelBorder = Color::Hex(0x5B8CFF, 0.22f);
        t.panelHighlight = Color::Hex(0xA8C4FF, 0.10f);
        t.window = Color::Hex(0x0B1428);
        t.nav = Color{0, 0, 0, 0};
        t.surface = Color::Hex(0x101C36);
        t.surfaceAlt = Color::Hex(0x132242);
        t.surfaceHover = Color::Hex(0x7FA6FF, 0.08f);
        t.surfacePressed = Color::Hex(0x7FA6FF, 0.05f);
        t.card = Color::Hex(0x111D39, 0.92f);
        t.cardBorder = Color::Hex(0x5B8CFF, 0.20f);
        t.divider = Color::Hex(0x7FA6FF, 0.10f);
        t.text = Color::Hex(0xEEF3FF);
        t.textSecondary = Color::Hex(0xA5B4D4);
        t.textTertiary = Color::Hex(0x6E7FA3);
        t.textDisabled = Color::Hex(0x4C5A78);
        t.selection = t.accent.Alpha(0.20f);
        t.selectionBorder = t.accent.Alpha(0.55f);
        t.hover = Color::Hex(0x7FA6FF, 0.06f);
        t.searchBg = Color::Hex(0x0D1830);
        t.searchBorder = Color::Hex(0x5B8CFF, 0.25f);
        t.good = Color::Hex(0x4ADE80);
        t.warning = Color::Hex(0xFBBF24);
        t.critical = Color::Hex(0xF87171);
        t.info = Color::Hex(0x60A5FA);
        t.heatLow = Color::Hex(0x1E3A8A, 0.32f);
        t.heatHigh = Color::Hex(0x2563EB, 0.85f);
        t.cpu = Color::Hex(0x3B82F6);
        t.memory = Color::Hex(0x7C5CFF);
        t.disk = Color::Hex(0x22C55E);
        t.network = Color::Hex(0x14B8A6);
        t.gpu = Color::Hex(0x8B5CF6);
        t.temp = Color::Hex(0xFB7185);
        t.graphGrid = Color::Hex(0x7FA6FF, 0.07f);
        t.graphBg = Color::Hex(0x050B18, 0.45f);
        t.tooltipBg = Color::Hex(0x15244A);
        t.tooltipText = t.text;
    } else {
        t.accent = Luma(acc) > 0.6f ? Darken(acc, 0.4f) : acc;
        t.accentText = Darken(t.accent, 0.15f);
        t.accentDeep = Darken(t.accent, 0.25f);
        t.onAccent = Color::Hex(0xFFFFFF);
        t.bgTop = Color::Hex(0xEEF3FC);
        t.bgBottom = Color::Hex(0xDCE6F7);
        t.glowA = t.accent.Alpha(0.12f);
        t.glowB = Color::Hex(0x8B5CF6, 0.07f);
        t.panel = Color::Hex(0xFFFFFF, 0.80f);
        t.panelBorder = Color::Hex(0x2B5BD7, 0.16f);
        t.panelHighlight = Color::Hex(0xFFFFFF, 0.70f);
        t.window = Color::Hex(0xF4F7FD);
        t.nav = Color{0, 0, 0, 0};
        t.surface = Color::Hex(0xFFFFFF);
        t.surfaceAlt = Color::Hex(0xF2F6FD);
        t.surfaceHover = Color::Hex(0x2B5BD7, 0.06f);
        t.surfacePressed = Color::Hex(0x2B5BD7, 0.09f);
        t.card = Color::Hex(0xFFFFFF, 0.90f);
        t.cardBorder = Color::Hex(0x2B5BD7, 0.14f);
        t.divider = Color::Hex(0x1E3A8A, 0.09f);
        t.text = Color::Hex(0x0F172A);
        t.textSecondary = Color::Hex(0x475569);
        t.textTertiary = Color::Hex(0x7A8699);
        t.textDisabled = Color::Hex(0xA3ACBA);
        t.selection = t.accent.Alpha(0.12f);
        t.selectionBorder = t.accent.Alpha(0.45f);
        t.hover = Color::Hex(0x2B5BD7, 0.05f);
        t.searchBg = Color::Hex(0xFFFFFF);
        t.searchBorder = Color::Hex(0x2B5BD7, 0.20f);
        t.good = Color::Hex(0x15803D);
        t.warning = Color::Hex(0xB45309);
        t.critical = Color::Hex(0xDC2626);
        t.info = Color::Hex(0x1D4ED8);
        t.heatLow = Color::Hex(0x93B4F5, 0.18f);
        t.heatHigh = Color::Hex(0x2563EB, 0.45f);
        t.cpu = Color::Hex(0x2563EB);
        t.memory = Color::Hex(0x6D4AFF);
        t.disk = Color::Hex(0x16A34A);
        t.network = Color::Hex(0x0D9488);
        t.gpu = Color::Hex(0x7C3AED);
        t.temp = Color::Hex(0xE11D48);
        t.graphGrid = Color::Hex(0x1E3A8A, 0.07f);
        t.graphBg = Color::Hex(0xEEF3FC, 0.8f);
        t.tooltipBg = Color::Hex(0xFFFFFF);
        t.tooltipText = t.text;
    }
    if (customBg) {
        // Keep the gradient and glows, around the chosen colour; tint the glass towards it.
        Color bg = Color::Hex(backgroundRgb);
        t.bgTop = bg;
        t.bgBottom = t.dark ? Lighten(bg, 0.06f) : Darken(bg, 0.07f);
        t.panel = t.panel.Mix(bg.Alpha(t.panel.a), 0.5f);
        t.card = t.card.Mix(bg.Alpha(t.card.a), 0.35f);
        t.window = t.window.Mix(bg, 0.5f);
        t.surface = t.surface.Mix(bg, 0.3f);
        t.surfaceAlt = t.surfaceAlt.Mix(bg, 0.3f);
        t.searchBg = t.searchBg.Mix(bg, 0.4f);
        t.tooltipBg = t.tooltipBg.Mix(bg, 0.3f);
        t.graphBg = t.graphBg.Mix(bg.Alpha(t.graphBg.a), 0.5f);
    }
    return t;
}

Color Theme::Heat(float v) const {
    v = std::clamp(v, 0.f, 1.f);
    return heatLow.Mix(heatHigh, v);
}

} // namespace nx
