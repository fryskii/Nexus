#pragma once
// Virtualized, sortable table with resizable/reorderable columns, tree rows, group headers and
// Task Manager style heat-map cells. Only visible rows are formatted and drawn.
#include "ui/Widgets.h"

#include <d2d1.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace nx {

struct TableColumn {
    std::string key;
    std::wstring title;
    float width = 100;
    Align align = Align::Left;
    bool visible = true;
    bool heat = false;        // shade cells by CellHeat()
    std::wstring tooltip;
    std::wstring header2;     // header value, e.g. total "34%"; shown as a metric pill when `icon` is set
    MetricIcon icon = MetricIcon::None;
    Color tint = Color::None(); // pill / icon tint (none: accent)
    bool heatAlways = false;  // draw the heat block even at zero load (CPU / Memory columns)
};

class TableSource {
public:
    virtual ~TableSource() = default;
    virtual size_t RowCount() const = 0;
    virtual void CellText(size_t row, int col, std::wstring& out) const = 0;
    virtual uint64_t RowKey(size_t row) const = 0;
    virtual float CellHeat(size_t /*row*/, int /*col*/) const { return -1.f; }
    virtual int RowDepth(size_t /*row*/) const { return 0; }
    virtual int RowExpander(size_t /*row*/) const { return 0; } // 0 none, 1 collapsed, 2 expanded
    virtual void ToggleExpand(size_t /*row*/) {}
    virtual ID2D1Bitmap* RowIcon(size_t /*row*/) { return nullptr; }
    virtual bool RowIsGroup(size_t /*row*/) const { return false; }
    // 0 normal, 1 new (green), 2 exiting (red), 3 dimmed (suspended / disabled)
    virtual int RowHighlight(size_t /*row*/) const { return 0; }
    // Text colour override; alpha 0 (the default) means "use the normal text colour".
    virtual Color CellColor(size_t /*row*/, int /*col*/, const Theme& /*t*/) const { return Color{0, 0, 0, 0}; }
    // Small badge drawn before the cell text: 1 = efficiency-mode leaf.
    virtual int CellBadge(size_t /*row*/, int /*col*/) const { return 0; }
};

class Table {
public:
    explicit Table(WidgetId id) : id_(id) {}
    std::vector<TableColumn> columns;
    int sortCol = -1;
    bool sortDesc = true;
    float scrollY = 0, scrollX = 0; // scrollX: columns wider than the panel scroll sideways
    uint64_t selectedKey = 0;
    bool compact = false;
    bool showIcons = true;
    bool heatMap = true;
    bool fillFirst = false; // first visible column absorbs the remaining width

    // Results of the last Draw() (consumed by the owner after the frame).
    bool activated = false, contextMenu = false, headerMenu = false, sortChanged = false, selectionChanged = false;
    int headerMenuColumn = -1;

    void Draw(Ui& ui, const Rect& r, TableSource& src);
    // Keyboard navigation. Returns true if handled.
    bool OnKey(UINT vk, TableSource& src);
    int SelectedRow(const TableSource& src) const;
    void SelectRow(size_t row, const TableSource& src);
    void EnsureVisibleNext() { ensureVisible_ = true; }
    float RowHeight() const { return compact ? 27.f : 34.f; }
    int ColumnIndex(const std::string& key) const;
    const std::string& SortKey() const;

    std::wstring SaveLayout() const;
    void LoadLayout(const std::wstring& s);

private:
    void DrawImpl(Ui& ui, const Rect& r, TableSource& src);
    bool HasPills() const;
    float MinWidth(Gfx& g, const TableColumn& c, bool sorted, bool tall); // narrowest width at which the header title still fits
    void HeaderTitle(Gfx& g, const Theme& t, int ci, const Rect& r, bool sorted, bool tall);
    struct HeaderFit { // how a header title is laid out, recomputed when its inputs change
        std::wstring title, lines[2];
        float w = -1, textW = 0;
        bool sorted = false, tall = false;
        Font font = Font::Caption;
    };
    std::vector<HeaderFit> headerFit_;
    WidgetId id_;
    float viewH_ = 0;
    bool ensureVisible_ = false;
    int resizing_ = -1, dragging_ = -1;
    float resizeStartX_ = 0, resizeStartW_ = 0, dragStartX_ = 0, sbDragY_ = 0, sbDragOff_ = 0, hsbDragX_ = 0, hsbDragOff_ = 0;
    mutable std::wstring cell_;
    std::vector<int> order_; // display order -> column index
    std::unordered_map<std::wstring, std::pair<float, float>> titleMinW_; // title width on one line / two lines (Caption)
    void EnsureOrder();
};

} // namespace nx
