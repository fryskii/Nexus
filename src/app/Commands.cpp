// Process actions, native dialogs (Run new task, affinity) and exports.
#include "app/App.h"
#include "core/Win.h"
#include "data/Hardware.h"
#include "data/ProcessOps.h"
#include "data/Processes.h"
#include "data/SystemData.h"
#include "data/Worker.h"
#include "ui/ProcessWindow.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wct.h>

#ifndef WCT_MAX_NODE_COUNT
#define WCT_MAX_NODE_COUNT 16
#endif
#ifndef WCTP_GETINFO_ALL_FLAGS
#define WCTP_GETINFO_ALL_FLAGS 0x7 // WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_COM_FLAG | WCT_OUT_OF_PROC_CS_FLAG
#endif

#include <algorithm>

namespace nx {

// ------------------------------------------------------------------------------------------
// In-memory dialog template builder (keeps dialogs native: keyboard, IME and screen readers work)
// ------------------------------------------------------------------------------------------
namespace {
class DlgBuilder {
public:
    DlgBuilder(const wchar_t* title, short w, short h) {
        Align4();
        DLGTEMPLATE t{};
        t.style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        t.cx = w;
        t.cy = h;
        Append(&t, sizeof t);
        Word(0); // menu
        Word(0); // class
        Str(title);
        Word(9); // point size
        Str(L"Segoe UI");
    }
    void Add(WORD cls, WORD id, const wchar_t* text, DWORD style, short x, short y, short w, short h) {
        Align4();
        DLGITEMTEMPLATE it{};
        it.style = style | WS_CHILD | WS_VISIBLE;
        it.x = x; it.y = y; it.cx = w; it.cy = h; it.id = id;
        Append(&it, sizeof it);
        Word(0xFFFF);
        Word(cls);
        Str(text);
        Word(0);
        ++count_;
    }
    DLGTEMPLATE* Get() {
        reinterpret_cast<DLGTEMPLATE*>(buf_.data())->cdit = count_;
        return reinterpret_cast<DLGTEMPLATE*>(buf_.data());
    }
    static constexpr WORD Button = 0x0080, Edit = 0x0081, Static = 0x0082;

private:
    void Append(const void* p, size_t n) { auto* b = static_cast<const uint8_t*>(p); buf_.insert(buf_.end(), b, b + n); }
    void Word(WORD w) { Append(&w, 2); }
    void Str(const wchar_t* s) { Append(s, (wcslen(s) + 1) * sizeof(wchar_t)); }
    void Align4() { while (buf_.size() % 4) buf_.push_back(0); }
    std::vector<uint8_t> buf_;
    WORD count_ = 0;
};

struct RunState { std::wstring cmd; bool elevated = false; };

INT_PTR CALLBACK RunDlgProc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<RunState*>(GetWindowLongPtrW(d, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(d, DWLP_USER, lp);
        st = reinterpret_cast<RunState*>(lp);
        HWND e = GetDlgItem(d, 10);
        SHAutoComplete(e, SHACF_FILESYSTEM | SHACF_URLALL | SHACF_USETAB);
        SetWindowTextW(e, st->cmd.c_str());
        SendMessageW(e, EM_SETSEL, 0, -1);
        if (IsElevated()) {
            CheckDlgButton(d, 11, BST_CHECKED);
            EnableWindow(GetDlgItem(d, 11), FALSE);
            SetDlgItemTextW(d, 11, L"This task will be created with administrative privileges.");
        }
        SetFocus(e);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            HWND e = GetDlgItem(d, 10);
            int n = GetWindowTextLengthW(e);
            st->cmd.assign(n, L'\0');
            GetWindowTextW(e, st->cmd.data(), n + 1);
            st->elevated = IsDlgButtonChecked(d, 11) == BST_CHECKED && !IsElevated();
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(d, IDCANCEL); return TRUE;
        case 12: {
            wchar_t file[MAX_PATH * 2] = {};
            OPENFILENAMEW of{sizeof of};
            of.hwndOwner = d;
            of.lpstrFilter = L"Programs (*.exe;*.com;*.bat;*.cmd;*.msc;*.lnk)\0*.exe;*.com;*.bat;*.cmd;*.msc;*.lnk\0All files (*.*)\0*.*\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH * 2;
            of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&of)) {
                std::wstring q = L"\"" + std::wstring(file) + L"\"";
                SetDlgItemTextW(d, 10, q.c_str());
            }
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

struct AffinityState { uint64_t mask = 0, system = 0; int count = 0; };

INT_PTR CALLBACK AffinityDlgProc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<AffinityState*>(GetWindowLongPtrW(d, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(d, DWLP_USER, lp);
        st = reinterpret_cast<AffinityState*>(lp);
        for (int i = 0; i < st->count; ++i) {
            CheckDlgButton(d, 1000 + i, (st->mask >> i) & 1 ? BST_CHECKED : BST_UNCHECKED);
            if (!((st->system >> i) & 1)) EnableWindow(GetDlgItem(d, 1000 + i), FALSE);
        }
        CheckDlgButton(d, 999, st->mask == st->system ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == 999) {
            bool all = IsDlgButtonChecked(d, 999) == BST_CHECKED;
            for (int i = 0; i < st->count; ++i)
                if ((st->system >> i) & 1) CheckDlgButton(d, 1000 + i, all ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wp) >= 1000 && LOWORD(wp) < 1064) {
            bool all = true;
            for (int i = 0; i < st->count; ++i)
                if (((st->system >> i) & 1) && IsDlgButtonChecked(d, 1000 + i) != BST_CHECKED) all = false;
            CheckDlgButton(d, 999, all ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            uint64_t mask = 0;
            for (int i = 0; i < st->count; ++i)
                if (IsDlgButtonChecked(d, 1000 + i) == BST_CHECKED) mask |= 1ULL << i;
            if (!mask) {
                MessageBoxW(d, L"The process must be allowed to run on at least one processor.", L"Processor affinity", MB_ICONWARNING);
                return TRUE;
            }
            st->mask = mask;
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

bool ConfirmWithCheck(App& app, const std::wstring& title, const std::wstring& msg, const std::wstring& ok, const char* dontAskKey, bool danger) {
    if (dontAskKey && app.Config().GetBool(dontAskKey, false)) return true;
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.hwndParent = app.Hwnd();
    tc.pszWindowTitle = L"NEXUS";
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = msg.c_str();
    tc.pszMainIcon = danger ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    TASKDIALOG_BUTTON buttons[] = {{IDOK, ok.c_str()}, {IDCANCEL, L"Cancel"}};
    tc.pButtons = buttons;
    tc.cButtons = 2;
    tc.nDefaultButton = danger ? IDCANCEL : IDOK;
    tc.pszVerificationText = dontAskKey ? L"Don't ask me again" : nullptr;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    int button = 0;
    BOOL verify = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, &verify))) return app.Confirm(title, msg, ok, danger);
    if (button == IDOK && verify && dontAskKey) app.Config().SetBool(dontAskKey, true);
    return button == IDOK;
}

void ReportFailure(App& app, const std::wstring& what, DWORD err, const std::wstring& why = L"") {
    std::wstring msg = why.empty() ? LastErrorMessage(err) : why;
    if (err == ERROR_ACCESS_DENIED && !app.Elevated()) {
        app.RequestElevatedRestart(what + L" failed: access is denied.\n\nThis process may belong to another user or run with higher privileges.");
        return;
    }
    app.ShowStatus(what + L" failed: " + msg, true);
}

std::wstring SaveDialog(HWND owner, const wchar_t* filter, const wchar_t* defExt, const std::wstring& defName) {
    wchar_t file[MAX_PATH * 2] = {};
    wcsncpy(file, defName.c_str(), MAX_PATH * 2 - 1);
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = owner;
    of.lpstrFilter = filter;
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = defExt;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&of) ? std::wstring(file) : std::wstring();
}

std::wstring Html(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else if (c == L'&') o += L"&amp;";
        else if (c == L'"') o += L"&quot;";
        else o += c;
    }
    return o;
}
} // namespace

// ------------------------------------------------------------------------------------------
// App-level commands
// ------------------------------------------------------------------------------------------
void App::RunNewTaskDialog() {
    DlgBuilder b(L"Create new task", 268, 104);
    b.Add(DlgBuilder::Static, 0xFFFF, L"Type the name of a program, folder, document, or Internet resource, and NEXUS will open it for you.", SS_LEFT, 7, 7, 254, 20);
    b.Add(DlgBuilder::Static, 0xFFFF, L"&Open:", SS_LEFT, 7, 34, 26, 10);
    b.Add(DlgBuilder::Edit, 10, L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 36, 32, 225, 13);
    b.Add(DlgBuilder::Button, 11, L"Create this task with &administrative privileges.", BS_AUTOCHECKBOX | WS_TABSTOP, 36, 52, 225, 12);
    b.Add(DlgBuilder::Button, IDOK, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 99, 82, 50, 15);
    b.Add(DlgBuilder::Button, IDCANCEL, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 153, 82, 50, 15);
    b.Add(DlgBuilder::Button, 12, L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, 207, 82, 54, 15);
    RunState st;
    st.cmd = settings_.GetString("run.last");
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), b.Get(), hwnd_, RunDlgProc, reinterpret_cast<LPARAM>(&st)) != IDOK) return;
    if (Trim(st.cmd).empty()) return;
    settings_.SetString("run.last", st.cmd);
    std::wstring why;
    DWORD e = RunNewTask(hwnd_, st.cmd, st.elevated, why);
    if (e && e != ERROR_CANCELLED) Info(L"Could not run the task", why, true);
    else if (!e) ShowStatus(L"Started: " + st.cmd);
}

