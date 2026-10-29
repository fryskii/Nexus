#pragma once
// Generic row/column data source for Table: string cells with optional numeric sort keys,
// substring filtering and stable sorting. Used by most list views.
#include "core/Format.h"
#include "ui/IconCache.h"
#include "ui/Table.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace nx {

struct GridRow {
    uint64_t key = 0;
    std::vector<std::wstring> cells;
    std::vector<double> nums;   // NaN = sort by text
    int highlight = 0;          // see TableSource::RowHighlight
    std::wstring iconPath;
    bool group = false;
    int depth = 0;
    Color color = Color::None(); // optional text color for column `colorCol`
    int colorCol = -1;
};

inline double NoNum() { return std::numeric_limits<double>::quiet_NaN(); }

class SimpleGrid : public TableSource {
public:
    std::vector<GridRow> rows;
    IconCache* icons = nullptr;

    void SetFilter(const std::wstring& f) { filter_ = f; dirty_ = true; }
    void Refresh(const Table& t) {
        view_.clear();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!filter_.empty()) {
                bool any = false;
                for (auto& c : rows[i].cells) if (ContainsI(c, filter_)) { any = true; break; }
                if (!any) continue;
            }
            view_.push_back(i);
        }
        int col = t.sortCol;
        bool desc = t.sortDesc;
        if (col >= 0) {
            std::stable_sort(view_.begin(), view_.end(), [&](size_t a, size_t b) {
                const GridRow& A = rows[a];
                const GridRow& B = rows[b];
                double na = col < static_cast<int>(A.nums.size()) ? A.nums[col] : NoNum();
                double nb = col < static_cast<int>(B.nums.size()) ? B.nums[col] : NoNum();
                int c;
                if (!std::isnan(na) && !std::isnan(nb)) c = na < nb ? -1 : na > nb ? 1 : 0;
                else {
                    const std::wstring& sa = col < static_cast<int>(A.cells.size()) ? A.cells[col] : empty_;
                    const std::wstring& sb = col < static_cast<int>(B.cells.size()) ? B.cells[col] : empty_;
                    c = _wcsicmp(sa.c_str(), sb.c_str());
                }
                return desc ? c > 0 : c < 0;
            });
        }
        dirty_ = false;
    }
    bool Dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }
    const GridRow* RowAt(size_t viewRow) const { return viewRow < view_.size() ? &rows[view_[viewRow]] : nullptr; }
    const GridRow* Find(uint64_t key) const {
        for (auto& r : rows) if (r.key == key) return &r;
        return nullptr;
    }

    size_t RowCount() const override { return view_.size(); }
    void CellText(size_t row, int col, std::wstring& out) const override {
        const GridRow& r = rows[view_[row]];
        out = col < static_cast<int>(r.cells.size()) ? r.cells[col] : empty_;
    }
    uint64_t RowKey(size_t row) const override { return rows[view_[row]].key; }
    int RowHighlight(size_t row) const override { return rows[view_[row]].highlight; }
    bool RowIsGroup(size_t row) const override { return rows[view_[row]].group; }
    int RowDepth(size_t row) const override { return rows[view_[row]].depth; }
    ID2D1Bitmap* RowIcon(size_t row) override {
        if (!icons) return nullptr;
        const GridRow& r = rows[view_[row]];
        return icons->Get(r.iconPath);
    }
    Color CellColor(size_t row, int col, const Theme&) const override {
        const GridRow& r = rows[view_[row]];
        return r.colorCol == col ? r.color : Color{0, 0, 0, 0};
    }

private:
    std::vector<size_t> view_;
    std::wstring filter_;
    bool dirty_ = true;
    std::wstring empty_;
};

inline TableColumn Col(const char* key, const wchar_t* title, float width, Align a = Align::Left, bool visible = true) {
    TableColumn c;
    c.key = key;
    c.title = title;
    c.width = width;
    c.align = a;
    c.visible = visible;
    return c;
}

} // namespace nx
