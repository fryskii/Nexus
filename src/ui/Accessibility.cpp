#include "ui/Accessibility.h"
#include "app/App.h"

#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <vector>

namespace nx::a11y {

namespace {

// Property / control / pattern ids (UIAutomationClient.h constants).
constexpr PROPERTYID kName = 30005, kControlType = 30003, kAutomationId = 30011, kIsKeyboardFocusable = 30009, kHasKeyboardFocus = 30008,
                     kIsEnabled = 30010, kLocalizedControlType = 30004, kHelpText = 30013, kIsContentElement = 30017, kIsControlElement = 30016,
                     kLiveSetting = 30135;
constexpr CONTROLTYPEID kPane = 50033, kList = 50008, kListItem = 50007, kText = 50020;
constexpr PATTERNID kSelectionItemPattern = 10010, kInvokePattern = 10000;
constexpr EVENTID kAutomationFocusChanged = 20005, kSelectionItemSelected = 20012, kLiveRegionChanged = 20024;

// uiautomationcore.dll is loaded on first use (only when an accessibility client queries the
// window), which also keeps it out of memory for everyone else.
struct UiaApi {
    LRESULT(WINAPI* returnProvider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*) = nullptr;
    HRESULT(WINAPI* hostFromHwnd)(HWND, IRawElementProviderSimple**) = nullptr;
    HRESULT(WINAPI* raiseEvent)(IRawElementProviderSimple*, EVENTID) = nullptr;
    BOOL(WINAPI* clientsListening)() = nullptr;
    HRESULT(WINAPI* raiseNotification)(IRawElementProviderSimple*, int, int, BSTR, BSTR) = nullptr;
};
const UiaApi& Uia() {
    static UiaApi api = [] {
        UiaApi a;
        HMODULE m = LoadLibraryExW(L"uiautomationcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) {
            a.returnProvider = reinterpret_cast<decltype(a.returnProvider)>(GetProcAddress(m, "UiaReturnRawElementProvider"));
            a.hostFromHwnd = reinterpret_cast<decltype(a.hostFromHwnd)>(GetProcAddress(m, "UiaHostProviderFromHwnd"));
            a.raiseEvent = reinterpret_cast<decltype(a.raiseEvent)>(GetProcAddress(m, "UiaRaiseAutomationEvent"));
            a.clientsListening = reinterpret_cast<decltype(a.clientsListening)>(GetProcAddress(m, "UiaClientsAreListening"));
            a.raiseNotification = reinterpret_cast<decltype(a.raiseNotification)>(GetProcAddress(m, "UiaRaiseNotificationEvent"));
        }
        return a;
    }();
    return api;
}
bool Listening() {
    // Avoid loading the DLL just to ask: if it isn't loaded, no client has connected to us.
    if (!GetModuleHandleW(L"uiautomationcore.dll")) return false;
    return Uia().clientsListening && Uia().clientsListening();
}

class Root;

// One element in the fragment tree: kind 0 = nav list, 1 = nav item, 2 = content.
class Element : public IRawElementProviderSimple, public IRawElementProviderFragment, public ISelectionItemProvider, public IInvokeProvider {
public:
    Element(Root* root, int kind, int index) : root_(root), kind_(kind), index_(index) {}
    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(ISelectionItemProvider) && kind_ == 1) *out = static_cast<ISelectionItemProvider*>(this);
        else if (riid == __uuidof(IInvokeProvider) && kind_ == 1) *out = static_cast<IInvokeProvider*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* r) override { *r = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** r) override {
        *r = nullptr;
        if (kind_ == 1 && (id == kSelectionItemPattern || id == kInvokePattern)) {
            *r = id == kInvokePattern ? static_cast<IUnknown*>(static_cast<IInvokeProvider*>(this)) : static_cast<IUnknown*>(static_cast<ISelectionItemProvider*>(this));
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** r) override { *r = nullptr; return S_OK; }
    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir, IRawElementProviderFragment** r) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** r) override {
        int ids[] = {UiaAppendRuntimeId, kind_ * 1000 + index_ + 1};
        *r = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*r) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*r, &i, &ids[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** r) override;
    // ISelectionItemProvider / IInvokeProvider (nav items)
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* r) override { *r = static_cast<int>(TheApp().Current()) == index_; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** r) override;
    HRESULT STDMETHODCALLTYPE Invoke() override { return Select(); }

    int Kind() const { return kind_; }
    int Index() const { return index_; }

