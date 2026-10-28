#include "data/Processes.h"
#include "core/Format.h"
#include "core/Win.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <unordered_set>

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif

namespace nx {

const wchar_t* GpuEngineTypeName(GpuEngineType t) {
    switch (t) {
    case GpuEngineType::ThreeD: return L"3D";
    case GpuEngineType::Copy: return L"Copy";
    case GpuEngineType::VideoDecode: return L"Video Decode";
    case GpuEngineType::VideoEncode: return L"Video Encode";
    case GpuEngineType::VideoProcessing: return L"Video Processing";
    case GpuEngineType::Compute: return L"Compute";
    case GpuEngineType::Other: return L"Other";
    default: return L"";
    }
}

const std::wstring& ProcSample::Name() const {
    static const std::wstring empty;
    return meta ? meta->name : empty;
}

const ProcSample* Snapshot::FindProc(uint32_t pid) const {
    auto it = std::lower_bound(procs.begin(), procs.end(), pid, [](const ProcSample& p, uint32_t v) { return p.pid < v; });
    return it != procs.end() && it->pid == pid ? &*it : nullptr;
}

namespace {

bool HasThreadsAllSuspended(const nt::SYSTEM_PROCESS_INFORMATION* p) {
    if (p->NumberOfThreads == 0) return false;
    for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
        const auto& t = p->Threads[i];
        if (t.ThreadState != 5 /*Waiting*/ || t.WaitReason != 5 /*Suspended*/) return false;
    }
    return true;
}

struct PROCESS_POWER_THROTTLING_STATE_NX { ULONG Version; ULONG ControlMask; ULONG StateMask; };

float PowerScore(const ProcSample& p) {
    // Heuristic estimate (documented in docs/LIMITATIONS.md): CPU dominates, GPU and I/O add load.
    float io = (p.ioReadRate + p.ioWriteRate) / (1024.f * 1024.f);
    float net = p.netSend >= 0 ? (p.netSend + p.netRecv) / (1024.f * 1024.f) : 0.f;
    return p.cpu + p.gpu * 0.6f + std::min(io, 100.f) * 0.15f + std::min(net, 100.f) * 0.2f;
}

} // namespace

ProcessCollector::ProcessCollector() = default;

ProcessCollector::~ProcessCollector() {
    for (auto& [pid, p] : prev_)
        if (p.handle) CloseHandle(p.handle);
}

void ProcessCollector::Collect(Snapshot& snap, double interval, uint32_t demandFlags, unsigned cpus) {
    ++pass_;
    if (!CollectNt(snap, interval, demandFlags, cpus)) CollectToolhelp(snap, interval, demandFlags, cpus);
    Finish(snap, demandFlags);
    first_ = false;
}

