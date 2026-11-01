#pragma once
#include "ui/Gfx.h"

namespace nx {

enum class ThemeMode { System, Light, Dark };

// How the window is drawn. Nexus: gradients, glows and glass panels. Simple: flat colours, plain
// header cells and small corners, close to Windows Task Manager (Gfx::SetFlat does the drawing part).
enum class ThemeStyle { Nexus, Simple };

constexpr uint32_t kNoBackground = 0xFFFFFFFF; // "use the style's own background"

struct Theme {
    bool dark = true;
    bool simple = false;
    bool mica = false;          // kept for compatibility; the window paints its own background
    Color accent, accentText, onAccent, accentDeep;
    // Window background: vertical gradient plus soft glows.
    Color bgTop, bgBottom, glowA, glowB;
    // Glass panels (content area, cards, table header pills).
    Color panel, panelBorder, panelHighlight;
    Color window, nav, surface, surfaceAlt, surfaceHover, surfacePressed, card, cardBorder, divider;
    Color text, textSecondary, textTertiary, textDisabled;
    Color selection, selectionBorder, hover;
    Color searchBg, searchBorder;
    Color good, warning, critical, info;
    Color heatLow, heatHigh;   // table heat cells
    Color cpu, memory, disk, network, gpu, temp;
    Color graphGrid, graphBg;
    Color tooltipBg, tooltipText;

    // Builds the palette for the given mode and accent (0x00RRGGBB; 0 = NEXUS blue). A background
    // (0x00RRGGBB, or kNoBackground) replaces the window colour; light or dark text is then chosen
    // by whichever reads better on it, so `mode` is ignored.
    static Theme Make(ThemeMode mode, uint32_t accentRgb, bool micaAvailable, ThemeStyle style = ThemeStyle::Nexus,
                      uint32_t backgroundRgb = kNoBackground);
    Color Heat(float t) const; // t in [0,1]
};

uint32_t AccentFromWindows(); // 0x00RRGGBB
// True when white text has more contrast than near-black text on this colour (WCAG contrast ratio).
bool WantsDarkPalette(uint32_t rgb);
// The plain window colour each style uses when no background is chosen.
uint32_t DefaultBackground(ThemeStyle style, bool dark);

} // namespace nx
