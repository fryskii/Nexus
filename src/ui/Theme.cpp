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
    Color acc = Color::Hex(accentRgb ? accentRgb : AccentFromWindows());
    if (t.dark) {
        // Keep the accent readable on dark surfaces.
        t.accent = Luma(acc) < 0.35f ? Lighten(acc, 0.45f) : acc;
        t.accentText = Lighten(t.accent, 0.25f);
        t.window = Color::Hex(0x202020);
        t.nav = micaAvailable ? Color{0, 0, 0, 0} : Color::Hex(0x1B1B1B);
        t.surface = Color::Hex(0x272727);
        t.surfaceAlt = Color::Hex(0x2C2C2C);
        t.surfaceHover = Color{1, 1, 1, 0.06f};
        t.surfacePressed = Color{1, 1, 1, 0.04f};
        t.card = Color::Hex(0x2B2B2B);
        t.cardBorder = Color{1, 1, 1, 0.07f};
        t.divider = Color{1, 1, 1, 0.08f};
        t.text = Color::Hex(0xFFFFFF);
        t.textSecondary = Color{1, 1, 1, 0.77f};
        t.textTertiary = Color{1, 1, 1, 0.54f};
        t.textDisabled = Color{1, 1, 1, 0.36f};
        t.selection = t.accent.Alpha(0.22f);
        t.selectionBorder = t.accent.Alpha(0.6f);
        t.hover = Color{1, 1, 1, 0.05f};
        t.good = Color::Hex(0x6CCB5F);
        t.warning = Color::Hex(0xFCE100);
        t.critical = Color::Hex(0xFF99A4);
        t.info = Color::Hex(0x60CDFF);
        t.heatLow = Color::Hex(0xB07D00, 0.0f);
        t.heatHigh = Color::Hex(0xC27C0E, 0.55f);
        t.cpu = Color::Hex(0x4CA8FF);
        t.memory = Color::Hex(0xC08CFF);
        t.disk = Color::Hex(0x5CCB8A);
        t.network = Color::Hex(0xF5A742);
        t.gpu = Color::Hex(0x3ED2E0);
        t.temp = Color::Hex(0xFF7A6B);
        t.graphGrid = Color{1, 1, 1, 0.06f};
        t.graphBg = Color{0, 0, 0, 0.12f};
        t.tooltipBg = Color::Hex(0x2C2C2C);
        t.tooltipText = t.text;
        t.onAccent = Luma(t.accent) > 0.5f ? Color::Hex(0x000000) : Color::Hex(0xFFFFFF);
    } else {
        t.accent = Luma(acc) > 0.6f ? Darken(acc, 0.4f) : acc;
        t.accentText = Darken(t.accent, 0.15f);
        t.window = Color::Hex(0xF3F3F3);
        t.nav = micaAvailable ? Color{0, 0, 0, 0} : Color::Hex(0xEEEEEE);
        t.surface = Color::Hex(0xFBFBFB);
        t.surfaceAlt = Color::Hex(0xF5F5F5);
        t.surfaceHover = Color{0, 0, 0, 0.04f};
        t.surfacePressed = Color{0, 0, 0, 0.06f};
        t.card = Color::Hex(0xFFFFFF);
        t.cardBorder = Color{0, 0, 0, 0.08f};
        t.divider = Color{0, 0, 0, 0.08f};
        t.text = Color{0, 0, 0, 0.9f};
        t.textSecondary = Color{0, 0, 0, 0.62f};
        t.textTertiary = Color{0, 0, 0, 0.45f};
        t.textDisabled = Color{0, 0, 0, 0.36f};
        t.selection = t.accent.Alpha(0.14f);
        t.selectionBorder = t.accent.Alpha(0.5f);
        t.hover = Color{0, 0, 0, 0.035f};
        t.good = Color::Hex(0x0F7B0F);
        t.warning = Color::Hex(0x9D5D00);
        t.critical = Color::Hex(0xC42B1C);
        t.info = Color::Hex(0x005FB8);
        t.heatLow = Color::Hex(0xFFF4CE, 0.0f);
        t.heatHigh = Color::Hex(0xFFB900, 0.55f);
        t.cpu = Color::Hex(0x0F6CBD);
        t.memory = Color::Hex(0x8B3FC9);
        t.disk = Color::Hex(0x2E8B57);
        t.network = Color::Hex(0xC86400);
        t.gpu = Color::Hex(0x0E8A99);
        t.temp = Color::Hex(0xC4314B);
        t.graphGrid = Color{0, 0, 0, 0.06f};
        t.graphBg = Color{0, 0, 0, 0.02f};
        t.tooltipBg = Color::Hex(0xF9F9F9);
        t.tooltipText = t.text;
        t.onAccent = Luma(t.accent) > 0.5f ? Color::Hex(0x000000) : Color::Hex(0xFFFFFF);
    }
    return t;
}

Color Theme::Heat(float v) const {
    v = std::clamp(v, 0.f, 1.f);
    return heatLow.Mix(heatHigh, v);
}

} // namespace nx
