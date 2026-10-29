#include "app/CrashReport.h"
#include "core/Format.h"
#include "core/SafeFile.h"
#include "core/Win.h"

#include <dbghelp.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace nx::crash {

namespace {
constexpr size_t kKeep = 10;
// Codes for the errors that end a process without an exception of their own ("NX" + 1, 2).
constexpr DWORD kUncaughtCppException = 0xE04E5801, kCrtError = 0xE04E5802;
std::wstring g_folder;

struct Job {
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD threadId = 0;
    std::wstring summary; // result
};

bool IsLink(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Creates the folder and its parent unless one of them is a junction or symbolic link (an elevated
// NEXUS must not follow a link planted in the user's profile; SafeWriteGuard checks the rest).
bool PrepareFolder(const std::wstring& folder) {
    std::wstring parent = folder.substr(0, folder.find_last_of(L'\\'));
    for (const std::wstring& dir : {parent, folder}) {
        if (IsLink(dir)) return false;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    DWORD a = GetFileAttributesW(folder.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Keeps the newest kKeep reports. Their names start with the time, so name order is age order.
void Prune(const std::wstring& folder) {
    if (folder.empty() || IsLink(folder)) return;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((folder + L"\\NEXUS-*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do names.push_back(fd.cFileName);
    while (FindNextFileW(f, &fd));
    FindClose(f);
    if (names.size() <= kKeep) return;
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + kKeep < names.size(); ++i) {
        std::wstring base = folder + L"\\" + names[i].substr(0, names[i].size() - 4);
        DeleteFileW((base + L".txt").c_str());
        DeleteFileW((base + L".dmp").c_str());
    }
}

std::string Summary(EXCEPTION_POINTERS* ep, DWORD threadId) {
    char b[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(b, sizeof b, "NEXUS %s crashed on %04u-%02u-%02u at %02u:%02u:%02u (thread %lu).\r\n", NEXUS_VERSION_STR, st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond, static_cast<unsigned long>(threadId));
    std::string s = b;
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD& er = *ep->ExceptionRecord;
        // The module's file name only: its path can contain the user's name.
        std::wstring module = L"unknown module";
        uintptr_t offset = reinterpret_cast<uintptr_t>(er.ExceptionAddress);
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(er.ExceptionAddress), &m)) {
            wchar_t path[MAX_PATH];
            DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
            module = FileNameOf(std::wstring(path, n));
            offset -= reinterpret_cast<uintptr_t>(m);
        }
        const char* kind = er.ExceptionCode == kUncaughtCppException   ? " (uncaught C++ exception)"
                           : er.ExceptionCode == kCrtError             ? " (C runtime error)"
                           : er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? " (access violation)"
                           : er.ExceptionCode == EXCEPTION_STACK_OVERFLOW   ? " (stack overflow)"
                                                                           : "";
        std::snprintf(b, sizeof b, "Exception 0x%08lX%s at %s+0x%llX.\r\n", static_cast<unsigned long>(er.ExceptionCode), kind, ToUtf8(module).c_str(),
                      static_cast<unsigned long long>(offset));
        s += b;
        if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
            ULONG_PTR op = er.ExceptionInformation[0];
            std::snprintf(b, sizeof b, "It tried to %s address 0x%llX.\r\n", op == 1 ? "write to" : op == 8 ? "execute" : "read",
                          static_cast<unsigned long long>(er.ExceptionInformation[1]));
            s += b;
        }
    }
    std::snprintf(b, sizeof b, "Windows build %u, %s.\r\n", WindowsBuild(), IsElevated() ? "running as administrator" : "running as a standard user");
    return s + b;
}

// A new file only (never an existing one), refused through links when elevated (see SafeFile.h).
HANDLE CreateNew(SafeWriteGuard& guard, const std::wstring& path) {
    if (!guard.Prepare(path)) return INVALID_HANDLE_VALUE;
    return guard.Open(path, GENERIC_WRITE | WRITE_DAC, CREATE_NEW);
}

DWORD WINAPI WriteFiles(void* p) {
    Job& job = *static_cast<Job*>(p);
    if (g_folder.empty() || !PrepareFolder(g_folder)) return 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[80];
    std::swprintf(name, 80, L"\\NEXUS-%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    std::wstring base = g_folder + name;
    {
        SafeWriteGuard guard;
        Handle f(CreateNew(guard, base + L".dmp"));
        // Part of NEXUS's memory: administrators only when elevated, like process dumps.
        bool ok = f && (!IsElevated() || RestrictToAdministrators(f.Get()));
        if (ok) {
            MINIDUMP_EXCEPTION_INFORMATION mei{job.threadId, job.ep, FALSE};
            auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
            ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f.Get(), type, job.ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
        }
        if (f && !ok) {
            f.Reset();
            DeleteFileW((base + L".dmp").c_str());
        }
    }
    std::string text = Summary(job.ep, job.threadId);
    SafeWriteGuard guard;
    Handle f(CreateNew(guard, base + L".txt"));
    DWORD written = 0;
    if (f && WriteFile(f.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) job.summary = base + L".txt";
    return 0;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    static volatile LONG entered = 0;
    // A second thread that crashes meanwhile waits: the first report ends the process when it is done.
    if (InterlockedExchange(&entered, 1) != 0) Sleep(INFINITE);
    WriteReport(ep);
    return EXCEPTION_CONTINUE_SEARCH; // Windows Error Reporting still records the crash (Reliability Monitor)
}

// These errors end the process without an exception; raising one sends them through the handler
// above, with the failing thread's stack in the dump.
[[noreturn]] void OnTerminate() {
    RaiseException(kUncaughtCppException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    RaiseException(kCrtError, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
void OnPureCall() { RaiseException(kCrtError, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
} // namespace

void Install(const std::wstring& folder) {
    if (folder.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) g_folder = std::wstring(p) + L"\\NEXUS\\Crashes";
        CoTaskMemFree(p);
    } else {
        g_folder = folder;
    }
    Prune(g_folder);
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
}

const std::wstring& Folder() { return g_folder; }

std::wstring WriteReport(EXCEPTION_POINTERS* ep) {
    // On a thread of its own: the crashed thread may have no stack left (stack overflow), and the
    // dump records it from outside. A writer that does not finish in time keeps its job.
    auto* job = new Job{ep, GetCurrentThreadId(), {}};
    HANDLE t = CreateThread(nullptr, 0, WriteFiles, job, 0, nullptr);
    if (!t) WriteFiles(job);
    else if (WaitForSingleObject(t, 30000) != WAIT_OBJECT_0) {
        CloseHandle(t);
        return {};
    }
    if (t) CloseHandle(t);
    std::wstring summary = std::move(job->summary);
    delete job;
    return summary;
}

std::vector<std::wstring> ReportsAfter(uint64_t afterFileTime, uint64_t* newest) {
    std::vector<std::pair<uint64_t, std::wstring>> found;
    WIN32_FIND_DATAW fd;
    HANDLE f = g_folder.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((g_folder + L"\\NEXUS-*.txt").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            uint64_t t = FileTimeToU64(fd.ftLastWriteTime);
            if (t > afterFileTime) found.emplace_back(t, g_folder + L"\\" + fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (newest) *newest = found.empty() ? afterFileTime : found.front().first;
    std::vector<std::wstring> out;
    for (auto& e : found) out.push_back(std::move(e.second));
    return out;
}

} // namespace nx::crash