void App::OpenProcessWindow(uint32_t pid) { ProcessWindow::Open(*this, pid); }

void App::ExportHistory(bool json) {
    if (sessionHistory_.empty()) { ShowStatus(L"No samples recorded yet.", true); return; }
    std::wstring path = json ? SaveDialog(hwnd_, L"JSON (*.json)\0*.json\0", L"json", L"nexus-performance.json")
                             : SaveDialog(hwnd_, L"CSV (*.csv)\0*.csv\0", L"csv", L"nexus-performance.csv");
    if (path.empty()) return;
    std::wstring err;
    bool ok = json ? ExportHistoryJson(path, sessionHistory_, err) : ExportHistoryCsv(path, sessionHistory_, err);
    ShowStatus(ok ? L"Exported " + std::to_wstring(sessionHistory_.size()) + L" samples to " + path : L"Export failed: " + err, !ok);
}

void App::ExportReport() {
    EnsureSystemInfo();
    EnsureGpuInfo();
    EnsureDiskInfo(true);
    if (!static_.sysLoaded || !static_.gpusLoaded || !static_.disksLoaded) {
        ShowStatus(L"Collecting system information for the report... try again in a moment.");
        return;
    }
    std::wstring path = SaveDialog(hwnd_, L"HTML report (*.html)\0*.html\0", L"html", L"nexus-report.html");
    if (path.empty()) return;
    const auto& s = static_.sys;
    const auto& cpu = sampler_->Cpu();
    std::wstring h = L"<!doctype html><html><head><meta charset=\"utf-8\"><title>NEXUS diagnostic report</title><style>"
                     L"body{font:14px Segoe UI,sans-serif;margin:24px;color:#1b1b1b}h1{font-weight:600}h2{margin-top:28px;border-bottom:1px solid #ddd}"
                     L"table{border-collapse:collapse}td,th{padding:4px 12px 4px 0;text-align:left;vertical-align:top}th{color:#555;font-weight:600}"
                     L".crit{color:#b00020}.warn{color:#8a5a00}.info{color:#0b5cad}</style></head><body>";
    h += L"<h1>NEXUS diagnostic report</h1><p>Generated " + Html(FormatFileTime(NowFileTime())) + L" UTC by NEXUS " NEXUS_VERSION_STR L"</p>";
    auto row = [&](const std::wstring& k, const std::wstring& v) { h += L"<tr><th>" + Html(k) + L"</th><td>" + Html(v.empty() ? L"—" : v) + L"</td></tr>"; };
    h += L"<h2>System</h2><table>";
    row(L"Computer", s.computerName);
    row(L"Operating system", s.osName + L" " + s.displayVersion + L" (build " + std::to_wstring(s.build) + L"." + std::to_wstring(s.ubr) + L")");
    row(L"Architecture", s.architecture);
    row(L"Boot mode", s.bootMode);
    row(L"Secure Boot", s.secureBoot);
    row(L"TPM", s.tpm + (s.tpmVersion.empty() ? L"" : L" " + s.tpmVersion));
    row(L"Virtualization-based security", s.vbs);
    row(L"Memory integrity (HVCI)", s.hvci);
    row(L"BIOS", s.smbios.biosVendor + L" " + s.smbios.biosVersion + L" (" + s.smbios.biosDate + L")");
    row(L"System", s.smbios.systemManufacturer + L" " + s.smbios.systemProduct);
    row(L"Motherboard", s.smbios.boardManufacturer + L" " + s.smbios.boardProduct);
    if (snap_) row(L"Uptime", FormatUptime(snap_->cpu.uptimeSec));
    h += L"</table><h2>Processor</h2><table>";
    row(L"Model", cpu.brand);
    row(L"Topology", std::to_wstring(cpu.sockets) + L" socket(s), " + std::to_wstring(cpu.cores) + L" cores, " + std::to_wstring(cpu.logical) + L" logical processors");
    row(L"Base speed", FormatFreqMHz(cpu.baseMHz));
    row(L"Virtualization", cpu.virtualizationFirmware ? L"Enabled" : L"Disabled");
    h += L"</table><h2>Memory</h2><table>";
    row(L"Installed", FormatBytes(static_.mem.installed));
    row(L"Slots used", std::to_wstring(static_.mem.slotsUsed) + L" of " + std::to_wstring(static_.mem.slotsTotal));
    row(L"Speed", static_.mem.configuredSpeedMTs ? std::to_wstring(static_.mem.configuredSpeedMTs) + L" MT/s" : L"");
    for (auto& m : static_.mem.modules)
        if (m.sizeBytes) row(m.deviceLocator, FormatBytes(m.sizeBytes) + L" " + m.MemoryTypeName() + L" " + m.manufacturer + L" " + m.partNumber);
    h += L"</table><h2>Graphics</h2><table>";
    for (auto& g : static_.gpus) row(g.name, L"Driver " + g.driverVersion + L" (" + g.driverDate + L"), " + FormatBytes(g.dedicatedVideo) + L" dedicated");
    h += L"</table><h2>Storage</h2><table>";
    for (auto& d : static_.disks) {
        std::wstring v = d.busType + L", " + FormatBytes(d.size) + (d.ssd ? L", SSD" : d.seekPenaltyKnown ? L", HDD" : L"");
        auto hi = static_.health.find(d.index);
        if (hi != static_.health.end() && hi->second.available) v += L", health: " + hi->second.healthEstimate;
        row(L"Disk " + std::to_wstring(d.index) + L": " + d.model, v);
        for (auto& vol : d.volumes)
            if (!vol.letter.empty()) row(L"    " + vol.letter + L" " + vol.label, vol.fileSystem + L", " + FormatBytes(vol.free) + L" free of " + FormatBytes(vol.total));
    }
    h += L"</table><h2>Diagnostic findings</h2>";
    if (alerts_.empty()) h += L"<p>No issues detected during this monitoring session.</p>";
    for (auto& a : alerts_) {
        const wchar_t* cls = a.severity == DiagAlert::Critical ? L"crit" : a.severity == DiagAlert::Warning ? L"warn" : L"info";
        h += L"<h3 class=\"" + std::wstring(cls) + L"\">" + Html(a.title) + L"</h3><ul>";
        for (auto& e : a.evidence) h += L"<li>" + Html(e) + L"</li>";
        h += L"</ul><p><b>Why:</b> " + Html(a.reasoning) + L"</p><p><b>Suggestion:</b> " + Html(a.suggestion) + L"</p>";
    }
    if (snap_) {
        h += L"<h2>Top processes</h2><table><tr><th>Name</th><th>PID</th><th>CPU</th><th>Memory (private)</th><th>Publisher</th></tr>";
        std::vector<const ProcSample*> v;
        for (auto& p : snap_->procs) if (p.pid) v.push_back(&p);
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->privateBytes > b->privateBytes; });
        for (size_t i = 0; i < v.size() && i < 25; ++i)
            h += L"<tr><td>" + Html(v[i]->Name()) + L"</td><td>" + std::to_wstring(v[i]->pid) + L"</td><td>" + FormatPercent(v[i]->cpu, 1) + L"</td><td>" +
                 FormatMB(v[i]->privateBytes) + L"</td><td>" + Html(v[i]->meta ? v[i]->meta->company : L"") + L"</td></tr>";
        h += L"</table>";
    }
    h += L"<h2>Event log summary</h2><table>";
    auto cnt = [](int n) { return n < 0 ? std::wstring(L"unavailable") : std::to_wstring(n); };
    row(L"Application crashes/hangs (24 h)", cnt(events.crashes24h));
    row(L"Unexpected shutdowns, Kernel-Power 41 (30 days)", cnt(events.kernelPower30d));
    row(L"WHEA hardware error events (30 days)", cnt(events.whea30d));
    row(L"System critical/error events (24 h)", cnt(events.errors24h));
    h += L"</table></body></html>";
    std::wstring err;
    if (WriteFileAtomic(path, ToUtf8(h), err)) {
        ShowStatus(L"Report saved to " + path);
        ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        ShowStatus(L"Could not save the report: " + err, true);
    }
}