bool ProcessCollector::CollectNt(Snapshot& snap, double interval, uint32_t demandFlags, unsigned cpus) {
    auto& api = nt::Get();
    if (!api.QuerySystemInformation) return false;
    if (buf_.empty()) buf_.resize(512 * 1024);
    ULONG needed = 0;
    nt::NTSTATUS st;
    for (int tries = 0; tries < 8; ++tries) {
        st = api.QuerySystemInformation(nt::SystemProcessInformation, buf_.data(), static_cast<ULONG>(buf_.size()), &needed);
        if (st != nt::STATUS_INFO_LENGTH_MISMATCH && st != nt::STATUS_BUFFER_TOO_SMALL) break;
        buf_.resize(std::max<size_t>(needed + 64 * 1024, buf_.size() * 2));
    }
    if (!nt::NT_OK(st)) return false;

    const double cpuDen = interval * 1e7 * std::max(1u, cpus);
    snap.procs.clear();
    snap.procs.reserve(prev_.size() + 32);
    for (auto& [pid, p] : prev_) p.seen = false;
    uint32_t totalThreads = 0, totalHandles = 0;

    size_t off = 0;
    for (;;) {
        if (off + sizeof(nt::SYSTEM_PROCESS_INFORMATION) > buf_.size()) break;
        auto* p = reinterpret_cast<const nt::SYSTEM_PROCESS_INFORMATION*>(buf_.data() + off);
        ProcSample s;
        s.pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p->UniqueProcessId));
        s.ppid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p->InheritedFromUniqueProcessId));
        s.session = p->SessionId;
        s.threads = p->NumberOfThreads;
        s.handles = p->HandleCount;
        s.basePriority = p->BasePriority;
        s.createTime = static_cast<uint64_t>(p->CreateTime.QuadPart);
        s.kernelTime = static_cast<uint64_t>(p->KernelTime.QuadPart);
        s.cpuTime = s.kernelTime + static_cast<uint64_t>(p->UserTime.QuadPart);
        s.cycles = p->CycleTime;
        s.workingSet = p->WorkingSetSize;
        s.privateWorkingSet = static_cast<uint64_t>(p->WorkingSetPrivateSize.QuadPart);
        s.privateBytes = p->PagefileUsage;
        s.peakWorkingSet = p->PeakWorkingSetSize;
        s.pagedPool = p->QuotaPagedPoolUsage;
        s.nonPagedPool = p->QuotaNonPagedPoolUsage;
        s.virtualSize = p->VirtualSize;
        s.pageFaults = p->PageFaultCount;
        s.hardFaults = p->HardFaultCount;
        s.ioReadBytes = static_cast<uint64_t>(p->ReadTransferCount.QuadPart);
        s.ioWriteBytes = static_cast<uint64_t>(p->WriteTransferCount.QuadPart);
        s.ioOtherBytes = static_cast<uint64_t>(p->OtherTransferCount.QuadPart);
        s.ioReadOps = static_cast<uint64_t>(p->ReadOperationCount.QuadPart);
        s.ioWriteOps = static_cast<uint64_t>(p->WriteOperationCount.QuadPart);
        s.ioOtherOps = static_cast<uint64_t>(p->OtherOperationCount.QuadPart);
        if (s.pid != 0 && HasThreadsAllSuspended(p)) s.status = ProcStatus::Suspended;
        totalThreads += s.threads;
        totalHandles += s.handles;

        if (s.pid == 0) {
            // System Idle Process: cpu time = idle time; report it like Task Manager does.
            s.cpuTime = s.kernelTime;
        }

        auto it = prev_.find(s.pid);
        if (it != prev_.end() && it->second.createTime != s.createTime) {
            // PID reused by a new process instance.
            if (it->second.handle) CloseHandle(it->second.handle);
            prev_.erase(it);
            it = prev_.end();
        }
        if (it == prev_.end()) {
            Prev pr;
            pr.createTime = s.createTime;
            pr.cpuTime = s.cpuTime;
            pr.kernelTime = s.kernelTime;
            pr.ioR = s.ioReadBytes; pr.ioW = s.ioWriteBytes; pr.ioO = s.ioOtherBytes;
            pr.pageFaults = s.pageFaults;
            pr.ws = s.workingSet;
            std::wstring name;
            if (s.pid == 0) name = L"System Idle Process";
            else if (p->ImageName.Buffer && p->ImageName.Length) name.assign(p->ImageName.Buffer, p->ImageName.Length / sizeof(wchar_t));
            else name = L"System";
            if (s.pid != 0) pr.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.pid);
            pr.meta = AcquireProcMeta(s.pid, name, s.session, pr.handle);
            it = prev_.emplace(s.pid, std::move(pr)).first;
            s.isNew = !first_;
        }
        Prev& pr = it->second;
        pr.seen = true;
        if (s.cpuTime >= pr.cpuTime) s.cpu = static_cast<float>(100.0 * static_cast<double>(s.cpuTime - pr.cpuTime) / cpuDen);
        if (s.kernelTime >= pr.kernelTime) s.cpuKernel = static_cast<float>(100.0 * static_cast<double>(s.kernelTime - pr.kernelTime) / cpuDen);
        s.cpu = std::min(s.cpu, 100.f);
        if (interval > 0) {
            s.ioReadRate = s.ioReadBytes >= pr.ioR ? static_cast<float>((s.ioReadBytes - pr.ioR) / interval) : 0.f;
            s.ioWriteRate = s.ioWriteBytes >= pr.ioW ? static_cast<float>((s.ioWriteBytes - pr.ioW) / interval) : 0.f;
            s.ioOtherRate = s.ioOtherBytes >= pr.ioO ? static_cast<float>((s.ioOtherBytes - pr.ioO) / interval) : 0.f;
        }
        s.pageFaultDelta = s.pageFaults >= pr.pageFaults ? s.pageFaults - pr.pageFaults : 0;
        s.workingSetDelta = static_cast<int64_t>(s.workingSet) - static_cast<int64_t>(pr.ws);
        pr.cpuTime = s.cpuTime; pr.kernelTime = s.kernelTime;
        pr.ioR = s.ioReadBytes; pr.ioW = s.ioWriteBytes; pr.ioO = s.ioOtherBytes;
        pr.pageFaults = s.pageFaults; pr.ws = s.workingSet;

        // Moderately expensive per-process queries: refreshed every few samples, and only when needed.
        if (pr.handle && (demandFlags & demand::ProcessExtra) && ++pr.extraAge >= 3) {
            pr.extraAge = 0;
            pr.priorityClass = GetPriorityClass(pr.handle);
            pr.gdi = GetGuiResources(pr.handle, GR_GDIOBJECTS);
            pr.user = GetGuiResources(pr.handle, GR_USEROBJECTS);
            PROCESS_POWER_THROTTLING_STATE_NX pt{PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0};
            pr.efficiency = GetProcessInformation(pr.handle, static_cast<PROCESS_INFORMATION_CLASS>(4) /*ProcessPowerThrottling*/, &pt, sizeof pt) &&
                            (pt.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) && (pt.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
        }
        s.priorityClass = pr.priorityClass;
        s.gdiObjects = pr.gdi;
        s.userObjects = pr.user;
        s.efficiencyMode = pr.efficiency;
        s.meta = pr.meta;
        snap.procs.push_back(std::move(s));

        if (!p->NextEntryOffset) break;
        off += p->NextEntryOffset;
    }

    snap.cpu.threads = totalThreads;
    snap.cpu.handles = totalHandles;
    return true;
}