private:
    virtual ~Element() = default;
    std::atomic<ULONG> ref_{1};
    Root* root_;
    int kind_, index_;
};

class Root : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot {
public:
    explicit Root(HWND h) : hwnd(h) {}
    HWND hwnd;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot)) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* r) override { *r = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** r) override { *r = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        v->vt = VT_EMPTY;
        if (id == kName) {
            v->vt = VT_BSTR;
            std::wstring n = L"NEXUS - " + std::wstring(TheApp().GetPage(TheApp().Current())->Title());
            v->bstrVal = SysAllocString(n.c_str());
        } else if (id == kAutomationId) {
            v->vt = VT_BSTR;
            v->bstrVal = SysAllocString(L"NexusMain");
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** r) override {
        *r = nullptr;
        return Uia().hostFromHwnd ? Uia().hostFromHwnd(hwnd, r) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir, IRawElementProviderFragment** r) override {
        *r = nullptr;
        if (dir == NavigateDirection_FirstChild) *r = new Element(this, 0, 0);
        else if (dir == NavigateDirection_LastChild) *r = new Element(this, 2, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override { *r = {}; return S_OK; } // host provides it
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** r) override { *r = this; AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** r) override {
        POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
        ScreenToClient(hwnd, &pt);
        float s = TheApp().G().Scale();
        float navW = TheApp().Config().GetBool("ui.nav.collapsed", false) ? 52.f : 214.f;
        *r = pt.x / s < navW ? static_cast<IRawElementProviderFragment*>(new Element(this, 0, 0)) : new Element(this, 2, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** r) override {
        *r = new Element(this, 2, 0);
        return S_OK;
    }

private:
    virtual ~Root() = default;
    std::atomic<ULONG> ref_{1};
};

std::vector<int> VisiblePages() {
    std::vector<int> v;
    for (int i = 0; i < static_cast<int>(PageId::Count); ++i)
        if (TheApp().PageVisibleInNav(static_cast<PageId>(i))) v.push_back(i);
    return v;
}

HRESULT Element::GetPropertyValue(PROPERTYID id, VARIANT* v) {
    v->vt = VT_EMPTY;
    App& app = TheApp();
    auto str = [&](const std::wstring& s) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(s.c_str()); };
    auto i4 = [&](int x) { v->vt = VT_I4; v->lVal = x; };
    auto b = [&](bool x) { v->vt = VT_BOOL; v->boolVal = x ? VARIANT_TRUE : VARIANT_FALSE; };
    switch (id) {
    case kControlType: i4(kind_ == 0 ? kList : kind_ == 1 ? kListItem : kPane); break;
    case kName:
        if (kind_ == 0) str(L"Navigation");
        else if (kind_ == 1) str(app.GetPage(static_cast<PageId>(index_))->Title());
        else str(app.GetPage(app.Current())->AccessibleSummary());
        break;
    case kAutomationId: str(kind_ == 0 ? L"Nav" : kind_ == 1 ? L"Nav." + std::to_wstring(index_) : L"Content"); break;
    case kIsKeyboardFocusable: b(kind_ != 0); break;
    case kHasKeyboardFocus: b(kind_ == 2 && GetFocus() == root_->hwnd); break;
    case kIsEnabled: case kIsContentElement: case kIsControlElement: b(true); break;
    case kHelpText:
        if (kind_ == 2) str(L"Use Ctrl+1 to Ctrl+9 to switch pages, arrow keys to move through lists, Enter to inspect, Delete to end a task, Shift+F10 for the context menu.");
        break;
    case kLiveSetting: if (kind_ == 2) i4(1 /*Polite*/); break;
    default: break;
    }
    return S_OK;
}

HRESULT Element::Navigate(NavigateDirection dir, IRawElementProviderFragment** r) {
    *r = nullptr;
    auto pages = VisiblePages();
    switch (dir) {
    case NavigateDirection_Parent:
        if (kind_ == 1) *r = new Element(root_, 0, 0);
        else { *r = root_; root_->AddRef(); }
        break;
    case NavigateDirection_FirstChild:
        if (kind_ == 0 && !pages.empty()) *r = new Element(root_, 1, pages.front());
        break;
    case NavigateDirection_LastChild:
        if (kind_ == 0 && !pages.empty()) *r = new Element(root_, 1, pages.back());
        break;
    case NavigateDirection_NextSibling:
        if (kind_ == 0) *r = new Element(root_, 2, 0);
        else if (kind_ == 1) {
            for (size_t i = 0; i + 1 < pages.size(); ++i)
                if (pages[i] == index_) { *r = new Element(root_, 1, pages[i + 1]); break; }
        }
        break;
    case NavigateDirection_PreviousSibling:
        if (kind_ == 2) *r = new Element(root_, 0, 0);
        else if (kind_ == 1) {
            for (size_t i = 1; i < pages.size(); ++i)
                if (pages[i] == index_) { *r = new Element(root_, 1, pages[i - 1]); break; }
        }
        break;
    }
    return S_OK;
}

HRESULT Element::get_BoundingRectangle(UiaRect* r) {
    App& app = TheApp();
    RECT cr;
    GetClientRect(root_->hwnd, &cr);
    POINT org{0, 0};
    ClientToScreen(root_->hwnd, &org);
    float s = app.G().Scale();
    float navW = (app.Config().GetBool("ui.nav.collapsed", false) ? 52.f : 214.f) * s;
    if (kind_ == 0) *r = {static_cast<double>(org.x), static_cast<double>(org.y), navW, static_cast<double>(cr.bottom)};
    else if (kind_ == 2) *r = {org.x + navW, static_cast<double>(org.y), cr.right - navW, static_cast<double>(cr.bottom)};
    else {
        auto pages = VisiblePages();
        float itemH = (app.Compact() ? 32.f : 38.f) + 2.f;
        size_t pos = 0;
        for (size_t i = 0; i < pages.size(); ++i) if (pages[i] == index_) pos = i;
        double y = index_ == static_cast<int>(PageId::Settings) ? cr.bottom - (itemH + 12) * s : (56.f + pos * itemH) * s;
        *r = {static_cast<double>(org.x + 6 * s), org.y + y, navW - 12 * s, (itemH - 2) * s};
    }
    return S_OK;
}

HRESULT Element::get_FragmentRoot(IRawElementProviderFragmentRoot** r) {
    *r = root_;
    root_->AddRef();
    return S_OK;
}

HRESULT Element::Select() {
    TheApp().Navigate(static_cast<PageId>(index_));
    return S_OK;
}

HRESULT Element::get_SelectionContainer(IRawElementProviderSimple** r) {
    *r = new Element(root_, 0, 0);
    return S_OK;
}

Root* g_root = nullptr;

} // namespace

