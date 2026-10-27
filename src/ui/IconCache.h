#pragma once
// Executable icon cache. Icons are extracted on a worker thread and converted to D2D bitmaps on
// the UI thread; device loss simply drops the D2D side.
#include "ui/Gfx.h"

#include <wincodec.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nx {

class IconCache {
public:
    void Init(Gfx* g, HWND notify, UINT msgRepaint);
    void Shutdown();
    // Returns nullptr until loaded (a repaint is requested when it becomes available).
    ID2D1Bitmap* Get(const std::wstring& path);
    ID2D1Bitmap* Default();
    void SetPixelSize(int px) { if (px != px_) { px_ = px; Clear(); } }
    void Clear();

private:
    struct Entry {
        ComPtr<IWICBitmap> wic;
        ComPtr<ID2D1Bitmap> d2d;
        uint32_t gen = 0;
        bool loading = false, failed = false;
    };
    ID2D1Bitmap* Realize(Entry& e);
    Gfx* g_ = nullptr;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    int px_ = 16;
    ComPtr<IWICImagingFactory> wic_;
    std::mutex lock_;
    std::unordered_map<std::wstring, std::shared_ptr<Entry>> map_;
    std::shared_ptr<Entry> default_;
};

} // namespace nx
