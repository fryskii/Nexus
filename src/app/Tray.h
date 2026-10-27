#pragma once
#include "core/DiagnosticsRules.h"
#include "data/Model.h"

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace nx {

class App;

class Tray {
public:
    explicit Tray(App& app) : app_(app) {}
    ~Tray();
    void Show();
    void Hide();
    bool Visible() const { return visible_; }
    void Update(const Snapshot& s);
    void OnMessage(WPARAM wp, LPARAM lp);
    void NotifyAlerts(const std::vector<DiagAlert>& alerts);

private:
    HICON MakeIcon(float cpu, float mem);
    App& app_;
    bool visible_ = false;
    HICON icon_ = nullptr;
    int lastCpuBucket_ = -1, lastMemBucket_ = -1;
    std::map<std::wstring, uint64_t> notified_;
    UINT taskbarCreated_ = 0;
};

} // namespace nx
