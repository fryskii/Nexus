#pragma once
// Real-time line/area graph with hover read-out, Ctrl+wheel zoom, drag-to-pan and double-click reset.
#include "core/RingBuffer.h"
#include "ui/Widgets.h"

#include <functional>
#include <string>
#include <vector>

namespace nx {

struct GraphLine {
    const RingBuffer<float>* data = nullptr;
    Color color;
    bool fill = true;
    std::wstring name;
    bool dashed = false;
};

class GraphView {
public:
    size_t zoom = 0;  // visible samples; 0 = full buffer capacity
    size_t pan = 0;   // samples back from the newest
    // Returns true if the user interacted (so callers can request a repaint).
    void Draw(Ui& ui, WidgetId id, const Rect& r, const std::vector<GraphLine>& lines, float maxValue, double intervalSec,
              const std::function<std::wstring(float)>& fmt, bool grid = true, bool timeAxis = false);
    // Mini sparkline without interaction.
    static void Spark(Gfx& g, const Rect& r, const RingBuffer<float>& data, float maxValue, Color c, bool fill = true);

private:
    float dragX_ = 0;
    size_t dragPan_ = 0;
    std::vector<float> tmp_;
};

} // namespace nx
