// TEMPORARY during development: remove once all pages exist.
#include "ui/pages/Common.h"
namespace nx {
namespace {
class Stub : public Page {
public:
    Stub(App& a, const wchar_t* t) : Page(a), t_(t) {}
    const wchar_t* Title() const override { return t_; }
    wchar_t Glyph() const override { return 0xE80F; }
    void Draw(Ui& ui, const Rect& r) override { ui.g.Text(L"(stub)", r, Font::Body, ui.t.textSecondary, Align::Center); }
    const wchar_t* t_;
};
}
#define STUB(fn, title) std::unique_ptr<Page> fn(App& a) { return std::make_unique<Stub>(a, title); }
STUB(MakeOverviewPage, L"Overview")
STUB(MakePerformancePage, L"Performance")
STUB(MakeAppHistoryPage, L"App history")
STUB(MakeStartupPage, L"Startup apps")
STUB(MakeUsersPage, L"Users")
STUB(MakeDetailsPage, L"Details")
STUB(MakeServicesPage, L"Services")
STUB(MakeHardwarePage, L"Hardware")
STUB(MakeNetworkPage, L"Network")
STUB(MakeStoragePage, L"Storage")
STUB(MakeDiagnosticsPage, L"System diagnostics")
STUB(MakeSettingsPage, L"Settings")
}
