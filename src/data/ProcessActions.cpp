#include "core/Format.h"
#include "core/Win.h"
#include "data/ProcessOps.h"

#include <dbghelp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <functional>
#include <unordered_map>

namespace nx {

namespace {
DWORD Err() { DWORD e = GetLastError(); return e ? e : ERROR_GEN_FAILURE; }
struct PowerThrottling { ULONG Version, ControlMask, StateMask; };

uint64_t CreateTimeOf(HANDLE h) {
    FILETIME c, e, k, u;
    return GetProcessTimes(h, &c, &e, &k, &u) ? FileTimeToU64(c) : 0;
}
} // namespace

bool IsCriticalSystemProcess(const ProcSample& p) {
    if (p.pid <= 4) return true;
    if (!p.meta) return false;
    if (p.meta->isCritical) return true;
    static const wchar_t* names[] = {L"csrss.exe", L"smss.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"Registry", L"Memory Compression", L"Secure System"};
    if (!p.meta->inWindowsDir && !p.meta->path.empty()) return false;
    for (auto* n : names)
        if (_wcsicmp(p.meta->name.c_str(), n) == 0) return true;
    return false;
}

DWORD TerminateProcessId(uint32_t pid, uint64_t expectedCreateTime, std::wstring& why) {
    if (pid == GetCurrentProcessId()) { why = L"NEXUS will not terminate itself; close it normally."; return ERROR_ACCESS_DENIED; }
    Handle h(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) {
        DWORD e = Err();
        why = e == ERROR_ACCESS_DENIED ? (IsElevated() ? L"Access is denied. The process is protected by Windows."
                                                       : L"Access is denied. Try again as administrator.")
                                       : LastErrorMessage(e);
        return e;
    }
    // Guard against PID reuse between the moment the user chose the process and now.
    if (expectedCreateTime && CreateTimeOf(h.Get()) != expectedCreateTime) { why = L"The process has already exited."; return ERROR_NOT_FOUND; }
    if (!TerminateProcess(h.Get(), 1)) {
        DWORD e = Err();
        why = LastErrorMessage(e);
        return e;
    }
    return ERROR_SUCCESS;
}

DWORD TerminateProcessTree(const Snapshot& snap, uint32_t root, std::wstring& why) {
    std::unordered_map<uint32_t, std::vector<const ProcSample*>> children;
    const ProcSample* rootP = snap.FindProc(root);
    if (!rootP) { why = L"The process has already exited."; return ERROR_NOT_FOUND; }
    for (auto& p : snap.procs) {
        const ProcSample* parent = snap.FindProc(p.ppid);
        // A child must have been created after its parent; otherwise the PPID was reused.
        if (parent && p.pid != p.ppid && p.createTime >= parent->createTime) children[p.ppid].push_back(&p);
    }
    DWORD firstErr = ERROR_SUCCESS;
    int failures = 0;
    std::function<void(const ProcSample*, int)> kill = [&](const ProcSample* p, int depth) {
        if (depth > 64) return;
        for (auto* c : children[p->pid]) kill(c, depth + 1);
        if (IsCriticalSystemProcess(*p)) return;
        std::wstring w;
        DWORD e = TerminateProcessId(p->pid, p->createTime, w);
        if (e && e != ERROR_NOT_FOUND) {
            ++failures;
            if (!firstErr) { firstErr = e; why = p->Name() + L": " + w; }
        }
    };
    kill(rootP, 0);
    if (failures > 1) why += L" (" + std::to_wstring(failures) + L" processes could not be ended)";
    return firstErr;
}

DWORD SetProcessPriority(uint32_t pid, uint32_t cls) {
    Handle h(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
    if (!h) return Err();
    return SetPriorityClass(h.Get(), cls) ? ERROR_SUCCESS : Err();
}

DWORD GetProcessAffinity(uint32_t pid, uint64_t& pm, uint64_t& sm) {
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return Err();
    DWORD_PTR a = 0, b = 0;
    if (!GetProcessAffinityMask(h.Get(), &a, &b)) return Err();
    pm = a;
    sm = b;
    return ERROR_SUCCESS;
}

DWORD SetProcessAffinity(uint32_t pid, uint64_t mask) {
    if (!mask) return ERROR_INVALID_PARAMETER;
    Handle h(OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return Err();
    return SetProcessAffinityMask(h.Get(), static_cast<DWORD_PTR>(mask)) ? ERROR_SUCCESS : Err();
}

DWORD SetEfficiencyMode(uint32_t pid, bool on) {
    // Same mechanism as Task Manager: EcoQoS (execution speed throttling) plus idle base priority.
    Handle h(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
    if (!h) return Err();
    PowerThrottling pt{1, 0x1, on ? 0x1u : 0u};
    if (!SetProcessInformation(h.Get(), static_cast<PROCESS_INFORMATION_CLASS>(4) /*ProcessPowerThrottling*/, &pt, sizeof pt)) return Err();
    if (!SetPriorityClass(h.Get(), on ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS)) return Err();
    return ERROR_SUCCESS;
}

DWORD SuspendResumeProcess(uint32_t pid, bool suspend) {
    auto fn = suspend ? nt::Get().SuspendProcess : nt::Get().ResumeProcess;
    if (!fn) return ERROR_NOT_SUPPORTED;
    Handle h(OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid));
    if (!h) return Err();
    return nt::NT_OK(fn(h.Get())) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

std::wstring DefaultDumpPath(const std::wstring& processName) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring base = processName;
    size_t dot = base.rfind(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    std::wstring p = std::wstring(tmp, n) + base + L".DMP";
    for (int i = 1; FileExists(p) && i < 1000; ++i) p = std::wstring(tmp, n) + base + L" (" + std::to_wstring(i) + L").DMP";
    return p;
}

DWORD WriteProcessDump(uint32_t pid, const std::wstring& path, bool full) {
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid));
    if (!h) h.Reset(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!h) return Err();
    Handle f(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return Err();
    MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        full ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                MiniDumpWithTokenInformation)
             : (MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory));
    if (!MiniDumpWriteDump(h.Get(), pid, f.Get(), type, nullptr, nullptr, nullptr)) {
        DWORD e = Err();
        f.Reset();
        DeleteFileW(path.c_str());
        return e;
    }
    return ERROR_SUCCESS;
}

DWORD RunNewTask(HWND owner, const std::wstring& cmdIn, bool elevated, std::wstring& why) {
    std::wstring cmd = Trim(cmdIn);
    if (cmd.empty()) return ERROR_INVALID_PARAMETER;
    // Split "file args" the way the Run dialog does.
    std::wstring file, args;
    if (cmd[0] == L'"') {
        size_t q = cmd.find(L'"', 1);
        file = cmd.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        if (q != std::wstring::npos) args = Trim(cmd.substr(q + 1));
    } else {
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        LPWSTR a = PathGetArgsW(buf.data());
        args = a ? a : L"";
        PathRemoveArgsW(buf.data());
        file = buf.data();
    }
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = elevated ? L"runas" : nullptr;
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = Err();
        why = e == ERROR_CANCELLED ? L"The operation was cancelled." : L"Windows cannot find '" + file + L"'. " + LastErrorMessage(e);
        return e;
    }
    return ERROR_SUCCESS;
}

void SwitchToProcessWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (!SetForegroundWindow(hwnd)) SwitchToThisWindow(hwnd, TRUE);
}

} // namespace nx
