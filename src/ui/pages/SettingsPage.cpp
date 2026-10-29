// Settings: appearance, data collection, window/startup behaviour, navigation, notifications,
// history recording, keyboard shortcuts and privacy information.
#include "ui/pages/Common.h"

#include <shellapi.h>

namespace nx {

namespace {

const wchar_t* kPageNames[] = {L"Overview", L"Processes", L"Performance", L"App history", L"Startup apps", L"Users", L"Details", L"Services", L"Hardware",
                               L"Network", L"Storage", L"System diagnostics", L"Settings"};

class SettingsPage final : public Page {
public:
    explicit SettingsPage(App& a) : Page(a) {}
    const wchar_t* Title() const override { return L"Settings"; }
    wchar_t Glyph() const override { return 0xE713; }
    std::wstring AccessibleSummary() const override { return L"Settings"; }
    void OnHide() override { nameEdit_.Hide(); }

    void Draw(Ui& ui, const Rect& area) override {
        Gfx& g = ui.g;
        const Theme& t = ui.t;
        Settings& cfg = app.Config();
        float off = scroll_;
        Rect view = area;
        g.PushClip(view);
        float w = std::min(view.w - 14, 900.f);
        float x = view.x;
        y_ = view.y - scroll_ + 4;

        Section(ui, x, w, L"Appearance");
        {
            std::wstring theme = cfg.GetString("ui.theme", L"system");
            int sel = theme == L"light" ? 1 : theme == L"dark" ? 2 : 0;
            Rect c = Row(ui, x, w, L"Theme", L"Follow Windows, or always use light or dark.");
            int nt = ui.Tabs(Id("set.theme"), {c.R() - 270, c.y + (c.h - 32) / 2, 270, 32}, {L"System", L"Light", L"Dark"}, sel);
            if (nt >= 0 && nt != sel) { cfg.SetString("ui.theme", nt == 1 ? L"light" : nt == 2 ? L"dark" : L"system"); app.ApplyTheme(); }
        }
        {
            bool inter = cfg.GetString("ui.font", L"inter") != L"segoe";
            Rect c = Row(ui, x, w, L"Font", Gfx::BundledFontAvailable() ? L"Inter is included with NEXUS (SIL Open Font License 1.1)."
                                                                       : L"The bundled Inter font could not be loaded, so Segoe UI is used.");
            int nf = ui.Tabs(Id("set.font"), {c.R() - 200, c.y + (c.h - 32) / 2, 200, 32}, {L"Inter", L"Segoe UI"}, inter ? 0 : 1);
            if (nf >= 0 && (nf == 0) != inter) {
                cfg.SetString("ui.font", nf == 0 ? L"inter" : L"segoe");
                Gfx::SetUiFont(nf == 0 ? L"inter" : L"segoe");
                app.ApplyTheme();
            }
        }
        {
            Rect c = Row(ui, x, w, L"Greeting",
                         L"Shows \u201CGood morning\u201D, \u201CGood afternoon\u201D or \u201CHappy <day>\u201D (in the evening) with your name in the "
                         L"title bar. Leave the name empty to use your Windows account name.");
            bool on = cfg.GetBool("ui.greeting", true);
            if (ui.Toggle(Id("set.greeting"), {c.R() - 60, c.y, 60, c.h}, on)) { cfg.SetBool("ui.greeting", on); app.Invalidate(); }
            // Native edit: only while the whole row is visible in the scrolled view.
            Rect er{c.R() - 290, c.y + (c.h - 32) / 2, 220, 32};
            if (on && er.y >= view.y && er.B() <= view.B()) {
                nameEdit_.Place(app, er, L"Your name"); // creates the edit on first use, so fill it afterwards
                if (!nameEditPlaced_) { nameEdit_.SetText(cfg.GetString("ui.greeting.name")); nameEditPlaced_ = true; }
                std::wstring typed = nameEdit_.Text();
                if (typed != cfg.GetString("ui.greeting.name")) { cfg.SetString("ui.greeting.name", typed); app.Invalidate(); }
            } else {
                nameEdit_.Hide();
            }
        }
        {
            Rect c = Row(ui, x, w, L"Accent color", L"Used for selection, graphs highlights and controls.");
            // 0 = NEXUS blue ("system"), 1 = the Windows accent ("windows"), then fixed colours.
            static const uint32_t swatches[] = {0x2B7BFF, 0, 0x8764B8, 0xC239B3, 0xE74856, 0xF7630C, 0x10893E, 0x00B7C3, 0x515C6B};
            std::wstring cur = cfg.GetString("ui.accent", L"system");
            float sx = c.R() - 9 * 30;
            for (int i = 0; i < 9; ++i) {
                Rect sr{sx + i * 30, c.y + (c.h - 24) / 2, 24, 24};
                WidgetId id = IdMix(Id("set.accent"), i);
                bool hot = ui.Hit(id, sr);
                Color col = i == 1 ? Color::Hex(AccentFromWindows()) : Color::Hex(swatches[i]);
                g.FillRound(sr, 12.f, col);
                bool selected = i == 0 ? cur == L"system" : i == 1 ? cur == L"windows" : cur == FormatHex(swatches[i], 6).substr(2);
                if (selected || hot) g.StrokeRound(sr.Inset(-3), 14.f, selected ? t.text : t.textTertiary, 2.f);
                if (i == 1) g.Text(L"W", sr, Font::Caption, Color{1, 1, 1, 1}, Align::Center);
                ui.Tooltip(id, i == 0 ? L"NEXUS blue (default)" : i == 1 ? L"Use the Windows accent color" : L"Custom accent");
                if (ui.Clicked(id)) {
                    cfg.SetString("ui.accent", i == 0 ? L"system" : i == 1 ? L"windows" : FormatHex(swatches[i], 6).substr(2));
                    app.ApplyTheme();
                }
            }
        }
        {
            Rect c = Row(ui, x, w, L"Text and UI scale", L"In addition to the Windows display scale. Ctrl + / Ctrl - also adjust it.");
            double sc = cfg.GetDouble("ui.scale", 1.0);
            if (ui.ComboButton(Id("set.scale"), {c.R() - 160, c.y + (c.h - 32) / 2, 160, 32}, FormatPercent(sc * 100))) {
                ui.Defer([this, sc] {
                    double opts[] = {0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0};
                    std::vector<MenuItem> items;
                    for (int i = 0; i < 8; ++i) items.push_back({static_cast<UINT>(i + 1), FormatPercent(opts[i] * 100), std::fabs(opts[i] - sc) < 0.01});
                    UINT c2 = app.ShowMenu(items);
                    if (c2) { app.Config().SetDouble("ui.scale", opts[c2 - 1]); app.ApplyTheme(); }
                });
            }
        }
        {
            Rect c = Row(ui, x, w, L"Compact mode", L"Denser rows and smaller cards to fit more information.");
            bool v = app.Compact();
            if (ui.Toggle(Id("set.compact"), {c.R() - 60, c.y, 60, c.h}, v)) app.SetCompact(v);
        }

        Section(ui, x, w, L"Data collection");
        {
            Rect c = Row(ui, x, w, L"Update speed", L"How often NEXUS samples the system. Slower speeds use less CPU. NEXUS automatically slows down while hidden.");
            int ms = static_cast<int>(cfg.GetInt("refresh.ms", 1000));
            bool paused = cfg.GetBool("refresh.paused", false);
            std::wstring cur = paused ? L"Paused" : ms <= 250 ? L"Very high (0.25 s)" : ms <= 500 ? L"High (0.5 s)" : ms <= 1000 ? L"Normal (1 s)" : ms <= 2000 ? L"Low (2 s)" : L"Very low (4 s)";
            if (ui.ComboButton(Id("set.refresh"), {c.R() - 200, c.y + (c.h - 32) / 2, 200, 32}, cur)) {
                ui.Defer([this] {
                    UINT r = app.ShowMenu({{6, L"Very high (0.25 s)"}, {1, L"High (0.5 s)"}, {2, L"Normal (1 s)"}, {3, L"Low (2 s)"}, {4, L"Very low (4 s)"}, MenuItem::Sep(), {5, L"Paused"}});
                    int nms[] = {0, 500, 1000, 2000, 4000, 0, 250};
                    if ((r >= 1 && r <= 4) || r == 6) { app.Config().SetInt("refresh.ms", nms[r]); app.Config().SetBool("refresh.paused", false); }
                    if (r == 5) app.Config().SetBool("refresh.paused", true);
                    if (r) { app.ApplyRefreshRate(); app.UpdateDemand(); ResizeHistory(); }
                });
            }
        }
        {
            Rect c = Row(ui, x, w, L"Graph history length", L"Time span kept in memory for graphs (bounded ring buffers; nothing is written to disk).");
            int64_t sec = cfg.GetInt("graph.historySeconds", 60);
            std::wstring cur = sec < 120 ? std::to_wstring(sec) + L" seconds" : std::to_wstring(sec / 60) + L" minutes";
            if (ui.ComboButton(Id("set.hist"), {c.R() - 200, c.y + (c.h - 32) / 2, 200, 32}, cur)) {
                ui.Defer([this, sec] {
                    int64_t opts[] = {60, 120, 300, 600, 1800, 3600};
                    const wchar_t* names[] = {L"60 seconds", L"2 minutes", L"5 minutes", L"10 minutes", L"30 minutes", L"60 minutes"};
                    std::vector<MenuItem> items;
                    for (int i = 0; i < 6; ++i) items.push_back({static_cast<UINT>(i + 1), names[i], opts[i] == sec});
                    UINT r = app.ShowMenu(items);
                    if (r) { app.Config().SetInt("graph.historySeconds", opts[r - 1]); ResizeHistory(); }
                });
            }
        }
        Choice(ui, x, w, "units.temperature", L"C", L"Temperature units", L"", {{L"C", L"Celsius (°C)"}, {L"F", L"Fahrenheit (°F)"}});
        Choice(ui, x, w, "units.network", L"bits", L"Network speed units", L"Bits per second matches Task Manager and link speeds.",
               {{L"bits", L"Bits per second"}, {L"bytes", L"Bytes per second"}});
        Toggle(ui, x, w, "net.etw", true, L"Per-process network monitoring",
               L"Uses the kernel network ETW provider to measure each process's traffic. Requires running NEXUS as administrator.",
               [this] { app.Collector().SetNetTraceEnabled(app.Config().GetBool("net.etw", true)); });

        Section(ui, x, w, L"Window and startup");
        Toggle(ui, x, w, "window.topmost", false, L"Always on top", L"Keep NEXUS above other windows (Ctrl+T).",
               [this] { app.SetTopmost(app.Config().GetBool("window.topmost", false)); });
        Toggle(ui, x, w, "tray.enabled", true, L"Show icon in the notification area", L"Live CPU and memory meter in the system tray.", [this] {
            app.Config().SetBool("tray.enabled", app.Config().GetBool("tray.enabled", true));
            app.Info(L"Restart required", L"The notification-area icon setting takes effect the next time NEXUS starts.");
        });
        Toggle(ui, x, w, "window.hideWhenMinimized", false, L"Hide when minimized", L"Minimize to the notification area instead of the taskbar.");
        Toggle(ui, x, w, "window.closeToTray", false, L"Close to the notification area", L"The close button hides NEXUS; exit from the tray menu.");
        Toggle(ui, x, w, "startup.minimized", false, L"Start minimized", L"");
        {
            Rect c = Row(ui, x, w, L"Start with Windows", L"Adds NEXUS to your user's startup apps (starts minimized).");
            bool on = StartsWithWindows();
            bool v = on;
            if (ui.Toggle(Id("set.autostart"), {c.R() - 60, c.y, 60, c.h}, v)) SetStartsWithWindows(v);
        }
        {
            Rect c = Row(ui, x, w, L"Default page", L"Page shown when NEXUS opens.");
            int cur = static_cast<int>(cfg.GetInt("ui.startPage", static_cast<int>(PageId::Processes)));
            if (ui.ComboButton(Id("set.start"), {c.R() - 220, c.y + (c.h - 32) / 2, 220, 32}, kPageNames[std::clamp(cur, 0, 12)])) {
                ui.Defer([this, cur] {
                    std::vector<MenuItem> items;
                    for (int i = 0; i < 12; ++i) items.push_back({static_cast<UINT>(i + 1), kPageNames[i], i == cur});
                    UINT r = app.ShowMenu(items);
                    if (r) app.Config().SetInt("ui.startPage", r - 1);
                });
            }
        }
        {
            Rect c = Row(ui, x, w, L"Replace Task Manager",
                         L"Open NEXUS when Windows starts Task Manager (Ctrl+Shift+Esc, taskbar menu). Uses the documented Image File Execution Options "
                         L"\"Debugger\" setting and needs administrator approval. Turn off to restore Task Manager.");
            bool on = ReplacesTaskManager();
            bool v = on;
            if (ui.Toggle(Id("set.replace"), {c.R() - 60, c.y, 60, c.h}, v)) ui.Defer([this, v] { SetReplaceTaskManager(v); });
        }

        Section(ui, x, w, L"Navigation");
        {
            Rect c = Row(ui, x, w, L"Sections shown in the sidebar", L"Hide sections you don't use. Right-click a sidebar item for more options.", 46 + 4 * 30.f + 8);
            float yy = c.y + 46;
            for (int i = 0; i < 12; ++i) {
                bool vis = app.PageVisibleInNav(static_cast<PageId>(i));
                Rect cb{c.x + 16 + (i % 3) * (c.w / 3), yy + (i / 3) * 30, c.w / 3 - 20, 28};
                if (ui.Checkbox(IdMix(Id("set.nav"), i), cb, vis, kPageNames[i])) app.SetPageHidden(static_cast<PageId>(i), !vis);
            }
        }

        Section(ui, x, w, L"Notifications");
        Toggle(ui, x, w, "alerts.notify", false, L"Notify me about health findings",
               L"Show a notification when System diagnostics detects a warning or critical condition (at most once per hour per finding).");

        Section(ui, x, w, L"Performance history recording");
        Toggle(ui, x, w, "history.enabled", false, L"Record performance history to disk",
               L"Off by default. Appends system-wide metrics to daily CSV files, buffered and written at most once a minute.", [this] { ApplyRecorder(); });
        Choice(ui, x, w, "history.retentionDays", L"7", L"Keep recordings for", L"Older files are deleted automatically.",
               {{L"1", L"1 day"}, {L"7", L"7 days"}, {L"30", L"30 days"}, {L"90", L"90 days"}}, [this] { ApplyRecorder(); });
        Choice(ui, x, w, "history.everyN", L"5", L"Recording interval", L"Every Nth sample is recorded.",
               {{L"1", L"Every sample"}, {L"5", L"Every 5 samples"}, {L"15", L"Every 15 samples"}, {L"60", L"Every 60 samples"}}, [this] { ApplyRecorder(); });
        {
            Rect c = Row(ui, x, w, L"Recordings and exports", app.Recorder().Folder().empty() ? L"" : app.Recorder().Folder());
            if (ui.Button(Id("set.open"), {c.R() - 380, c.y + (c.h - 32) / 2, 120, 32}, L"Open folder")) {
                std::wstring f = app.Recorder().Folder();
                if (f.empty()) { ApplyRecorder(); f = app.Recorder().Folder(); }
                CreateDirectoryW(f.c_str(), nullptr);
                ShellExecuteW(app.Hwnd(), L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (ui.Button(Id("set.csv"), {c.R() - 250, c.y + (c.h - 32) / 2, 120, 32}, L"Export CSV")) ui.Defer([this] { app.ExportHistory(false); });
            if (ui.Button(Id("set.json"), {c.R() - 120, c.y + (c.h - 32) / 2, 120, 32}, L"Export JSON")) ui.Defer([this] { app.ExportHistory(true); });
        }

        Section(ui, x, w, L"Keyboard shortcuts");
        {
            static const std::pair<const wchar_t*, const wchar_t*> keys[] = {
                {L"Ctrl+1 … Ctrl+9", L"Switch to the n-th sidebar section"}, {L"Ctrl+0", L"Overview"}, {L"Ctrl+,", L"Settings"},
                {L"Ctrl+F / F3", L"Search"}, {L"Esc", L"Clear search"}, {L"Delete", L"End the selected task"}, {L"Enter", L"Inspect the selected process"},
                {L"Shift+F10 / Menu key", L"Context menu"}, {L"← / →", L"Collapse / expand a group"}, {L"Ctrl+N", L"Run new task"},
                {L"F5", L"Refresh now"}, {L"Ctrl+P", L"Pause / resume updates"}, {L"Ctrl+T", L"Always on top"}, {L"Ctrl+M", L"Mini monitor"},
                {L"Ctrl+E", L"Export diagnostic report"}, {L"Ctrl+B", L"Collapse / expand the sidebar"}, {L"Ctrl + / Ctrl -", L"UI scale"},
                {L"Ctrl+C (Performance)", L"Copy the current device summary"}, {L"Ctrl+Wheel on a graph", L"Zoom; drag to pan; double-click to reset"}};
            Rect c = Row(ui, x, w, L"", L"", 16 + (sizeof keys / sizeof keys[0]) * 22.f);
            float yy = c.y + 10;
            for (auto& [k, d] : keys) {
                g.Text(k, {c.x + 16, yy, 220, 22}, Font::BodySemi, t.text);
                g.Text(d, {c.x + 240, yy, c.w - 260, 22}, Font::Body, t.textSecondary);
                yy += 22;
            }
        }

        Section(ui, x, w, L"Privacy and about");
        {
            std::wstring about = L"NEXUS " NEXUS_VERSION_STR L" · native Win32 / Direct2D · " + std::wstring(app.Elevated() ? L"running as administrator" : L"running as standard user") +
                                 L"\n\nNEXUS collects no telemetry and makes no network connections on its own. The only network activity happens when you use "
                                 L"Search online, Ping or DNS lookup, or enable host-name resolution on the Network page.\n\nSettings are stored in " +
                                 app.ConfigDir() + L"\\nexus.ini (place a file named 'portable' next to NEXUS.exe to keep settings beside the executable).";
            float hgt = g.MeasureParagraph(about, w - 32, Font::Small) + 70;
            Rect c = Row(ui, x, w, L"", L"", hgt);
            g.Paragraph(about, {c.x + 16, c.y + 12, c.w - 32, hgt - 50}, Font::Small, t.textSecondary);
            if (ui.Button(Id("set.cfgdir"), {c.x + 16, c.y + hgt - 42, 170, 30}, L"Open settings folder"))
                ShellExecuteW(app.Hwnd(), L"open", app.ConfigDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (ui.Button(Id("set.reset"), {c.x + 196, c.y + hgt - 42, 170, 30}, L"Reset all settings", 0, ButtonStyle::Danger))
                ui.Defer([this] {
                    if (app.Confirm(L"Reset all settings?", L"NEXUS will restore its default settings. Window layout and column choices are reset too.", L"Reset", true)) {
                        std::wstring path = app.ConfigDir() + L"\\nexus.ini";
                        app.Config().Parse("");
                        DeleteFileW(path.c_str());
                        app.ApplyTheme();
                        app.ApplyRefreshRate();
                        app.ShowStatus(L"Settings reset. Some changes apply after restarting NEXUS.");
                    }
                });
        }
        y_ += 20;
        g.PopClip();
        ui.Scroll(Id("set.scroll"), view, y_ + scroll_ - view.y, off);
        scroll_ = off;
    }

private:
    void Section(Ui& ui, float x, float w, const wchar_t* title) {
        y_ += 14;
        ui.g.Text(title, {x, y_, w, 28}, Font::Subtitle, ui.t.text);
        y_ += 34;
    }

    Rect Row(Ui& ui, float x, float w, const std::wstring& label, const std::wstring& desc, float minH = 0) {
        float descH = desc.empty() ? 0 : ui.g.MeasureParagraph(desc, w - 360, Font::Small);
        float h = std::max(minH, std::max(60.f, 30 + descH + 14));
        Rect c{x, y_, w, h};
        ui.Card(c);
        if (!label.empty()) ui.g.Text(label, {c.x + 16, c.y + 10, w - 360, 22}, Font::Body, ui.t.text);
        if (!desc.empty()) ui.g.Paragraph(desc, {c.x + 16, c.y + 32, w - 360, descH + 4}, Font::Small, ui.t.textSecondary);
        y_ += h + 4;
        return Rect{c.x, c.y, c.w - 16, std::min(h, 60.f)};
    }

    void Toggle(Ui& ui, float x, float w, const char* key, bool def, const std::wstring& label, const std::wstring& desc, std::function<void()> onChange = {}) {
        Rect c = Row(ui, x, w, label, desc);
        bool v = app.Config().GetBool(key, def);
        if (ui.Toggle(IdMix(Id("set.toggle"), std::hash<std::string>{}(key)), {c.R() - 60, c.y, 60, c.h}, v)) {
            app.Config().SetBool(key, v);
            if (onChange) onChange();
        }
    }

    void Choice(Ui& ui, float x, float w, const char* key, const wchar_t* def, const std::wstring& label, const std::wstring& desc,
                std::vector<std::pair<std::wstring, std::wstring>> opts, std::function<void()> onChange = {}) {
        Rect c = Row(ui, x, w, label, desc);
        std::wstring cur = app.Config().GetString(key, def);
        std::wstring shown = cur;
        for (auto& [v, n] : opts) if (v == cur) shown = n;
        if (ui.ComboButton(IdMix(Id("set.choice"), std::hash<std::string>{}(key)), {c.R() - 220, c.y + (c.h - 32) / 2, 220, 32}, shown)) {
            std::string k = key;
            ui.Defer([this, k, opts, cur, onChange] {
                std::vector<MenuItem> items;
                for (size_t i = 0; i < opts.size(); ++i) items.push_back({static_cast<UINT>(i + 1), opts[i].second, opts[i].first == cur});
                UINT r = app.ShowMenu(items);
                if (r) {
                    app.Config().SetString(k.c_str(), opts[r - 1].first);
                    if (onChange) onChange();
                }
            });
        }
    }

    void ResizeHistory() {
        int64_t sec = app.Config().GetInt("graph.historySeconds", 60);
        int64_t ms = std::max<int64_t>(250, app.Config().GetInt("refresh.ms", 1000));
        app.Hist().Resize(static_cast<size_t>(std::clamp<int64_t>(sec * 1000 / ms, 30, 7200)));
    }

    void ApplyRecorder() {
        auto& c = app.Config();
        app.Recorder().Configure(c.GetBool("history.enabled", false), c.GetString("history.folder"), static_cast<int>(_wtoi(c.GetString("history.retentionDays", L"7").c_str())),
                                 static_cast<int>(_wtoi(c.GetString("history.everyN", L"5").c_str())));
    }

    bool StartsWithWindows() const {
        RegKey k;
        std::wstring v;
        return k.Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") && k.GetString(L"NEXUS", v);
    }
    void SetStartsWithWindows(bool on) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return;
        if (on) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --minimized";
            RegSetValueExW(h, L"NEXUS", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(h, L"NEXUS");
        }
        RegCloseKey(h);
    }

    bool ReplacesTaskManager() const {
        RegKey k;
        std::wstring v;
        return k.Open(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe") && k.GetString(L"Debugger", v) &&
               ContainsI(v, L"nexus");
    }
    void SetReplaceTaskManager(bool on) {
        if (on && !app.Confirm(L"Use NEXUS instead of Task Manager?",
                               L"Windows will start NEXUS whenever Task Manager is requested. To undo it, turn this setting off in NEXUS (or uninstall NEXUS). "
                               L"If NEXUS is moved or deleted while this is on, Task Manager will not open until the setting is removed.",
                               L"Replace Task Manager"))
            return;
        if (on) {
            // Windows may start the Task Manager replacement elevated without asking, so NEXUS must be
            // somewhere only administrators can change; otherwise any program could swap NEXUS.exe
            // (or plant files next to it) and gain administrator rights.
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring why;
            if (!IsProtectedFromNonAdmins(exe, &why)) {
                app.Info(L"Install NEXUS first",
                         L"NEXUS can only replace Task Manager when it is installed in a folder that only administrators can change, "
                         L"such as Program Files (the installer does this). This copy is not:\n\n" + why, true);
                return;
            }
        }
        DWORD e = RunSelfElevatedAndWait(app.Hwnd(), std::wstring(L"--taskmgr-replace ") + (on ? L"1" : L"0"));
        if (e == ERROR_CANCELLED) return;
        if (e == ERROR_INVALID_OWNER) app.ShowStatus(L"Not changed: NEXUS must be installed in a folder only administrators can change", true);
        else if (e) app.ShowStatus(L"Could not change the Task Manager replacement: " + LastErrorMessage(e), true);
        else app.ShowStatus(on ? L"NEXUS now opens instead of Task Manager" : L"Task Manager restored");
    }

    float scroll_ = 0, y_ = 0;
    NativeEdit nameEdit_;
    bool nameEditPlaced_ = false;
};

} // namespace

std::unique_ptr<Page> MakeSettingsPage(App& app) { return std::make_unique<SettingsPage>(app); }

} // namespace nx
