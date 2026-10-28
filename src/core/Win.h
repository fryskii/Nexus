#pragma once
// Windows helpers: RAII handles, COM pointer, string/registry utilities and NT definitions that the
// public SDK headers only partially declare. Every NT structure defined here is used defensively:
// callers handle failure and fall back to documented APIs.
#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nx {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept { if (this != &o) { Reset(); h_ = o.h_; o.h_ = nullptr; } return *this; }
    void Reset(HANDLE h = nullptr) { if (h_) CloseHandle(h_); h_ = (h == INVALID_HANDLE_VALUE ? nullptr : h); }
    HANDLE Get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    HANDLE Release() { HANDLE h = h_; h_ = nullptr; return h; }
private:
    HANDLE h_ = nullptr;
};

class RegKey {
public:
    RegKey() = default;
    ~RegKey() { if (k_) RegCloseKey(k_); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    bool Open(HKEY root, const wchar_t* path, REGSAM sam = KEY_READ) {
        if (k_) { RegCloseKey(k_); k_ = nullptr; }
        return RegOpenKeyExW(root, path, 0, sam, &k_) == ERROR_SUCCESS;
    }
    HKEY Get() const { return k_; }
    bool GetString(const wchar_t* name, std::wstring& out) const;
    bool GetDword(const wchar_t* name, DWORD& out) const;
    bool GetQword(const wchar_t* name, uint64_t& out) const;
    bool GetBinary(const wchar_t* name, std::vector<uint8_t>& out) const;
    std::vector<std::wstring> SubKeys() const;
    std::vector<std::wstring> ValueNames() const;
private:
    HKEY k_ = nullptr;
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(const ComPtr& o) { if (this != &o) { Reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void** PutVoid() { Reset(); return reinterpret_cast<void**>(&p_); }
    explicit operator bool() const { return p_ != nullptr; }
    void Attach(T* p) { Reset(); p_ = p; }
private:
    T* p_ = nullptr;
};

std::wstring LastErrorMessage(DWORD err = GetLastError());
std::wstring ExpandEnv(const std::wstring& s);
std::wstring GetModuleDir();
std::wstring FileNameOf(const std::wstring& path);
std::wstring DirOf(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool IsElevated();
// True if only Administrators, SYSTEM and TrustedInstaller can replace or modify the file at
// `path`: the file and its folder grant no write/delete/permission rights to anyone else, and no
// parent folder lets anyone else delete, rename or re-permission its children. Reparse points are
// resolved first. Used before NEXUS registers itself to run in place of Task Manager.
bool IsProtectedFromNonAdmins(const std::wstring& path, std::wstring* why = nullptr);
uint32_t WindowsBuild();     // e.g. 22631
bool IsRunningUnderWine();   // Wine implements many APIs as stubs that report success
bool IsWindowsDarkMode();      // apps theme
DWORD GetWindowsAccentColor(); // 0x00BBGGRR or 0 if unavailable
uint64_t FileTimeToU64(const FILETIME& ft);
uint64_t NowFileTime();
uint64_t QpcNow();
double QpcToSeconds(uint64_t ticks);

// Version resource info (cached by caller).
struct FileVersionInfo { std::wstring description, company, version, product, copyright; };
bool ReadFileVersionInfo(const std::wstring& path, FileVersionInfo& out);

// Opens Explorer with the file selected.
void OpenFileLocation(const std::wstring& path);
void SearchOnline(const std::wstring& term);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
// Runs a program elevated via the UAC prompt ("runas"); returns false if the user cancelled.
bool ShellRun(HWND owner, const std::wstring& file, const std::wstring& args, bool elevated, int show = SW_SHOWNORMAL);
// Runs NEXUS itself elevated with the given arguments (a privileged helper action), waits for it
// and returns its exit code (a Win32 error code), or ERROR_CANCELLED if the UAC prompt was declined.
DWORD RunSelfElevatedAndWait(HWND owner, const std::wstring& args, DWORD timeoutMs = 60000);
// Converts a native NT path (\Device\HarddiskVolume3\...) to a DOS path (C:\...).
std::wstring NtPathToDos(const std::wstring& nt);

} // namespace nx

// ---------------------------------------------------------------------------------------------
// NT native definitions (from winternl.h / public symbols). Layouts have been stable since
// Windows Vista and are relied on by Task Manager itself; NEXUS treats every call as optional.
// ---------------------------------------------------------------------------------------------
namespace nt {

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH = static_cast<NTSTATUS>(0xC0000004L);
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = static_cast<NTSTATUS>(0xC0000023L);
constexpr NTSTATUS STATUS_BUFFER_OVERFLOW = static_cast<NTSTATUS>(0x80000005L);
inline bool NT_OK(NTSTATUS s) { return s >= 0; }

struct UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct CLIENT_ID { HANDLE UniqueProcess; HANDLE UniqueThread; };

struct SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};

struct SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    SYSTEM_THREAD_INFORMATION Threads[1];
};

struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};
struct SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
};

struct PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
};

struct OBJECT_TYPE_INFORMATION {
    UNICODE_STRING TypeName;
    ULONG Reserved[22];
};
struct OBJECT_TYPES_INFORMATION { ULONG NumberOfTypes; };

enum : ULONG {
    SystemProcessorPerformanceInformation = 8,
    SystemProcessInformation = 5,
    SystemExtendedHandleInformation = 64,
};
enum : ULONG {
    ProcessBasicInformation = 0,
    ProcessCommandLineInformation = 60,
};
enum : ULONG { ObjectNameInformation = 1, ObjectTypesInformation = 3 };

using NtQuerySystemInformation_t = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcess_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryObject_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtSuspendProcess_t = NTSTATUS(NTAPI*)(HANDLE);
using RtlGetVersion_t = NTSTATUS(NTAPI*)(OSVERSIONINFOEXW*);

struct Api {
    NtQuerySystemInformation_t QuerySystemInformation = nullptr;
    NtQueryInformationProcess_t QueryInformationProcess = nullptr;
    NtQueryObject_t QueryObject = nullptr;
    NtSuspendProcess_t SuspendProcess = nullptr;
    NtSuspendProcess_t ResumeProcess = nullptr;
    RtlGetVersion_t GetVersion = nullptr;
};
const Api& Get();

} // namespace nt
