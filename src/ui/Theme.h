#pragma once
#include "ui/Gfx.h"

namespace nx {

enum class ThemeMode { System, Light, Dark };

struct Theme {
    bool dark = true;
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

    // Builds the palette for the given mode and accent (0x00RRGGBB; 0 = NEXUS blue).
    static Theme Make(ThemeMode mode, uint32_t accentRgb, bool micaAvailable);
    Color Heat(float t) const; // t in [0,1]
};

uint32_t AccentFromWindows(); // 0x00RRGGBB

} // namespace nx
