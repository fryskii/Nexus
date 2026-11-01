#include "app/CrashReport.h"
#include "core/Format.h"
#include "core/SafeFile.h"
#include "core/Win.h"
#include "data/ProcessOps.h"

#include <dbghelp.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// "NEXUS.exe+0x1234": the module's file name only (its path can contain the user's name).
std::string Where(uintptr_t address, bool* inModule = nullptr) {
    if (inModule) *inModule = false;
    std::wstring module = L"unknown module";
    uintptr_t offset = address;
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &m)) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
        module = FileNameOf(std::wstring(path, n));
        offset -= reinterpret_cast<uintptr_t>(m);
        if (inModule) *inModule = true;
    }
    char b[300];
    std::snprintf(b, sizeof b, "%s+0x%llX", ToUtf8(module).c_str(), static_cast<unsigned long long>(offset));
    return b;
}

// True when [address, address + size) is committed, readable memory of the allocation `base` (the
// crashed thread's stack), so unwinding never reads outside that stack.
bool InStack(uintptr_t address, size_t size, void* base) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi)) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    return mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) && !(mbi.Protect & PAGE_GUARD) && mbi.AllocationBase == base &&
           address + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// The crashed thread's call stack, one "#n module+offset" line per frame, unwound from the
// exception context with the modules' own unwind tables (x64 .pdata). Frame #0 is where the
// exception happened; later frames are return addresses (scripts/symbolize.py allows for that).
std::string StackTrace(const CONTEXT* context) {
    std::string s;
#if defined(_M_X64) || defined(__x86_64__)
    if (!context) return s;
    CONTEXT c = *context;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(c.Rsp), &mbi, sizeof mbi)) return s;
    void* stack = mbi.AllocationBase;
    int outside = 0; // frames in a row outside every module: past a few, the stack is damaged
    for (int i = 0; i < 64 && c.Rip; ++i) {
        char b[340];
        bool inModule = false;
        std::snprintf(b, sizeof b, "  #%d %s\r\n", i, Where(c.Rip, &inModule).c_str());
        s += b;
        outside = inModule ? 0 : outside + 1;
        if (outside >= 3) {
            s += "  (the rest of the stack is damaged)\r\n";
            break;
        }
        DWORD64 prevRsp = c.Rsp, imageBase = 0;
        // A frame needs at least its return address on the stack; the unwinder reads only within
        // the frame described by the module's tables, which lies between Rsp and the caller's Rsp.
        if (!InStack(c.Rsp, 8, stack)) break;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (fn) {
            // A function with a frame register (UNWIND_INFO byte 3, low 4 bits) is unwound through
            // that register, so it must point into the stack too. CONTEXT keeps Rax..R15 in x64
            // register-number order, as the unwinder indexes them.
            unsigned frameReg = reinterpret_cast<const BYTE*>(imageBase + fn->UnwindData)[3] & 0x0F;
            if (frameReg && !InStack((&c.Rax)[frameReg], 8, stack)) break;
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        } else {
            // A leaf function, or a call to a bad address: the return address is on top of the stack.
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        if (c.Rsp <= prevRsp || !InStack(c.Rsp - 8, 8, stack)) break; // stacks grow down: callers sit higher
    }
#endif
    return s;
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
        const char* kind = er.ExceptionCode == kUncaughtCppException   ? " (uncaught C++ exception)"
                           : er.ExceptionCode == kCrtError             ? " (C runtime error)"
                           : er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? " (access violation)"
                           : er.ExceptionCode == EXCEPTION_STACK_OVERFLOW   ? " (stack overflow)"
                                                                           : "";
        std::snprintf(b, sizeof b, "Exception 0x%08lX%s at %s.\r\n", static_cast<unsigned long>(er.ExceptionCode), kind,
                      Where(reinterpret_cast<uintptr_t>(er.ExceptionAddress)).c_str());
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
    // Added after the summary is on disk: a stack too damaged to unwind cannot cost the rest.
    if (f && job.ep && job.ep->ContextRecord) {
        std::string stack = StackTrace(job.ep->ContextRecord);
        if (!stack.empty()) {
            stack = "Call stack (newest first; scripts/symbolize.py with the release's NEXUS.debug names them):\r\n" + stack;
            WriteFile(f.Get(), stack.data(), static_cast<DWORD>(stack.size()), &written, nullptr);
        }
    }
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

namespace {
// ---- Call stacks of another process (a hung NEXUS) ----
// dbghelp's StackWalk64 with our own lookups: the unwind tables (.pdata) are read straight from the
// other process's modules, so dbghelp's symbol engine (which can follow _NT_SYMBOL_PATH to a symbol
// server, a network connection) is never used.
struct RemoteModule {
    uintptr_t base = 0, size = 0;
    std::wstring name;
    std::vector<RUNTIME_FUNCTION> functions;
    bool read = false;
};
struct RemoteWalk {
    std::vector<RemoteModule> modules;
    RUNTIME_FUNCTION found{};
};
thread_local RemoteWalk* t_walk = nullptr;

RemoteModule* RemoteModuleAt(DWORD64 address) {
    for (RemoteModule& m : t_walk->modules)
        if (address >= m.base && address - m.base < m.size) return &m;
    return nullptr;
}

void ReadFunctionTable(HANDLE process, RemoteModule& m) {
    m.read = true;
    BYTE header[4096];
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(m.base), header, sizeof header, &got) || got != sizeof header) return;
    LONG pe = 0;
    std::memcpy(&pe, header + 0x3C, sizeof pe);
    IMAGE_NT_HEADERS64 nt;
    if (pe < 0 || static_cast<size_t>(pe) + sizeof nt > sizeof header) return;
    std::memcpy(&nt, header + pe, sizeof nt);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return;
    IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    if (!dir.VirtualAddress || !count || count > 4'000'000 || static_cast<uintptr_t>(dir.VirtualAddress) + dir.Size > m.size) return;
    m.functions.resize(count);
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(m.base + dir.VirtualAddress), m.functions.data(), count * sizeof(RUNTIME_FUNCTION), &got) ||
        got != count * sizeof(RUNTIME_FUNCTION))
        m.functions.clear();
}

