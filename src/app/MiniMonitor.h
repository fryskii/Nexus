#pragma once
// Floating, always-on-top compact monitor showing essential live metrics.
#include "ui/Gfx.h"
#include "ui/Widgets.h"

#include <memory>

namespace nx {

class App;

class MiniMonitor {
public:
    explicit MiniMonitor(App& app) : app_(app) {}
    ~MiniMonitor();
    void Show(bool show);
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void Update();
    void OnThemeChanged();
    void Destroy();
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);

private:
    void Create();
    void Paint();
    App& app_;
    HWND hwnd_ = nullptr;
    Gfx gfx_;
    InputState in_;
    std::unique_ptr<Ui> ui_;
};

} // namespace nx