// ------------------------------------------------------------------------------------------
// Process actions
// ------------------------------------------------------------------------------------------
namespace actions {

void EndTask(App& app, const ProcSample& p) {
    if (IsCriticalSystemProcess(p)) {
        app.Info(L"This process can't be ended", p.Name() + L" is a critical Windows process. Ending it would immediately crash Windows (stop error), "
                                                            L"so NEXUS does not allow it.", true);
        return;
    }
    bool system = p.meta && p.meta->inWindowsDir && p.category == ProcCategory::Windows;
    std::wstring title = L"End " + p.Name() + L"?";
    std::wstring msg = system ? L"This is a Windows process. Ending it may make Windows unstable or cause data loss until you restart. "
                                L"Unsaved data in programs that depend on it will be lost."
                              : L"Unsaved data in this program will be lost.";
    if (!ConfirmWithCheck(app, title, msg, L"End process", system ? nullptr : "confirm.endTask.skip", system)) return;
    std::wstring why;
    DWORD e = TerminateProcessId(p.pid, p.createTime, why);
    if (e) ReportFailure(app, L"Ending " + p.Name(), e, why);
    else { app.ShowStatus(L"Ended " + p.Name() + L" (PID " + std::to_wstring(p.pid) + L")"); app.Collector().RefreshNow(); }
}

void EndTree(App& app, const ProcSample& p) {
    if (IsCriticalSystemProcess(p)) {
        app.Info(L"This process tree can't be ended", p.Name() + L" is a critical Windows process.", true);
        return;
    }
    auto snap = app.Snap();
    if (!snap) return;
    size_t children = 0;
    for (auto& c : snap->procs) children += c.ppid == p.pid && c.createTime >= p.createTime ? 1 : 0;
    if (!app.Confirm(L"End the process tree of " + p.Name() + L"?",
                     L"This ends " + p.Name() + L" and all processes it started (" + std::to_wstring(children) +
                         L" direct child process(es) and their descendants). Unsaved data will be lost.",
                     L"End process tree", true))
        return;
    std::wstring why;
    DWORD e = TerminateProcessTree(*snap, p.pid, why);
    if (e) ReportFailure(app, L"Ending the process tree", e, why);
    else app.ShowStatus(L"Ended process tree of " + p.Name());
    app.Collector().RefreshNow();
}

void SetPriority(App& app, const ProcSample& p, uint32_t cls) {
    if (cls == REALTIME_PRIORITY_CLASS &&
        !app.Confirm(L"Set realtime priority?", L"Realtime priority can make the system unresponsive if this process uses a lot of CPU. "
                                                L"Without administrator rights Windows will use High instead.", L"Change priority", true))
        return;
    else if (cls != REALTIME_PRIORITY_CLASS &&
             !ConfirmWithCheck(app, L"Change the priority of " + p.Name() + L"?",
                               L"Changing the priority of certain processes could cause system instability.", L"Change priority", "confirm.priority.skip", false))
        return;
    DWORD e = SetProcessPriority(p.pid, cls);
    if (e) ReportFailure(app, L"Changing priority", e);
    else app.ShowStatus(L"Priority of " + p.Name() + L" set to " + PriorityClassName(cls));
}

void SetAffinityDialog(App& app, const ProcSample& p) {
    AffinityState st;
    DWORD e = GetProcessAffinity(p.pid, st.mask, st.system);
    if (e) { ReportFailure(app, L"Reading processor affinity", e); return; }
    int count = 0;
    for (int i = 0; i < 64; ++i) if ((st.system >> i) & 1) count = i + 1;
    st.count = count;
    int cols = count > 32 ? 4 : count > 8 ? 2 : 1;
    int rows = (count + cols - 1) / cols;
    short w = static_cast<short>(16 + cols * 70), hgt = static_cast<short>(70 + rows * 12);
    DlgBuilder b(L"Processor affinity", std::max<short>(w, 220), hgt);
    std::wstring head = L"Which processors are allowed to run \"" + p.Name() + L"\"?";
    b.Add(DlgBuilder::Static, 0xFFFF, head.c_str(), SS_LEFT, 7, 7, std::max<short>(w, 220) - 14, 10);
    b.Add(DlgBuilder::Button, 999, L"<All processors>", BS_AUTOCHECKBOX | WS_TABSTOP, 7, 22, 120, 10);
    std::vector<std::wstring> labels(count);
    for (int i = 0; i < count; ++i) {
        labels[i] = L"CPU " + std::to_wstring(i);
        b.Add(DlgBuilder::Button, static_cast<WORD>(1000 + i), labels[i].c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, static_cast<short>(7 + (i / rows) * 70),
              static_cast<short>(36 + (i % rows) * 12), 64, 10);
    }
    short bw = std::max<short>(w, 220);
    b.Add(DlgBuilder::Button, IDOK, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, static_cast<short>(bw - 114), static_cast<short>(hgt - 22), 50, 15);
    b.Add(DlgBuilder::Button, IDCANCEL, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, static_cast<short>(bw - 60), static_cast<short>(hgt - 22), 50, 15);
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), b.Get(), app.Hwnd(), AffinityDlgProc, reinterpret_cast<LPARAM>(&st)) != IDOK) return;
    e = SetProcessAffinity(p.pid, st.mask);
    if (e) ReportFailure(app, L"Setting processor affinity", e);
    else app.ShowStatus(L"Affinity of " + p.Name() + L" updated");
}