PVOID CALLBACK RemoteFunctionTable(HANDLE process, DWORD64 address) {
    RemoteModule* m = RemoteModuleAt(address);
    if (!m) return nullptr;
    if (!m->read) ReadFunctionTable(process, *m);
    DWORD rva = static_cast<DWORD>(address - m->base);
    // The table is sorted by start address (a PE requirement).
    auto it = std::upper_bound(m->functions.begin(), m->functions.end(), rva, [](DWORD v, const RUNTIME_FUNCTION& f) { return v < f.BeginAddress; });
    if (it == m->functions.begin() || rva >= (--it)->EndAddress) return nullptr;
    t_walk->found = *it;
    return &t_walk->found;
}

DWORD64 CALLBACK RemoteModuleBase(HANDLE, DWORD64 address) {
    RemoteModule* m = RemoteModuleAt(address);
    return m ? m->base : 0;
}

std::string RemoteWhere(DWORD64 address) {
    RemoteModule* m = RemoteModuleAt(address);
    char b[300];
    if (m) std::snprintf(b, sizeof b, "%s+0x%llX", ToUtf8(m->name).c_str(), static_cast<unsigned long long>(address - m->base));
    else std::snprintf(b, sizeof b, "unknown module+0x%llX", static_cast<unsigned long long>(address));
    return b;
}