void Attach(HWND hwnd) {
    if (!g_root) g_root = new Root(hwnd);
}

void Detach(HWND hwnd) {
    if (g_root) {
        if (GetModuleHandleW(L"uiautomationcore.dll") && Uia().returnProvider) Uia().returnProvider(hwnd, 0, 0, nullptr);
        g_root->Release();
        g_root = nullptr;
    }
}

LRESULT HandleGetObject(HWND hwnd, WPARAM wp, LPARAM lp) {
    if (static_cast<long>(lp) == static_cast<long>(UiaRootObjectId) && g_root && Uia().returnProvider)
        return Uia().returnProvider(hwnd, wp, lp, static_cast<IRawElementProviderSimple*>(g_root));
    return 0;
}

void NotifyPageChanged(HWND hwnd, const std::wstring& title) {
    if (!g_root || !Listening()) return;
    auto* e = new Element(g_root, 2, 0);
    if (Uia().raiseEvent) Uia().raiseEvent(static_cast<IRawElementProviderSimple*>(e), kAutomationFocusChanged);
    e->Release();
    Announce(hwnd, title);
}

void Announce(HWND, const std::wstring& text) {
    if (!g_root || text.empty() || !Listening()) return;
    if (auto fn = Uia().raiseNotification) {
        BSTR t = SysAllocString(text.c_str());
        BSTR id = SysAllocString(L"NexusStatus");
        fn(static_cast<IRawElementProviderSimple*>(g_root), 5 /*NotificationKind_Other*/, 2 /*NotificationProcessing_MostRecent*/, t, id);
        SysFreeString(t);
        SysFreeString(id);
    }
}

} // namespace nx::a11y
