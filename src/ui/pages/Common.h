#pragma once
// Helpers shared by the page implementations.
#include "app/App.h"
#include "core/Filter.h"
#include "data/ProcessOps.h"
#include "data/Processes.h"
#include "ui/Graph.h"
#include "ui/Grid.h"
#include "ui/Table.h"

#include <algorithm>
#include <cmath>

namespace nx {

inline FilterSubject MakeSubject(const ProcSample& p, const std::wstring* services = nullptr) {
    FilterSubject s;
    s.pid = p.pid;
    s.ppid = p.ppid;
    if (p.meta) {
        s.name = &p.meta->name;
        s.description = services && !services->empty() ? services : &p.meta->description;
        s.publisher = &p.meta->company;
        s.path = &p.meta->path;
        s.user = &p.meta->user;
        s.commandLine = &p.meta->commandLine;
    }
    s.cpu = p.cpu;
    s.gpu = p.gpu;
    s.ioRate = p.ioReadRate + p.ioWriteRate;
    s.memory = p.privateWorkingSet ? p.privateWorkingSet : p.privateBytes;
    s.threads = p.threads;
    s.handles = p.handles;
    return s;
}

inline const wchar_t* PowerBand(float score) {
    if (score < 0.1f) return L"Very low";
    if (score < 2.f) return L"Low";
    if (score < 10.f) return L"Moderate";
    if (score < 30.f) return L"High";
    return L"Very high";
}

inline float PowerHeat(float score) { return std::clamp(score / 30.f, 0.f, 1.f); }

// Draws a section header with optional right-side caption; returns the next y.
inline float SectionHeader(Gfx& g, const Theme& t, float x, float y, float w, const std::wstring& title, const std::wstring& right = L"") {
    g.Text(title, {x, y, w * 0.6f, 26}, Font::Subtitle, t.text);
    if (!right.empty()) g.Text(right, {x + w * 0.4f, y, w * 0.6f, 26}, Font::Small, t.textSecondary, Align::Right);
    return y + 32;
}

// Wraps long text into a card-like info block; returns height used.
inline float InfoText(Gfx& g, const Theme& t, const Rect& r, const std::wstring& text, Color c = {}) {
    return g.Paragraph(text, r, Font::Small, c.a > 0 ? c : t.textSecondary);
}

inline std::wstring TempText(App& app, double c) { return c > -50 ? FormatTemp(c, app.TempUnits()) : L"—"; }

// Column chooser (header right-click) for any Table.
inline void ColumnMenu(App& app, Table& t) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < t.columns.size(); ++i) items.push_back({static_cast<UINT>(100 + i), t.columns[i].title, t.columns[i].visible, i != 0});
    items.push_back(MenuItem::Sep());
    items.push_back({1, L"Heat-map shading", t.heatMap});
    items.push_back({2, L"Compact rows", t.compact});
    UINT c = app.ShowMenu(items);
    if (c >= 100 && c < 100 + t.columns.size()) t.columns[c - 100].visible = !t.columns[c - 100].visible;
    if (c == 1) t.heatMap = !t.heatMap;
    if (c == 2) t.compact = !t.compact;
}

// Table metadata that cannot be expressed in the TableColumn.
inline TableColumn HeatCol(const char* key, const wchar_t* title, float w, const wchar_t* tooltip = L"") {
    TableColumn c = Col(key, title, w, Align::Right);
    c.heat = true;
    c.tooltip = tooltip;
    return c;
}

} // namespace nx
