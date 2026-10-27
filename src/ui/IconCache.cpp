#include "ui/IconCache.h"
#include "core/Format.h"
#include "data/Worker.h"

#include <shellapi.h>

namespace nx {

void IconCache::Init(Gfx* g, HWND notify, UINT msg) {
    g_ = g;
    notify_ = notify;
    msg_ = msg;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, wic_.PutVoid());
}

void IconCache::Shutdown() {
    std::lock_guard<std::mutex> l(lock_);
    map_.clear();
    default_.reset();
    wic_.Reset();
}

void IconCache::Clear() {
    std::lock_guard<std::mutex> l(lock_);
    map_.clear();
    default_.reset();
}

ID2D1Bitmap* IconCache::Realize(Entry& e) {
    if (!e.wic) return nullptr;
    if (e.d2d && e.gen == g_->DeviceGeneration()) return e.d2d.Get();
    e.d2d.Reset();
    if (FAILED(g_->RT()->CreateBitmapFromWicBitmap(e.wic.Get(), nullptr, e.d2d.Put()))) return nullptr;
    e.gen = g_->DeviceGeneration();
    return e.d2d.Get();
}

ID2D1Bitmap* IconCache::Default() {
    if (!wic_) return nullptr;
    if (!default_) {
        default_ = std::make_shared<Entry>();
        HICON ic = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, px_, px_, LR_SHARED));
        if (ic) {
            ComPtr<IWICBitmap> bmp;
            if (SUCCEEDED(wic_->CreateBitmapFromHICON(ic, bmp.Put()))) {
                ComPtr<IWICFormatConverter> conv;
                if (SUCCEEDED(wic_->CreateFormatConverter(conv.Put())) &&
                    SUCCEEDED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                    wic_->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, default_->wic.Put());
            }
        }
    }
    return Realize(*default_);
}

ID2D1Bitmap* IconCache::Get(const std::wstring& path) {
    if (path.empty() || !wic_) return Default();
    std::wstring key = ToLower(path);
    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> l(lock_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            e = std::make_shared<Entry>();
            e->loading = true;
            map_[key] = e;
        } else {
            e = it->second;
            if (e->loading) return Default();
            if (e->failed) return Default();
            return Realize(*e);
        }
    }
    // Extract on a worker; WIC factories are free-threaded.
    ComPtr<IWICImagingFactory> wic = wic_;
    int px = px_;
    HWND notify = notify_;
    UINT msg = msg_;
    worker::Run([e, path, wic, px]() mutable {
        HICON ic = nullptr;
        UINT got = PrivateExtractIconsW(path.c_str(), 0, px, px, &ic, nullptr, 1, LR_DEFAULTCOLOR);
        if (got == 0 || got == 0xFFFFFFFF || !ic) {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_SMALLICON)) ic = sfi.hIcon;
        }
        if (ic) {
            ComPtr<IWICBitmap> bmp;
            if (SUCCEEDED(wic->CreateBitmapFromHICON(ic, bmp.Put()))) {
                ComPtr<IWICFormatConverter> conv;
                ComPtr<IWICBitmap> out;
                if (SUCCEEDED(wic->CreateFormatConverter(conv.Put())) &&
                    SUCCEEDED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, out.Put())))
                    e->wic = out;
            }
            DestroyIcon(ic);
        }
    }, [this, e, notify, msg] {
        e->failed = !e->wic;
        e->loading = false;
        InvalidateRect(notify, nullptr, FALSE);
        (void)msg;
    });
    return Default();
}

} // namespace nx
