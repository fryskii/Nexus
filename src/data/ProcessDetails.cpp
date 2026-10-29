#include "core/Format.h"
#include "core/Win.h"
#include "data/ProcessOps.h"
#include "data/Processes.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <mscat.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <sddl.h>
#include <wct.h>

#ifndef WCT_MAX_NODE_COUNT
#define WCT_MAX_NODE_COUNT 16
#endif
#ifndef WCTP_GETINFO_ALL_FLAGS
#define WCTP_GETINFO_ALL_FLAGS 0x7 // WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_COM_FLAG | WCT_OUT_OF_PROC_CS_FLAG
#endif

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <unordered_map>

namespace nx {

// ------------------------------------------------------------------------------------------
// Authenticode / catalog signature verification
// ------------------------------------------------------------------------------------------
const wchar_t* SignatureInfo::StateName() const {
    switch (state) {
    case Signed: return L"Verified";
    case Unsigned: return L"Not signed";
    case Invalid: return L"Invalid signature";
    case Untrusted: return L"Untrusted";
    default: return L"Unknown";
    }
}

namespace {

std::wstring CertName(PCCERT_CONTEXT cert, DWORD flags) {
    wchar_t buf[512];
    DWORD n = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, 512);
    return n > 1 ? std::wstring(buf, n - 1) : std::wstring();
}

LONG Verify(WINTRUST_DATA& wd, SignatureInfo& out) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (r == ERROR_SUCCESS || r == CERT_E_UNTRUSTEDROOT || r == CERT_E_EXPIRED) {
        if (CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0)) {
                if (sg->csCertChain && sg->pasCertChain && sg->pasCertChain[0].pCert) {
                    out.signer = CertName(sg->pasCertChain[0].pCert, 0);
                    out.issuer = CertName(sg->pasCertChain[0].pCert, CERT_NAME_ISSUER_FLAG);
                }
            }
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    return r;
}

void Classify(LONG r, SignatureInfo& s) {
    switch (r) {
    case ERROR_SUCCESS: s.state = SignatureInfo::Signed; break;
    case TRUST_E_NOSIGNATURE: case TRUST_E_SUBJECT_FORM_UNKNOWN: case TRUST_E_PROVIDER_UNKNOWN: s.state = SignatureInfo::Unsigned; break;
    case CERT_E_UNTRUSTEDROOT: case CERT_E_CHAINING: case TRUST_E_EXPLICIT_DISTRUST: case CERT_E_EXPIRED: s.state = SignatureInfo::Untrusted; break;
    default: s.state = SignatureInfo::Invalid; break;
    }
    if (r != ERROR_SUCCESS) s.detail = LastErrorMessage(static_cast<DWORD>(r));
}

// Windows 8+ catalog APIs, resolved at run time (older import libraries lack them).
struct CatApi {
    BOOL(WINAPI* acquire2)(HCATADMIN*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD) = nullptr;
    BOOL(WINAPI* hash2)(HCATADMIN, HANDLE, DWORD*, BYTE*, DWORD) = nullptr;
};
const CatApi& Cat() {
    static CatApi c = [] {
        CatApi a;
        if (HMODULE m = GetModuleHandleW(L"wintrust.dll")) {
            a.acquire2 = reinterpret_cast<decltype(a.acquire2)>(GetProcAddress(m, "CryptCATAdminAcquireContext2"));
            a.hash2 = reinterpret_cast<decltype(a.hash2)>(GetProcAddress(m, "CryptCATAdminCalcHashFromFileHandle2"));
        }
        return a;
    }();
    return c;
}

std::mutex g_sigLock;
std::unordered_map<std::wstring, SignatureInfo> g_sigCache;

} // namespace