void ToggleEfficiency(App& app, const ProcSample& p) {
    bool on = !p.efficiencyMode;
    if (on && !ConfirmWithCheck(app, L"Turn on Efficiency mode for " + p.Name() + L"?",
                                L"Efficiency mode lowers the process priority and lets Windows run it on power-efficient cores and frequencies. "
                                L"It can make the app slower and, for some apps, unstable.",
                                L"Turn on Efficiency mode", "confirm.efficiency.skip", false))
        return;
    if (p.meta && p.meta->inWindowsDir && p.category == ProcCategory::Windows) {
        app.Info(L"Efficiency mode is not available", L"Windows processes can't be put in Efficiency mode (same rule as Task Manager).");
        return;
    }
    DWORD e = SetEfficiencyMode(p.pid, on);
    if (e) ReportFailure(app, L"Changing Efficiency mode", e);
    else app.ShowStatus(std::wstring(L"Efficiency mode ") + (on ? L"on" : L"off") + L" for " + p.Name());
    app.Collector().RefreshNow();
}

void CreateDump(App& app, const ProcSample& p, bool full) {
    std::wstring path = DefaultDumpPath(p.Name());
    uint32_t pid = p.pid;
    std::wstring name = p.Name();
    app.ShowStatus(L"Writing " + std::wstring(full ? L"full" : L"mini") + L" dump of " + name + L"...");
    auto result = std::make_shared<DWORD>(0);
    worker::Run([=] { *result = WriteProcessDump(pid, path, full); }, [&app, result, path, name] {
        if (*result) ReportFailure(app, L"Creating the dump file", *result);
        else if (app.Confirm(L"The dump file was created", name + L" was written to:\n" + path, L"Open file location")) OpenFileLocation(path);
    });
}