bool ProcessCollector::CollectToolhelp(Snapshot& snap, double interval, uint32_t /*demandFlags*/, unsigned cpus) {
    // Fallback path using only documented APIs.
    Handle snapH(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapH) return false;
    PROCESSENTRY32W pe{sizeof pe};
    const double cpuDen = interval * 1e7 * std::max(1u, cpus);
    snap.procs.clear();
    for (auto& [pid, p] : prev_) p.seen = false;
    uint32_t totalThreads = 0;
    for (BOOL ok = Process32FirstW(snapH.Get(), &pe); ok; ok = Process32NextW(snapH.Get(), &pe)) {
        ProcSample s;
        s.pid = pe.th32ProcessID;
        s.ppid = pe.th32ParentProcessID;
        s.threads = pe.cntThreads;
        s.basePriority = pe.pcPriClassBase;
        DWORD sess = 0;
        ProcessIdToSessionId(s.pid, &sess);
        s.session = sess;
        totalThreads += s.threads;
        HANDLE h = s.pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.pid) : nullptr;
        if (h) {
            FILETIME c, e, k, u;
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                s.createTime = FileTimeToU64(c);
                s.kernelTime = FileTimeToU64(k);
                s.cpuTime = s.kernelTime + FileTimeToU64(u);
            }
            PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
            if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
                s.workingSet = pmc.WorkingSetSize;
                s.privateBytes = pmc.PrivateUsage;
                s.peakWorkingSet = pmc.PeakWorkingSetSize;
                s.pageFaults = pmc.PageFaultCount;
                s.pagedPool = pmc.QuotaPagedPoolUsage;
                s.nonPagedPool = pmc.QuotaNonPagedPoolUsage;
            }
            IO_COUNTERS io{};
            if (GetProcessIoCounters(h, &io)) {
                s.ioReadBytes = io.ReadTransferCount; s.ioWriteBytes = io.WriteTransferCount; s.ioOtherBytes = io.OtherTransferCount;
                s.ioReadOps = io.ReadOperationCount; s.ioWriteOps = io.WriteOperationCount; s.ioOtherOps = io.OtherOperationCount;
            }
            DWORD hc = 0;
            if (GetProcessHandleCount(h, &hc)) s.handles = hc;
        }
        auto it = prev_.find(s.pid);
        if (it != prev_.end() && it->second.createTime != s.createTime) {
            if (it->second.handle) CloseHandle(it->second.handle);
            prev_.erase(it);
            it = prev_.end();
        }
        if (it == prev_.end()) {
            Prev pr;
            pr.createTime = s.createTime;
            pr.cpuTime = s.cpuTime;
            pr.handle = h;
            h = nullptr;
            pr.meta = AcquireProcMeta(s.pid, s.pid ? pe.szExeFile : L"System Idle Process", s.session, pr.handle);
            it = prev_.emplace(s.pid, std::move(pr)).first;
            s.isNew = !first_;
        }
        if (h) CloseHandle(h);
        Prev& pr = it->second;
        pr.seen = true;
        if (s.cpuTime >= pr.cpuTime) s.cpu = std::min(100.f, static_cast<float>(100.0 * static_cast<double>(s.cpuTime - pr.cpuTime) / cpuDen));
        if (interval > 0) {
            s.ioReadRate = s.ioReadBytes >= pr.ioR ? static_cast<float>((s.ioReadBytes - pr.ioR) / interval) : 0.f;
            s.ioWriteRate = s.ioWriteBytes >= pr.ioW ? static_cast<float>((s.ioWriteBytes - pr.ioW) / interval) : 0.f;
        }
        pr.cpuTime = s.cpuTime;
        pr.ioR = s.ioReadBytes; pr.ioW = s.ioWriteBytes; pr.ioO = s.ioOtherBytes;
        s.meta = pr.meta;
        snap.procs.push_back(std::move(s));
    }
    std::sort(snap.procs.begin(), snap.procs.end(), [](const ProcSample& a, const ProcSample& b) { return a.pid < b.pid; });
    snap.cpu.threads = totalThreads;
    return true;
}