SignatureInfo VerifyFileSignature(const std::wstring& path) {
    std::wstring key = ToLower(path);
    {
        std::lock_guard<std::mutex> l(g_sigLock);
        auto it = g_sigCache.find(key);
        if (it != g_sigCache.end()) return it->second;
    }
    SignatureInfo s;
    if (path.empty()) return s;

    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE; // never touch the network
    LONG r = Verify(wd, s);

    if (r == TRUST_E_NOSIGNATURE) {
        // Most Windows binaries are catalog-signed rather than embedded-signed.
        Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        HCATADMIN cat = nullptr;
        if (f && Cat().acquire2 && Cat().hash2 && Cat().acquire2(&cat, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
            DWORD hashLen = 0;
            Cat().hash2(cat, f.Get(), &hashLen, nullptr, 0);
            std::vector<BYTE> hash(hashLen ? hashLen : 64);
            if (hashLen && Cat().hash2(cat, f.Get(), &hashLen, hash.data(), 0)) {
                HCATINFO ci = CryptCATAdminEnumCatalogFromHash(cat, hash.data(), hashLen, 0, nullptr);
                if (ci) {
                    CATALOG_INFO info{};
                    info.cbStruct = sizeof info;
                    if (CryptCATCatalogInfoFromContext(ci, &info, 0)) {
                        std::wstring tag;
                        for (DWORD i = 0; i < hashLen; ++i) {
                            wchar_t b[3];
                            swprintf(b, 3, L"%02X", hash[i]);
                            tag += b;
                        }
                        WINTRUST_CATALOG_INFO wci{};
                        wci.cbStruct = sizeof wci;
                        wci.pcwszCatalogFilePath = info.wszCatalogFile;
                        wci.pcwszMemberFilePath = path.c_str();
                        wci.pcwszMemberTag = tag.c_str();
                        wci.pbCalculatedFileHash = hash.data();
                        wci.cbCalculatedFileHash = hashLen;
                        wci.hCatAdmin = cat;
                        WINTRUST_DATA wc{};
                        wc.cbStruct = sizeof wc;
                        wc.dwUIChoice = WTD_UI_NONE;
                        wc.fdwRevocationChecks = WTD_REVOKE_NONE;
                        wc.dwUnionChoice = WTD_CHOICE_CATALOG;
                        wc.pCatalog = &wci;
                        wc.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
                        r = Verify(wc, s);
                        s.catalog = true;
                    }
                    CryptCATAdminReleaseCatalogContext(cat, ci, 0);
                }
            }
            CryptCATAdminReleaseContext(cat, 0);
        }
    }
    Classify(r, s);
    std::lock_guard<std::mutex> l(g_sigLock);
    if (g_sigCache.size() > 4096) g_sigCache.clear();
    g_sigCache[key] = s;
    return s;
}

// ------------------------------------------------------------------------------------------
// Threads
// ------------------------------------------------------------------------------------------
const wchar_t* ThreadStateName(uint32_t s) {
    static const wchar_t* n[] = {L"Initialized", L"Ready", L"Running", L"Standby", L"Terminated", L"Waiting", L"Transition", L"Deferred ready",
                                 L"Gate wait", L"Waiting for process in swap"};
    return s < 10 ? n[s] : L"Unknown";
}

const wchar_t* WaitReasonName(uint32_t r) {
    static const wchar_t* n[] = {L"Executive", L"FreePage", L"PageIn", L"PoolAllocation", L"DelayExecution", L"Suspended", L"UserRequest",
                                 L"WrExecutive", L"WrFreePage", L"WrPageIn", L"WrPoolAllocation", L"WrDelayExecution", L"WrSuspended",
                                 L"WrUserRequest", L"WrEventPair", L"WrQueue", L"WrLpcReceive", L"WrLpcReply", L"WrVirtualMemory",
                                 L"WrPageOut", L"WrRendezvous", L"WrKeyedEvent", L"WrTerminated", L"WrProcessInSwap", L"WrCpuRateControl",
                                 L"WrCalloutStack", L"WrKernel", L"WrResource", L"WrPushLock", L"WrMutex", L"WrQuantumEnd", L"WrDispatchInt",
                                 L"WrPreempted", L"WrYieldExecution", L"WrFastMutex", L"WrGuardedMutex", L"WrRundown", L"WrAlertByThreadId",
                                 L"WrDeferredPreempt", L"WrPhysicalFault", L"WrIoRing", L"WrMdlCache"};
    return r < sizeof(n) / sizeof(n[0]) ? n[r] : L"Unknown";
}

std::vector<ThreadInfo> QueryThreads(uint32_t pid) {
    std::vector<ThreadInfo> out;
    auto q = nt::Get().QuerySystemInformation;
    if (!q) return out;
    std::vector<uint8_t> buf(1024 * 1024);
    ULONG needed = 0;
    nt::NTSTATUS st;
    for (int i = 0; i < 6; ++i) {
        st = q(nt::SystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &needed);
        if (st != nt::STATUS_INFO_LENGTH_MISMATCH) break;
        buf.resize(needed + 256 * 1024);
    }
    if (!nt::NT_OK(st)) return out;

    std::wstring err;
    std::vector<ModuleInfo> mods = QueryModules(pid, err);
    using NtQIT = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto ntqit = reinterpret_cast<NtQIT>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto getDesc = reinterpret_cast<GetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

    for (size_t off = 0;;) {
        auto* p = reinterpret_cast<const nt::SYSTEM_PROCESS_INFORMATION*>(buf.data() + off);
        if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p->UniqueProcessId)) == pid) {
            for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
                const auto& t = p->Threads[i];
                ThreadInfo ti;
                ti.tid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t.ClientId.UniqueThread));
                ti.priority = t.Priority;
                ti.basePriority = t.BasePriority;
                ti.state = t.ThreadState;
                ti.waitReason = t.WaitReason;
                ti.contextSwitches = t.ContextSwitches;
                ti.kernelTime = static_cast<uint64_t>(t.KernelTime.QuadPart);
                ti.userTime = static_cast<uint64_t>(t.UserTime.QuadPart);
                ti.createTime = static_cast<uint64_t>(t.CreateTime.QuadPart);
                ti.startAddress = reinterpret_cast<uint64_t>(t.StartAddress);
                HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ti.tid);
                if (!th) th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ti.tid);
                if (th) {
                    PVOID start = nullptr;
                    if (ntqit && ntqit(th, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof start, nullptr) >= 0 && start)
                        ti.startAddress = reinterpret_cast<uint64_t>(start);
                    PWSTR d = nullptr;
                    if (getDesc && SUCCEEDED(getDesc(th, &d)) && d) {
                        ti.description = d;
                        LocalFree(d);
                    }
                    CloseHandle(th);
                }
                for (auto& m : mods) {
                    if (ti.startAddress >= m.base && ti.startAddress < m.base + m.size) {
                        wchar_t b[64];
                        swprintf(b, 64, L"+0x%llx", static_cast<unsigned long long>(ti.startAddress - m.base));
                        ti.startModule = m.name + b;
                        break;
                    }
                }
                out.push_back(std::move(ti));
            }
            break;
        }
        if (!p->NextEntryOffset) break;
        off += p->NextEntryOffset;
    }
    return out;
}

