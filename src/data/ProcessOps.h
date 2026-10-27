#pragma once
// Detailed process inspection (on demand, run on worker threads) and process management actions.
#include "data/Model.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nx {

// ---------------- Inspection ----------------
struct SignatureInfo {
    enum State { Unknown, Signed, Unsigned, Invalid, Untrusted } state = Unknown;
    std::wstring signer, issuer, detail;
    bool catalog = false;
    const wchar_t* StateName() const;
};
SignatureInfo VerifyFileSignature(const std::wstring& path);

struct ThreadInfo {
    uint32_t tid = 0;
    int32_t priority = 0, basePriority = 0;
    uint32_t state = 0, waitReason = 0, contextSwitches = 0;
    uint64_t kernelTime = 0, userTime = 0, createTime = 0, startAddress = 0;
    std::wstring startModule;
    float cpu = 0;
    std::wstring description; // SetThreadDescription name
};
const wchar_t* ThreadStateName(uint32_t state);
const wchar_t* WaitReasonName(uint32_t reason);

struct ModuleInfo {
    std::wstring name, path, description, company, version;
    uint64_t base = 0, size = 0;
};

struct MemRegion {
    uint64_t base = 0, size = 0, allocationBase = 0;
    uint32_t state = 0, protect = 0, type = 0;
    std::wstring mappedFile;
};
struct MemSummary {
    uint64_t imageCommit = 0, mappedCommit = 0, privateCommit = 0, reserved = 0, total = 0, regions = 0;
    std::vector<MemRegion> list;
};
std::wstring ProtectionText(uint32_t protect);

struct HandleInfo {
    uint64_t value = 0;
    uint32_t access = 0;
    uint16_t typeIndex = 0;
    std::wstring type, name;
};

struct TokenDetails {
    std::wstring user, sid, integrity, elevationType, logonSession;
    bool elevated = false, virtualizationAllowed = false, virtualizationEnabled = false, appContainer = false;
    uint32_t session = 0;
    struct Group { std::wstring name, flags; };
    std::vector<Group> groups;
    struct Privilege { std::wstring name, description; bool enabled = false, enabledByDefault = false; };
    std::vector<Privilege> privileges;
};

struct ProcessGeneral {
    std::wstring path, commandLine, currentDirectory, parentName, startTime, packageName;
    uint32_t pid = 0, ppid = 0, session = 0;
    std::wstring architecture, integrity, priority, dep, aslr, cfg, cet, protection;
    uint64_t affinity = 0, systemAffinity = 0;
    std::wstring description, company, version, product, copyright;
    SignatureInfo signature;
    bool accessible = false;
};

std::vector<ThreadInfo> QueryThreads(uint32_t pid);
std::vector<ModuleInfo> QueryModules(uint32_t pid, std::wstring& error);
MemSummary QueryMemoryMap(uint32_t pid, std::wstring& error);
std::vector<HandleInfo> QueryHandles(uint32_t pid, std::wstring& error);
TokenDetails QueryToken(uint32_t pid, std::wstring& error);
std::vector<std::pair<std::wstring, std::wstring>> QueryEnvironment(uint32_t pid, std::wstring& error);
ProcessGeneral QueryGeneral(uint32_t pid, const ProcMeta* meta);

// ---------------- Actions ----------------
// All actions return ERROR_SUCCESS or a Win32 error code; `why` receives a user-facing explanation.
DWORD TerminateProcessId(uint32_t pid, uint64_t expectedCreateTime, std::wstring& why);
// Terminates descendants first (deepest first), then the root. Uses the given snapshot for the tree.
DWORD TerminateProcessTree(const Snapshot& snap, uint32_t pid, std::wstring& why);
DWORD SetProcessPriority(uint32_t pid, uint32_t priorityClass);
DWORD GetProcessAffinity(uint32_t pid, uint64_t& processMask, uint64_t& systemMask);
DWORD SetProcessAffinity(uint32_t pid, uint64_t mask);
DWORD SetEfficiencyMode(uint32_t pid, bool on);
DWORD SuspendResumeProcess(uint32_t pid, bool suspend);
DWORD WriteProcessDump(uint32_t pid, const std::wstring& path, bool full);
DWORD RunNewTask(HWND owner, const std::wstring& commandLine, bool elevated, std::wstring& why);
void SwitchToProcessWindow(HWND hwnd);
std::wstring DefaultDumpPath(const std::wstring& processName);

// Processes whose termination would crash or destabilise Windows.
bool IsCriticalSystemProcess(const ProcSample& p);

} // namespace nx