void ProcessCollector::Finish(Snapshot& snap, uint32_t demandFlags) {
    // Drop vanished processes.
    for (auto it = prev_.begin(); it != prev_.end();) {
        if (!it->second.seen) {
            snap.terminated.push_back(it->first);
            if (it->second.handle) CloseHandle(it->second.handle);
            it = prev_.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(snap.procs.begin(), snap.procs.end(), [](const ProcSample& a, const ProcSample& b) { return a.pid < b.pid; });
    snap.cpu.processes = static_cast<uint32_t>(std::count_if(snap.procs.begin(), snap.procs.end(), [](const ProcSample& p) { return p.pid != 0; }));

    for (auto& s : snap.procs) {
        s.category = s.meta && (s.meta->inWindowsDir || s.pid <= 4 || s.meta->path.empty()) ? ProcCategory::Windows : ProcCategory::Background;
        auto g = gpu.find(s.pid);
        if (g != gpu.end()) {
            s.gpu = g->second.util;
            s.gpuEngine = g->second.engine;
            s.gpuAdapter = g->second.adapter;
            s.gpuDedicated = g->second.dedicated;
            s.gpuShared = g->second.shared;
        }
        if (netValid) {
            auto n = net.find(s.pid);
            s.netSend = n != net.end() ? static_cast<float>(n->second.send) : 0.f;
            s.netRecv = n != net.end() ? static_cast<float>(n->second.recv) : 0.f;
        }
        // Power band with ~2 minute smoothing for the trend column.
        auto pr = prev_.find(s.pid);
        float score = PowerScore(s);
        if (pr != prev_.end()) {
            pr->second.powerAvg = pr->second.powerAvg * 0.983f + score * 0.017f;
            s.powerTrend = pr->second.powerAvg;
        }
        s.power = score;
    }
    if (demandFlags & demand::AppWindows) ClassifyWindows(snap);
}

namespace {
struct EnumCtx {
    std::unordered_map<uint32_t, HWND> windows;
    DWORD selfPid;
};

BOOL CALLBACK EnumChildForCore(HWND child, LPARAM lp) {
    auto* pair = reinterpret_cast<std::pair<DWORD, DWORD>*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid != pair->first) { pair->second = pid; return FALSE; }
    return TRUE;
}

BOOL CALLBACK EnumTop(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Packaged apps are hosted in ApplicationFrameHost; the real owner is the CoreWindow child.
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        std::pair<DWORD, DWORD> pr{pid, 0};
        EnumChildWindows(hwnd, EnumChildForCore, reinterpret_cast<LPARAM>(&pr));
        if (pr.second) pid = pr.second;
    }
    if (!ctx->windows.count(pid)) ctx->windows[pid] = hwnd;
    return TRUE;
}
} // namespace

void ProcessCollector::ClassifyWindows(Snapshot& snap) {
    EnumCtx ctx;
    ctx.selfPid = GetCurrentProcessId();
    EnumWindows(EnumTop, reinterpret_cast<LPARAM>(&ctx));
    for (auto& s : snap.procs) {
        auto it = ctx.windows.find(s.pid);
        if (it == ctx.windows.end()) continue;
        // explorer.exe is listed as an app by Task Manager ("Windows Explorer"); others in the
        // Windows directory with windows are apps too (e.g. Notepad).
        s.category = ProcCategory::App;
        s.mainWindow = it->second;
        wchar_t title[256];
        int n = GetWindowTextW(it->second, title, 256);
        s.windowTitle = std::make_shared<const std::wstring>(title, n > 0 ? n : 0);
        if (s.status == ProcStatus::Running && IsHungAppWindow(it->second)) s.status = ProcStatus::NotResponding;
    }
}

} // namespace nx