// ------------------------------------------------------------------------------------------
// Wait chains
// ------------------------------------------------------------------------------------------
namespace {
// File name of a process's executable, or empty when the process cannot be opened.
std::wstring ProcessFileName(uint32_t pid) {
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t path[1024];
    DWORD len = 1024;
    return h && QueryFullProcessImageNameW(h.Get(), 0, path, &len) ? FileNameOf(std::wstring(path, len)) : std::wstring();
}
} // namespace

std::wstring DescribeWaitChains(uint32_t pid) {
    HWCT session = OpenThreadWaitChainSession(0, nullptr);
    if (!session) return L"Wait chain analysis is unavailable: " + LastErrorMessage();
    auto threads = QueryThreads(pid);
    std::wstring out;
    int blocked = 0, deadlocked = 0, behindDeadlock = 0;
    for (auto& t : threads) {
        WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
        DWORD count = WCT_MAX_NODE_COUNT;
        BOOL cycle = FALSE;
        if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, t.tid, &count, nodes, &cycle) || count < 2) continue;
        ++blocked;
        // `cycle` is set when the chain ends in a cycle anywhere: the thread is part of the deadlock
        // only if the chain comes back to it; otherwise it waits on threads that are.
        bool inCycle = false;
        for (DWORD i = 1; i < count; ++i)
            if (nodes[i].ObjectType == WctThreadType && nodes[i].ThreadObject.ThreadId == t.tid) inCycle = true;
        if (cycle && inCycle) ++deadlocked;
        else if (cycle) ++behindDeadlock;
        std::wstring line = L"Thread " + std::to_wstring(t.tid);
        for (DWORD i = 1; i < count && i < 8; ++i) {
            auto& n = nodes[i];
            if (n.ObjectType == WctThreadType && n.ThreadObject.ThreadId == 0) { // the process that a process wait is for
                std::wstring exe = ProcessFileName(n.ThreadObject.ProcessId);
                line += L" \u2192 " + (exe.empty() ? std::wstring(L"process") : exe) + L" (PID " + std::to_wstring(n.ThreadObject.ProcessId) + L")";
            } else if (n.ObjectType == WctThreadType)
                line += L" \u2192 thread " + std::to_wstring(n.ThreadObject.ThreadId) + L" (PID " + std::to_wstring(n.ThreadObject.ProcessId) + L")";
            else {
                static const wchar_t* types[] = {L"", L"critical section", L"SendMessage", L"mutex", L"ALPC", L"COM", L"thread wait", L"process wait",
                                                 L"thread", L"COM activation", L"unknown", L"socket", L"SMB I/O"};
                int ty = static_cast<int>(n.ObjectType);
                line += std::wstring(L" \u2192 ") + (ty >= 0 && ty < 13 ? types[ty] : L"object");
                if (n.LockObject.ObjectName[0]) line += std::wstring(L" '") + n.LockObject.ObjectName + L"'";
            }
        }
        if (cycle) line += inCycle ? L"  [DEADLOCK]" : L"  [waits on a deadlock]";
        out += line + L"\n";
        if (out.size() > 6000) { out += L"\u2026"; break; }
    }
    CloseThreadWaitChainSession(session);
    if (!blocked) out = L"No thread is waiting on another thread, process or lock. The process is not blocked by a wait chain.";
    else
        out = std::to_wstring(blocked) + L" blocked thread(s)" + (deadlocked ? L", " + std::to_wstring(deadlocked) + L" in a deadlock cycle" : L"") +
              (behindDeadlock ? L", " + std::to_wstring(behindDeadlock) + L" waiting on a deadlocked thread" : L"") + L":\n\n" + out;
    // Suspension is not a wait chain, but it is why a suspended process looks stuck.
    size_t suspended = static_cast<size_t>(
        std::count_if(threads.begin(), threads.end(), [](const ThreadInfo& th) { return th.state == 5 /*Waiting*/ && th.waitReason == 5 /*Suspended*/; }));
    if (suspended && suspended == threads.size()) out = L"The process is suspended: its threads are frozen until it is resumed.\n\n" + out;
    else if (suspended) out = std::to_wstring(suspended) + L" of " + std::to_wstring(threads.size()) + L" threads are suspended.\n\n" + out;
    return out;
}