void SuspendResume(App& app, const ProcSample& p, bool suspend) {
    if (suspend && IsCriticalSystemProcess(p)) { app.Info(L"Can't suspend", L"Critical Windows processes can't be suspended.", true); return; }
    if (suspend && !app.Confirm(L"Suspend " + p.Name() + L"?", L"All threads of the process will be frozen until you resume it. "
                                                              L"Suspending system processes or programs others depend on can hang Windows or other apps.",
                                L"Suspend", true))
        return;
    DWORD e = SuspendResumeProcess(p.pid, suspend);
    if (e) ReportFailure(app, suspend ? L"Suspending" : L"Resuming", e);
    else app.ShowStatus((suspend ? L"Suspended " : L"Resumed ") + p.Name());
    app.Collector().RefreshNow();
}

void OpenLocation(const ProcSample& p) {
    if (p.meta && !p.meta->path.empty()) OpenFileLocation(p.meta->path);
}

void Properties(App& app, const ProcSample& p) {
    if (!p.meta || p.meta->path.empty()) { app.ShowStatus(L"The executable path of this process is not accessible.", true); return; }
    SHObjectProperties(app.Hwnd(), SHOP_FILEPATH, p.meta->path.c_str(), nullptr);
}

void RestartExplorer(App& app, const ProcSample& p) {
    if (!app.Confirm(L"Restart Windows Explorer?", L"The taskbar, desktop and File Explorer windows will disappear briefly and then reload.", L"Restart"))
        return;
    std::wstring why;
    DWORD e = TerminateProcessId(p.pid, p.createTime, why);
    if (e) { ReportFailure(app, L"Restarting Windows Explorer", e, why); return; }
    Sleep(500);
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    ShellExecuteW(nullptr, L"open", (std::wstring(win) + L"\\explorer.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    app.ShowStatus(L"Windows Explorer restarted");
}

// Wait Chain Traversal (documented in wct.h): shows which threads are blocked and on what.
void AnalyzeWaitChain(App& app, const ProcSample& p) {
    uint32_t pid = p.pid;
    std::wstring name = p.Name();
    auto out = std::make_shared<std::wstring>();
    worker::Run([pid, out] {
        HWCT session = OpenThreadWaitChainSession(0, nullptr);
        if (!session) { *out = L"Wait chain analysis is unavailable: " + LastErrorMessage(); return; }
        int blocked = 0, deadlocks = 0;
        for (auto& t : QueryThreads(pid)) {
            WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
            DWORD count = WCT_MAX_NODE_COUNT;
            BOOL cycle = FALSE;
            if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, t.tid, &count, nodes, &cycle) || count < 2) continue;
            ++blocked;
            if (cycle) ++deadlocks;
            std::wstring line = L"Thread " + std::to_wstring(t.tid);
            for (DWORD i = 1; i < count && i < 8; ++i) {
                auto& n = nodes[i];
                if (n.ObjectType == WctThreadType)
                    line += L" \u2192 thread " + std::to_wstring(n.ThreadObject.ThreadId) + L" (PID " + std::to_wstring(n.ThreadObject.ProcessId) + L")";
                else {
                    static const wchar_t* types[] = {L"", L"critical section", L"SendMessage", L"mutex", L"ALPC", L"COM", L"thread wait", L"process wait",
                                                     L"thread", L"COM activation", L"unknown", L"socket", L"SMB I/O"};
                    int ty = static_cast<int>(n.ObjectType);
                    line += std::wstring(L" \u2192 ") + (ty >= 0 && ty < 13 ? types[ty] : L"object");
                    if (n.LockObject.ObjectName[0]) line += std::wstring(L" '") + n.LockObject.ObjectName + L"'";
                }
            }
            if (cycle) line += L"  [DEADLOCK]";
            *out += line + L"\n";
            if (out->size() > 6000) { *out += L"\u2026"; break; }
        }
        CloseThreadWaitChainSession(session);
        if (!blocked) *out = L"No thread is waiting on another thread, process or lock. The process is not blocked by a wait chain.";
        else *out = std::to_wstring(blocked) + L" blocked thread(s)" + (deadlocks ? L", " + std::to_wstring(deadlocks) + L" in a deadlock cycle" : L"") + L":\n\n" + *out;
    }, [&app, name, out] { app.Info(L"Wait chain for " + name, *out); });
}