std::string RemoteThreadStack(HANDLE process, HANDLE thread) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return "    (could not be paused to read its stack)\r\n";
    std::string s;
    alignas(16) CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &c)) {
        STACKFRAME64 f{};
        f.AddrPC = {c.Rip, 0, AddrModeFlat};
        f.AddrStack = {c.Rsp, 0, AddrModeFlat};
        f.AddrFrame = {c.Rbp, 0, AddrModeFlat};
        int outside = 0;
        for (int i = 0; i < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &f, &c, nullptr, RemoteFunctionTable, RemoteModuleBase, nullptr) &&
                        f.AddrPC.Offset;
             ++i) {
            char b[340];
            std::snprintf(b, sizeof b, "    #%d %s\r\n", i, RemoteWhere(f.AddrPC.Offset).c_str());
            s += b;
            outside = RemoteModuleAt(f.AddrPC.Offset) ? 0 : outside + 1;
            if (outside >= 3) break;
        }
    }
    ResumeThread(thread);
    return s.empty() ? "    (no stack read)\r\n" : s;
}

std::string RemoteStacks(DWORD pid, DWORD windowThreadId) {
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!process) return "Threads: could not be read (error " + std::to_string(GetLastError()) + ").\r\n";
    RemoteWalk walk;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModulesEx(process.Get(), mods, sizeof mods, &needed, LIST_MODULES_ALL)) {
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
            MODULEINFO mi{};
            wchar_t name[MAX_PATH];
            if (!GetModuleInformation(process.Get(), mods[i], &mi, sizeof mi)) continue;
            DWORD n = GetModuleBaseNameW(process.Get(), mods[i], name, MAX_PATH);
            walk.modules.push_back({reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), mi.SizeOfImage, std::wstring(name, n), {}, false});
        }
    }
    t_walk = &walk;
    std::string s = "Threads (newest frame first; scripts/symbolize.py with the release's NEXUS.debug names them):\r\n";
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    THREADENTRY32 te{sizeof te};
    if (snap && Thread32First(snap.Get(), &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            char b[120];
            std::snprintf(b, sizeof b, "  Thread %lu%s:\r\n", static_cast<unsigned long>(te.th32ThreadID), te.th32ThreadID == windowThreadId ? " (window)" : "");
            s += b;
            Handle thread(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID));
            s += thread ? RemoteThreadStack(process.Get(), thread.Get()) : "    (could not be opened)\r\n";
        } while (Thread32Next(snap.Get(), &te));
    }
    t_walk = nullptr;
    return s;
}
} // namespace

std::wstring WriteHangReport(DWORD pid, DWORD windowThreadId) {
    if (g_folder.empty() || !PrepareFolder(g_folder)) return {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[80];
    std::swprintf(name, 80, L"\\NEXUS-%04u%02u%02u-%02u%02u%02u-%lu-hang", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(pid));
    std::wstring base = g_folder + name;
    char b[512];
    std::snprintf(b, sizeof b,
                  "NEXUS (process %lu) stopped responding and was ended from a new NEXUS %s on %04u-%02u-%02u at %02u:%02u:%02u.\r\n"
                  "Windows build %u, %s.\r\n\r\n",
                  static_cast<unsigned long>(pid), NEXUS_VERSION_STR, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, WindowsBuild(),
                  IsElevated() ? "running as administrator" : "running as a standard user");
    std::string text = b;
    // Stacks first: the dump and the wait chains take longer, and the threads are what matters.
    text += RemoteStacks(pid, windowThreadId) + "\r\nWait chains:\r\n" + ToUtf8(DescribeWaitChains(pid)) + "\r\n";
    DWORD dump = WriteProcessDump(pid, base + L".dmp", false);
    if (dump != ERROR_SUCCESS) {
        std::snprintf(b, sizeof b, "\r\nNo minidump (error %lu).\r\n", static_cast<unsigned long>(dump));
        text += b;
    }
    SafeWriteGuard guard;
    Handle f(CreateNew(guard, base + L".txt"));
    DWORD written = 0;
    if (!f || !WriteFile(f.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) return {};
    return base + L".txt";
}

bool IsHangReport(const std::wstring& path) { return path.size() >= 9 && path.compare(path.size() - 9, 9, L"-hang.txt") == 0; }

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