// ------------------------------------------------------------------------------------------
// Modules
// ------------------------------------------------------------------------------------------
std::vector<ModuleInfo> QueryModules(uint32_t pid, std::wstring& error) {
    std::vector<ModuleInfo> out;
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 5; ++i) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break; // retry while the loader is busy
    }
    if (snap == INVALID_HANDLE_VALUE) {
        error = LastErrorMessage();
        return out;
    }
    MODULEENTRY32W me{sizeof me};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        ModuleInfo m;
        m.name = me.szModule;
        m.path = me.szExePath;
        m.base = reinterpret_cast<uint64_t>(me.modBaseAddr);
        m.size = me.modBaseSize;
        FileVersionInfo v;
        if (ReadFileVersionInfo(m.path, v)) {
            m.description = v.description;
            m.company = v.company;
            m.version = v.version;
        }
        out.push_back(std::move(m));
    }
    CloseHandle(snap);
    return out;
}

// ------------------------------------------------------------------------------------------
// Memory map
// ------------------------------------------------------------------------------------------
std::wstring ProtectionText(uint32_t p) {
    std::wstring s;
    switch (p & 0xFF) {
    case PAGE_NOACCESS: s = L"No access"; break;
    case PAGE_READONLY: s = L"Read"; break;
    case PAGE_READWRITE: s = L"Read/Write"; break;
    case PAGE_WRITECOPY: s = L"Write copy"; break;
    case PAGE_EXECUTE: s = L"Execute"; break;
    case PAGE_EXECUTE_READ: s = L"Execute/Read"; break;
    case PAGE_EXECUTE_READWRITE: s = L"Execute/Read/Write"; break;
    case PAGE_EXECUTE_WRITECOPY: s = L"Execute/Write copy"; break;
    default: s = p ? FormatHex(p) : L""; break;
    }
    if (p & PAGE_GUARD) s += L" +Guard";
    if (p & PAGE_NOCACHE) s += L" +NoCache";
    if (p & PAGE_WRITECOMBINE) s += L" +WriteCombine";
    return s;
}

MemSummary QueryMemoryMap(uint32_t pid, std::wstring& error) {
    MemSummary s;
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!h) h.Reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) { error = LastErrorMessage(); return s; }
    uint8_t* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(h.Get(), addr, &mbi, sizeof mbi) == sizeof mbi && s.list.size() < 200000) {
        if (mbi.State != MEM_FREE) {
            MemRegion r;
            r.base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
            r.size = mbi.RegionSize;
            r.allocationBase = reinterpret_cast<uint64_t>(mbi.AllocationBase);
            r.state = mbi.State;
            r.protect = mbi.State == MEM_COMMIT ? mbi.Protect : mbi.AllocationProtect;
            r.type = mbi.Type;
            if (mbi.State == MEM_COMMIT) {
                if (mbi.Type == MEM_IMAGE) s.imageCommit += r.size;
                else if (mbi.Type == MEM_MAPPED) s.mappedCommit += r.size;
                else s.privateCommit += r.size;
            } else {
                s.reserved += r.size;
            }
            s.total += r.size;
            if ((mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) && mbi.BaseAddress == mbi.AllocationBase) {
                wchar_t name[MAX_PATH * 2];
                DWORD n = GetMappedFileNameW(h.Get(), mbi.BaseAddress, name, MAX_PATH * 2);
                if (n) r.mappedFile = NtPathToDos(std::wstring(name, n));
            }
            s.list.push_back(std::move(r));
        }
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    s.regions = s.list.size();
    return s;
}