void ProcessContextMenu(App& app, const ProcSample& pIn, bool detailsView) {
    ProcSample p = pIn; // copy: the snapshot may change while the menu is open
    bool app_ = p.mainWindow != nullptr;
    bool hasPath = p.meta && !p.meta->path.empty();
    uint32_t pc = p.priorityClass;
    auto pr = [&](UINT id, const wchar_t* n, uint32_t cls) { MenuItem m{id, n, pc == cls}; m.radio = true; return m; };
    std::vector<MenuItem> items;
    if (app_ && !detailsView) items.push_back({1, L"Switch to"});
    items.push_back({2, L"End task\tDel"});
    items.push_back({3, L"End process tree"});
    items.push_back({4, L"Efficiency mode", p.efficiencyMode});
    items.push_back(p.status == ProcStatus::Suspended ? MenuItem{6, L"Resume"} : MenuItem{5, L"Suspend"});
    items.push_back(MenuItem::Sep());
    MenuItem prio{0, L"Set priority"};
    prio.sub = {pr(20, L"Realtime", REALTIME_PRIORITY_CLASS), pr(21, L"High", HIGH_PRIORITY_CLASS), pr(22, L"Above normal", ABOVE_NORMAL_PRIORITY_CLASS),
                pr(23, L"Normal", NORMAL_PRIORITY_CLASS), pr(24, L"Below normal", BELOW_NORMAL_PRIORITY_CLASS), pr(25, L"Low", IDLE_PRIORITY_CLASS)};
    items.push_back(prio);
    items.push_back({7, L"Set affinity..."});
    MenuItem dump{0, L"Create memory dump file"};
    dump.sub = {{8, L"Full dump"}, {9, L"Minidump"}};
    items.push_back(dump);
    items.push_back(MenuItem::Sep());
    items.push_back({10, L"Inspect...\tEnter"});
    if (!detailsView) items.push_back({11, L"Go to details"});
    bool hasServices = false;
    if (auto snap = app.Snap(); snap && snap->servicesByPid) hasServices = snap->servicesByPid->count(p.pid) != 0;
    if (hasServices) items.push_back({12, L"Go to service(s)"});
    items.push_back({13, L"Open file location", false, hasPath});
    items.push_back({14, L"Search online"});
    MenuItem copy{0, L"Copy"};
    copy.sub = {{30, L"Name"}, {31, L"PID"}, {32, L"Path", false, hasPath}, {33, L"Command line", false, p.meta && !p.meta->commandLine.empty()}};
    items.push_back(copy);
    items.push_back({15, L"Properties", false, hasPath});
    items.push_back(MenuItem::Sep());
    items.push_back({16, L"Analyze wait chain..."});
    bool explorer = _wcsicmp(p.Name().c_str(), L"explorer.exe") == 0;
    if (explorer) items.insert(items.begin(), MenuItem{17, L"Restart"});
    UINT c = app.ShowMenu(items);
    switch (c) {
    case 1: SwitchToProcessWindow(p.mainWindow); break;
    case 2: EndTask(app, p); break;
    case 3: EndTree(app, p); break;
    case 4: ToggleEfficiency(app, p); break;
    case 5: SuspendResume(app, p, true); break;
    case 6: SuspendResume(app, p, false); break;
    case 7: SetAffinityDialog(app, p); break;
    case 8: CreateDump(app, p, true); break;
    case 9: CreateDump(app, p, false); break;
    case 10: app.OpenProcessWindow(p.pid); break;
    case 11: app.Navigate(PageId::Details); app.Config().SetInt("details.focusPid", p.pid); break;
    case 12: app.Navigate(PageId::Services); app.Config().SetInt("services.focusPid", p.pid); break;
    case 13: OpenLocation(p); break;
    case 14: SearchOnline(p.Name() + (p.meta && !p.meta->description.empty() ? L" " + p.meta->description : L"")); break;
    case 15: Properties(app, p); break;
    case 20: SetPriority(app, p, REALTIME_PRIORITY_CLASS); break;
    case 21: SetPriority(app, p, HIGH_PRIORITY_CLASS); break;
    case 22: SetPriority(app, p, ABOVE_NORMAL_PRIORITY_CLASS); break;
    case 23: SetPriority(app, p, NORMAL_PRIORITY_CLASS); break;
    case 24: SetPriority(app, p, BELOW_NORMAL_PRIORITY_CLASS); break;
    case 25: SetPriority(app, p, IDLE_PRIORITY_CLASS); break;
    case 30: CopyTextToClipboard(app.Hwnd(), p.Name()); break;
    case 31: CopyTextToClipboard(app.Hwnd(), std::to_wstring(p.pid)); break;
    case 32: if (hasPath) CopyTextToClipboard(app.Hwnd(), p.meta->path); break;
    case 33: if (p.meta) CopyTextToClipboard(app.Hwnd(), p.meta->commandLine); break;
    case 16: AnalyzeWaitChain(app, p); break;
    case 17: RestartExplorer(app, p); break;
    default: break;
    }
}

} // namespace actions

} // namespace nx
