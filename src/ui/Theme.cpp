#include "ui/Theme.h"

#include <algorithm>

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
} // namespace

Theme Theme::Make(ThemeMode mode, uint32_t accentRgb, bool micaAvailable) {
    Theme t;
    t.dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && IsWindowsDarkMode());
    t.mica = micaAvailable;
    // NEXUS's signature blue unless the user picked another accent.
    Color acc = Color::Hex(accentRgb ? accentRgb : 0x2B7BFF);
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
    return t;
}

Color Theme::Heat(float v) const {
    v = std::clamp(v, 0.f, 1.f);
    return heatLow.Mix(heatHigh, v);
}

} // namespace nx