// ------------------------------------------------------------------------------------------
// Handles (process-scoped snapshot; Windows 8+)
// ------------------------------------------------------------------------------------------
namespace {
struct PROCESS_HANDLE_TABLE_ENTRY_INFO_NX {
    HANDLE HandleValue;
    ULONG_PTR HandleCount;
    ULONG_PTR PointerCount;
    ACCESS_MASK GrantedAccess;
    ULONG ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};
struct PROCESS_HANDLE_SNAPSHOT_INFORMATION_NX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    PROCESS_HANDLE_TABLE_ENTRY_INFO_NX Handles[1];
};

std::map<uint32_t, std::wstring> ObjectTypeNames() {
    std::map<uint32_t, std::wstring> out;
    auto q = nt::Get().QueryObject;
    if (!q) return out;
    std::vector<uint8_t> buf(64 * 1024);
    ULONG ret = 0;
    nt::NTSTATUS st;
    for (int i = 0; i < 4; ++i) {
        st = q(nullptr, nt::ObjectTypesInformation, buf.data(), static_cast<ULONG>(buf.size()), &ret);
        if (st != nt::STATUS_INFO_LENGTH_MISMATCH) break;
        buf.resize(std::max<size_t>(ret, buf.size() * 2));
    }
    if (!nt::NT_OK(st)) return out;
    auto* info = reinterpret_cast<nt::OBJECT_TYPES_INFORMATION*>(buf.data());
    uint8_t* p = buf.data() + ((sizeof(nt::OBJECT_TYPES_INFORMATION) + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
    for (ULONG i = 0; i < info->NumberOfTypes; ++i) {
        if (p + sizeof(nt::OBJECT_TYPE_INFORMATION) > buf.data() + buf.size()) break;
        auto* t = reinterpret_cast<nt::OBJECT_TYPE_INFORMATION*>(p);
        uint8_t typeIndex = p[90]; // OBJECT_TYPE_INFORMATION::TypeIndex (Windows 8.1+)
        if (t->TypeName.Buffer) out[typeIndex ? typeIndex : i + 2] = std::wstring(t->TypeName.Buffer, t->TypeName.Length / sizeof(wchar_t));
        p += sizeof(nt::OBJECT_TYPE_INFORMATION) + ((t->TypeName.MaximumLength + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
    }
    return out;
}
} // namespace

std::vector<HandleInfo> QueryHandles(uint32_t pid, std::wstring& error) {
    std::vector<HandleInfo> out;
    auto qip = nt::Get().QueryInformationProcess;
    if (!qip) { error = L"Not supported"; return out; }
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid));
    bool canDup = true;
    if (!h) {
        h.Reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
        canDup = false;
    }
    if (!h) { error = LastErrorMessage(); return out; }
    std::vector<uint8_t> buf(64 * 1024);
    ULONG ret = 0;
    nt::NTSTATUS st;
    for (int i = 0; i < 8; ++i) {
        st = qip(h.Get(), 51 /*ProcessHandleInformation*/, buf.data(), static_cast<ULONG>(buf.size()), &ret);
        if (st != nt::STATUS_INFO_LENGTH_MISMATCH) break;
        buf.resize(std::max<size_t>(ret + 4096, buf.size() * 2));
    }
    if (!nt::NT_OK(st)) { error = L"Handle information is not available for this process."; return out; }
    auto types = ObjectTypeNames();
    auto* info = reinterpret_cast<PROCESS_HANDLE_SNAPSHOT_INFORMATION_NX*>(buf.data());
    size_t fits = (buf.size() - offsetof(PROCESS_HANDLE_SNAPSHOT_INFORMATION_NX, Handles)) / sizeof(PROCESS_HANDLE_TABLE_ENTRY_INFO_NX);
    size_t n = std::min<size_t>({info->NumberOfHandles, fits, 50000});
    std::vector<uint8_t> nameBuf(8192);
    for (size_t i = 0; i < n; ++i) {
        auto& e = info->Handles[i];
        HandleInfo hi;
        hi.value = reinterpret_cast<uint64_t>(e.HandleValue);
        hi.access = e.GrantedAccess;
        hi.typeIndex = static_cast<uint16_t>(e.ObjectTypeIndex);
        auto t = types.find(e.ObjectTypeIndex);
        hi.type = t != types.end() ? t->second : L"Type " + std::to_wstring(e.ObjectTypeIndex);
        if (canDup && i < 20000) {
            HANDLE dup = nullptr;
            if (DuplicateHandle(h.Get(), e.HandleValue, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                DuplicateHandle(h.Get(), e.HandleValue, GetCurrentProcess(), &dup, 0, FALSE, 0)) {
                if (hi.type == L"File") {
                    // Only disk files: querying names of pipes and some devices can block indefinitely.
                    if (GetFileType(dup) == FILE_TYPE_DISK) {
                        wchar_t p[MAX_PATH * 2];
                        DWORD len = GetFinalPathNameByHandleW(dup, p, MAX_PATH * 2, VOLUME_NAME_DOS);
                        if (len && len < MAX_PATH * 2) {
                            hi.name.assign(p, len);
                            if (hi.name.rfind(L"\\\\?\\", 0) == 0) hi.name = hi.name.substr(4);
                        }
                    }
                } else if (hi.type == L"Process") {
                    DWORD p = GetProcessId(dup);
                    if (p) hi.name = L"PID " + std::to_wstring(p);
                } else if (hi.type == L"Thread") {
                    DWORD t2 = GetThreadId(dup);
                    if (t2) hi.name = L"TID " + std::to_wstring(t2);
                } else if (nt::Get().QueryObject) {
                    ULONG r2 = 0;
                    if (nt::NT_OK(nt::Get().QueryObject(dup, nt::ObjectNameInformation, nameBuf.data(), static_cast<ULONG>(nameBuf.size()), &r2))) {
                        auto* us = reinterpret_cast<nt::UNICODE_STRING*>(nameBuf.data());
                        if (us->Buffer && us->Length) hi.name.assign(us->Buffer, us->Length / sizeof(wchar_t));
                    }
                }
                CloseHandle(dup);
            }
        }
        out.push_back(std::move(hi));
    }
    if (!canDup) error = L"Object names unavailable: the process denies handle duplication (try running as administrator).";
    return out;
}

// ------------------------------------------------------------------------------------------
// Token
// ------------------------------------------------------------------------------------------
namespace {
std::wstring SidName(PSID sid) {
    wchar_t name[256], dom[256];
    DWORD nl = 256, dl = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, name, &nl, dom, &dl, &use)) return dl ? std::wstring(dom) + L"\\" + name : std::wstring(name);
    LPWSTR s = nullptr;
    std::wstring r;
    if (ConvertSidToStringSidW(sid, &s)) { r = s; LocalFree(s); }
    return r;
}
std::vector<uint8_t> TokInfo(HANDLE tok, TOKEN_INFORMATION_CLASS c) {
    DWORD len = 0;
    GetTokenInformation(tok, c, nullptr, 0, &len);
    std::vector<uint8_t> b(len);
    if (!len || !GetTokenInformation(tok, c, b.data(), len, &len)) b.clear();
    return b;
}
} // namespace

