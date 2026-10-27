#pragma once
#include "ui/Gfx.h"

namespace nx {

enum class ThemeMode { System, Light, Dark };

struct Theme {
    bool dark = true;
    bool mica = false;          // backdrop visible behind the navigation pane
    Color accent, accentText, onAccent;
    Color window, nav, surface, surfaceAlt, surfaceHover, surfacePressed, card, cardBorder, divider;
    Color text, textSecondary, textTertiary, textDisabled;
    Color selection, selectionBorder, hover;
    Color good, warning, critical, info;
    Color heatLow, heatHigh;   // table heat-map shading
    Color cpu, memory, disk, network, gpu, temp;
    Color graphGrid, graphBg;
    Color tooltipBg, tooltipText;

    // Builds the palette for the given mode and accent (0x00RRGGBB; 0 = Windows accent).
    static Theme Make(ThemeMode mode, uint32_t accentRgb, bool micaAvailable);
    Color Heat(float t) const; // t in [0,1]
};

uint32_t AccentFromWindows(); // 0x00RRGGBB

} // namespace nx