TokenDetails QueryToken(uint32_t pid, std::wstring& error) {
    TokenDetails t;
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE tok = nullptr;
    if (!h || !OpenProcessToken(h.Get(), TOKEN_QUERY, &tok)) { error = LastErrorMessage(); return t; }
    Handle tk(tok);
    auto b = TokInfo(tok, TokenUser);
    if (!b.empty()) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(b.data());
        t.user = SidName(tu->User.Sid);
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &s)) { t.sid = s; LocalFree(s); }
    }
    DWORD len = 0, v = 0;
    if (GetTokenInformation(tok, TokenSessionId, &v, sizeof v, &len)) t.session = v;
    TOKEN_ELEVATION el{};
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &len)) t.elevated = el.TokenIsElevated != 0;
    TOKEN_ELEVATION_TYPE et{};
    if (GetTokenInformation(tok, TokenElevationType, &et, sizeof et, &len))
        t.elevationType = et == TokenElevationTypeFull ? L"Full" : et == TokenElevationTypeLimited ? L"Limited" : L"Default";
    if (GetTokenInformation(tok, TokenVirtualizationAllowed, &v, sizeof v, &len)) t.virtualizationAllowed = v != 0;
    if (GetTokenInformation(tok, TokenVirtualizationEnabled, &v, sizeof v, &len)) t.virtualizationEnabled = v != 0;
    if (GetTokenInformation(tok, TokenIsAppContainer, &v, sizeof v, &len)) t.appContainer = v != 0;
    b = TokInfo(tok, TokenIntegrityLevel);
    if (!b.empty()) {
        auto* ml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(b.data());
        DWORD rid = *GetSidSubAuthority(ml->Label.Sid, *GetSidSubAuthorityCount(ml->Label.Sid) - 1);
        t.integrity = IntegrityName(rid >= 0x5000 ? 5 : rid >= 0x4000 ? 4 : rid >= 0x3000 ? 3 : rid >= 0x2000 ? 2 : rid >= 0x1000 ? 1 : 0);
    }
    TOKEN_STATISTICS ts{};
    if (GetTokenInformation(tok, TokenStatistics, &ts, sizeof ts, &len)) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%08lX-%08lX", static_cast<unsigned long>(ts.AuthenticationId.HighPart), ts.AuthenticationId.LowPart);
        t.logonSession = buf;
    }
    b = TokInfo(tok, TokenGroups);
    if (!b.empty()) {
        auto* g = reinterpret_cast<TOKEN_GROUPS*>(b.data());
        for (DWORD i = 0; i < g->GroupCount; ++i) {
            TokenDetails::Group gr;
            gr.name = SidName(g->Groups[i].Sid);
            DWORD a = g->Groups[i].Attributes;
            std::wstring f;
            auto add = [&](const wchar_t* x) { if (!f.empty()) f += L", "; f += x; };
            if (a & SE_GROUP_INTEGRITY) add(L"Integrity");
            if (a & SE_GROUP_LOGON_ID) add(L"Logon ID");
            if (a & SE_GROUP_OWNER) add(L"Owner");
            if (a & SE_GROUP_USE_FOR_DENY_ONLY) add(L"Deny only");
            if (a & SE_GROUP_MANDATORY) add(L"Mandatory");
            if (a & SE_GROUP_ENABLED) add(L"Enabled");
            gr.flags = f;
            t.groups.push_back(std::move(gr));
        }
    }
    b = TokInfo(tok, TokenPrivileges);
    if (!b.empty()) {
        auto* p = reinterpret_cast<TOKEN_PRIVILEGES*>(b.data());
        for (DWORD i = 0; i < p->PrivilegeCount; ++i) {
            TokenDetails::Privilege pr;
            wchar_t name[128];
            DWORD nl = 128;
            if (LookupPrivilegeNameW(nullptr, &p->Privileges[i].Luid, name, &nl)) pr.name = name;
            wchar_t disp[256];
            DWORD dl = 256, lang = 0;
            if (!pr.name.empty() && LookupPrivilegeDisplayNameW(nullptr, pr.name.c_str(), disp, &dl, &lang)) pr.description = disp;
            pr.enabled = (p->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
            pr.enabledByDefault = (p->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT) != 0;
            t.privileges.push_back(std::move(pr));
        }
    }
    return t;
}

// ------------------------------------------------------------------------------------------
// Environment / current directory (reads the target's process parameters; x64 layout)
// ------------------------------------------------------------------------------------------
namespace {
bool ReadParams(HANDLE h, uint64_t& envAddr, uint64_t& envSize, std::wstring* cwd) {
#if defined(_M_X64) || defined(__x86_64__)
    auto qip = nt::Get().QueryInformationProcess;
    if (!qip) return false;
    nt::PROCESS_BASIC_INFORMATION pbi{};
    if (!nt::NT_OK(qip(h, nt::ProcessBasicInformation, &pbi, sizeof pbi, nullptr)) || !pbi.PebBaseAddress) return false;
    uint64_t params = 0;
    SIZE_T rd = 0;
    if (!ReadProcessMemory(h, static_cast<uint8_t*>(pbi.PebBaseAddress) + 0x20, &params, 8, &rd) || !params) return false;
    uint8_t pp[0x400];
    if (!ReadProcessMemory(h, reinterpret_cast<void*>(params), pp, sizeof pp, &rd)) return false;
    memcpy(&envAddr, pp + 0x80, 8);
    memcpy(&envSize, pp + 0x3F0, 8);
    if (cwd) {
        nt::UNICODE_STRING us;
        memcpy(&us, pp + 0x38, sizeof us);
        // Length comes from the (possibly hostile) target: read whole characters only, so an odd
        // length cannot write past the buffer.
        size_t chars = us.Length / sizeof(wchar_t);
        if (us.Buffer && chars && us.Length < 65534) {
            std::wstring s(chars, L'\0');
            if (ReadProcessMemory(h, us.Buffer, s.data(), chars * sizeof(wchar_t), &rd)) *cwd = s.substr(0, rd / sizeof(wchar_t));
        }
    }
    return true;
#else
    return false;
#endif
}
} // namespace

std::vector<std::pair<std::wstring, std::wstring>> QueryEnvironment(uint32_t pid, std::wstring& error) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!h) { error = LastErrorMessage(); return out; }
    uint64_t addr = 0, size = 0;
    if (!ReadParams(h.Get(), addr, size, nullptr) || !addr) { error = L"The process environment could not be read."; return out; }
    if (size == 0 || size > 4 * 1024 * 1024) size = 64 * 1024;
    std::vector<wchar_t> env(static_cast<size_t>(size / sizeof(wchar_t)) + 2, 0);
    SIZE_T rd = 0;
    if (!ReadProcessMemory(h.Get(), reinterpret_cast<void*>(addr), env.data(), static_cast<SIZE_T>(size), &rd) && rd == 0) {
        error = LastErrorMessage();
        return out;
    }
    size_t n = rd / sizeof(wchar_t);
    for (size_t i = 0; i < n && env[i];) {
        size_t start = i;
        while (i < n && env[i]) ++i;
        std::wstring kv(env.data() + start, i - start);
        ++i;
        size_t eq = kv.find(L'=', 1); // keys like "=C:" start with '='
        if (eq == std::wstring::npos) continue;
        out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    return out;
}

// ------------------------------------------------------------------------------------------
// General
// ------------------------------------------------------------------------------------------
ProcessGeneral QueryGeneral(uint32_t pid, const ProcMeta* meta) {
    ProcessGeneral g;
    g.pid = pid;
    if (meta) {
        g.path = meta->path;
        g.commandLine = meta->commandLine;
        g.packageName = meta->packageName;
        g.architecture = MachineName(meta->machine);
        g.integrity = IntegrityName(meta->integrity);
        g.protection = meta->isProtected ? L"Protected process" : L"None";
    }
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!h) h.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (h) {
        g.accessible = true;
        FILETIME c, e, k, u;
        if (GetProcessTimes(h.Get(), &c, &e, &k, &u)) {
            FILETIME lc;
            FileTimeToLocalFileTime(&c, &lc);
            g.startTime = FormatFileTime(FileTimeToU64(lc));
        }
        g.priority = PriorityClassName(GetPriorityClass(h.Get()));
        DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(h.Get(), &pm, &sm)) { g.affinity = pm; g.systemAffinity = sm; }
        PROCESS_MITIGATION_DEP_POLICY dep{};
        if (GetProcessMitigationPolicy(h.Get(), ProcessDEPPolicy, &dep, sizeof dep)) g.dep = dep.Enable ? (dep.Permanent ? L"Enabled (permanent)" : L"Enabled") : L"Disabled";
        PROCESS_MITIGATION_ASLR_POLICY aslr{};
        if (GetProcessMitigationPolicy(h.Get(), ProcessASLRPolicy, &aslr, sizeof aslr))
            g.aslr = aslr.EnableBottomUpRandomization || aslr.EnableForceRelocateImages || aslr.EnableHighEntropy
                         ? (aslr.EnableHighEntropy ? L"Enabled (high entropy)" : L"Enabled")
                         : L"Image-based";
        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
        if (GetProcessMitigationPolicy(h.Get(), ProcessControlFlowGuardPolicy, &cfg, sizeof cfg)) g.cfg = cfg.EnableControlFlowGuard ? L"Enabled" : L"Disabled";
        DWORD shadow = 0;
        if (GetProcessMitigationPolicy(h.Get(), static_cast<PROCESS_MITIGATION_POLICY>(15) /*ProcessUserShadowStackPolicy*/, &shadow, sizeof shadow))
            g.cet = (shadow & 1) ? ((shadow & 2) ? L"Enabled (strict)" : L"Enabled (compatibility)") : L"Disabled";
        uint64_t a = 0, s = 0;
        ReadParams(h.Get(), a, s, &g.currentDirectory);
    }
    if (!g.path.empty()) {
        FileVersionInfo v;
        if (ReadFileVersionInfo(g.path, v)) {
            g.description = v.description; g.company = v.company; g.version = v.version; g.product = v.product; g.copyright = v.copyright;
        }
        g.signature = VerifyFileSignature(g.path);
    }
    return g;
}

} // namespace nx
