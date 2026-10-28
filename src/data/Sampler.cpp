#include "data/Sampler.h"
#include "core/Format.h"
#include "core/Win.h"
#include "data/Connections.h"
#include "data/Pdh.h"
#include "data/Processes.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <map>

namespace nx {

struct Sampler::Impl {
    ProcessCollector procs;
    PdhQuery sys;
    int cUtil = -1, cPriv = -1, cPerf = -1, cLimit = -1, cDpc = -1, cIntr = -1, cIntrPerSec = -1;
    int cQueue = -1, cCtx = -1, cSyscalls = -1;
    int cStandbyCore = -1, cStandbyNormal = -1, cStandbyReserve = -1, cModified = -1, cFreeZero = -1, cPoolPaged = -1, cPoolNp = -1;
    int cPagesSec = -1, cFaultsSec = -1, cPageReads = -1, cPageWrites = -1, cCacheBytes = -1, cPagefile = -1;
    int cThermal = -1;

    std::unique_ptr<PdhQuery> disk;
    int dIdle = -1, dLat = -1, dLatR = -1, dLatW = -1, dRead = -1, dWrite = -1, dQueue = -1, dReads = -1, dWrites = -1;

    std::unique_ptr<PdhQuery> gpu;
    int gEngine = -1, gProcDed = -1, gProcShared = -1, gAdDed = -1, gAdShared = -1;
    std::vector<GpuStatic> gpuStatic;
    bool gpuStaticLoaded = false;
    uint32_t gpuSensorTick = 0;
    std::vector<GpuSnap> lastSensors;

    struct NicPrev { uint64_t in = 0, out = 0, inP = 0, outP = 0; };
    std::map<uint64_t, NicPrev> nicPrev;

    std::shared_ptr<const std::unordered_map<uint32_t, std::wstring>> services;
    uint64_t servicesTick = 0;

    std::unique_ptr<NetTrace> netTrace;

    std::vector<PdhQuery::Item> items, items2, items3;
    std::vector<std::pair<std::wstring, int64_t>> large, large2;
};

Sampler::Sampler(HWND notifyWindow, UINT msg) : notify_(notifyWindow), msg_(msg), impl_(std::make_unique<Impl>()) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Sampler::~Sampler() {
    Stop();
    if (wake_) CloseHandle(wake_);
}

void Sampler::Start() {
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { Run(); });
}

void Sampler::Stop() {
    quit_ = true;
    Wake();
    if (thread_.joinable()) thread_.join();
}

void Sampler::Wake() { if (wake_) SetEvent(wake_); }

SnapshotPtr Sampler::Latest() const {
    std::lock_guard<std::mutex> l(lock_);
    return latest_;
}

std::wstring Sampler::NetTraceStatus() const {
    if (netTraceActive_) return L"Per-process network activity is measured with Event Tracing for Windows.";
    if (!IsElevated()) return L"Per-process network throughput requires running NEXUS as administrator (kernel network ETW events).";
    if (impl_->netTrace) return impl_->netTrace->Error();
    return L"Per-process network measurement is disabled in Settings.";
}

void Sampler::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // stay responsive under load
    cpu_ = QueryCpuStatic();
    Impl& m = *impl_;

    // Core system counters (always on; all are cheap).
    m.cUtil = m.sys.Add(L"\\Processor Information(*)\\% Processor Utility");
    m.cPriv = m.sys.Add(L"\\Processor Information(*)\\% Privileged Utility");
    m.cPerf = m.sys.Add(L"\\Processor Information(*)\\% Processor Performance");
    m.cLimit = m.sys.Add(L"\\Processor Information(_Total)\\% Performance Limit");
    m.cDpc = m.sys.Add(L"\\Processor Information(_Total)\\% DPC Time");
    m.cIntr = m.sys.Add(L"\\Processor Information(_Total)\\% Interrupt Time");
    m.cIntrPerSec = m.sys.Add(L"\\Processor Information(_Total)\\Interrupts/sec");
    m.cQueue = m.sys.Add(L"\\System\\Processor Queue Length");
    m.cCtx = m.sys.Add(L"\\System\\Context Switches/sec");
    m.cSyscalls = m.sys.Add(L"\\System\\System Calls/sec");
    m.cStandbyCore = m.sys.Add(L"\\Memory\\Standby Cache Core Bytes");
    m.cStandbyNormal = m.sys.Add(L"\\Memory\\Standby Cache Normal Priority Bytes");
    m.cStandbyReserve = m.sys.Add(L"\\Memory\\Standby Cache Reserve Bytes");
    m.cModified = m.sys.Add(L"\\Memory\\Modified Page List Bytes");
    m.cFreeZero = m.sys.Add(L"\\Memory\\Free & Zero Page List Bytes");
    m.cPoolPaged = m.sys.Add(L"\\Memory\\Pool Paged Bytes");
    m.cPoolNp = m.sys.Add(L"\\Memory\\Pool Nonpaged Bytes");
    m.cPagesSec = m.sys.Add(L"\\Memory\\Pages/sec");
    m.cFaultsSec = m.sys.Add(L"\\Memory\\Page Faults/sec");
    m.cPageReads = m.sys.Add(L"\\Memory\\Page Reads/sec");
    m.cPageWrites = m.sys.Add(L"\\Memory\\Page Writes/sec");
    m.cCacheBytes = m.sys.Add(L"\\Memory\\Cache Bytes");
    m.cPagefile = m.sys.Add(L"\\Paging File(_Total)\\% Usage");
    m.cThermal = m.sys.Add(L"\\Thermal Zone Information(*)\\High Precision Temperature");
    m.sys.Collect();

    uint64_t lastQpc = QpcNow();
    uint64_t seq = 0;
    bool first = true;
    while (!quit_) {
        DWORD wait = first ? 250 : intervalMs_.load(); // quick second sample so rates appear promptly
        if (paused_ && !refreshNow_) wait = INFINITE;
        WaitForSingleObject(wake_, wait);
        if (quit_) break;
        if (paused_ && !refreshNow_) continue;
        bool forced = refreshNow_.exchange(false);
        uint64_t now = QpcNow();
        double interval = QpcToSeconds(now - lastQpc);
        if (!forced && !first && interval * 1000.0 < intervalMs_ * 0.5) continue; // spurious wake (settings change)
        if (interval <= 0.001) interval = 0.001;
        lastQpc = now;
        first = false;

        uint32_t dem = demand_;
        uint64_t t0 = QpcNow();
        auto snap = std::make_shared<Snapshot>();
        snap->seq = ++seq;
        snap->time = NowFileTime();
        snap->interval = interval;

        m.sys.Collect();
        CollectSystem(*snap);
        CollectMemory(*snap);
        if (dem & demand::Disk) CollectDisks(*snap);
        CollectNetwork(*snap, interval);
        if (dem & (demand::GpuProcess | demand::GpuSystem | demand::GpuSensors)) CollectGpu(*snap, dem);
        else m.procs.gpu.clear();

        // Per-process network via ETW.
        if (wantNetTrace_ && IsElevated()) {
            if (!m.netTrace) {
                m.netTrace = std::make_unique<NetTrace>();
                m.netTrace->Start();
            }
        } else if (m.netTrace) {
            m.netTrace.reset();
        }
        netTraceActive_ = m.netTrace && m.netTrace->Active();
        m.procs.netValid = netTraceActive_;
        m.procs.net.clear();
        if (netTraceActive_) {
            std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> bytes;
            m.netTrace->Drain(bytes);
            for (auto& [pid, sr] : bytes) m.procs.net[pid] = {sr.first / interval, sr.second / interval};
        }
        snap->netPerProcess = netTraceActive_;

        m.procs.Collect(*snap, interval, dem, cpu_.logical ? cpu_.logical : 1);
        if (dem & demand::Services) CollectServices(*snap);
        snap->servicesByPid = m.services;

        // "Compressed" memory: working set of the Memory Compression process (Task Manager semantics).
        for (auto& p : snap->procs)
            if (p.meta && p.meta->name == L"Memory Compression") { snap->mem.compressed = p.workingSet; break; }

        snap->collectMs = QpcToSeconds(QpcNow() - t0) * 1000.0;
        {
            std::lock_guard<std::mutex> l(lock_);
            latest_ = snap;
        }
        PostMessageW(notify_, msg_, 0, 0);
    }
    m.netTrace.reset();
}

void Sampler::CollectSystem(Snapshot& s) {
    Impl& m = *impl_;
    auto& c = s.cpu;
    double v = 0;
    const size_t n = cpu_.logical ? cpu_.logical : 1;
    c.cores.assign(n, 0.f);
    c.coresKernel.assign(n, 0.f);
    c.coreMHz.assign(n, 0.f);

    // Instance names are "group,number" plus "_Total" and "group,_Total".
    auto parse = [](const std::wstring& name, int& group, int& num) {
        size_t comma = name.find(L',');
        if (comma == std::wstring::npos || name.find(L'_') != std::wstring::npos) return false;
        group = _wtoi(name.c_str());
        num = _wtoi(name.c_str() + comma + 1);
        return true;
    };
    auto fill = [&](int counter, std::vector<float>& dest, float& total, double scale, bool clamp) {
        if (!m.sys.Array(counter, m.items)) return;
        std::vector<std::pair<std::pair<int, int>, double>> v2;
        for (auto& it : m.items) {
            int g, k;
            if (it.name == L"_Total") total = static_cast<float>(clamp ? std::min(100.0, it.value) : it.value);
            else if (parse(it.name, g, k)) v2.push_back({{g, k}, it.value});
        }
        std::sort(v2.begin(), v2.end());
        for (size_t i = 0; i < v2.size() && i < dest.size(); ++i) {
            double x = v2[i].second * scale;
            dest[i] = static_cast<float>(clamp ? std::clamp(x, 0.0, 100.0) : x);
        }
    };
    float dummy = 0;
    fill(m.cUtil, c.cores, c.total, 1.0, true);
    fill(m.cPriv, c.coresKernel, c.kernel, 1.0, true);
    float perfTotal = 0;
    fill(m.cPerf, c.coreMHz, perfTotal, cpu_.baseMHz / 100.0, false);
    (void)dummy;
    c.performancePct = perfTotal;
    c.currentMHz = perfTotal > 0 ? perfTotal * cpu_.baseMHz / 100.0 : 0;
    if (m.sys.Value(m.cLimit, v)) c.performanceLimitPct = static_cast<float>(v);
    if (m.sys.Value(m.cDpc, v)) c.dpcPct = static_cast<float>(v);
    if (m.sys.Value(m.cIntr, v)) c.interruptPct = static_cast<float>(v);
    if (m.sys.Value(m.cIntrPerSec, v)) c.interruptsPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cQueue, v)) c.queueLength = static_cast<float>(v);
    if (m.sys.Value(m.cCtx, v)) c.contextSwitchesPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cSyscalls, v)) c.systemCallsPerSec = static_cast<float>(v);
    c.uptimeSec = GetTickCount64() / 1000ULL;

    if (demand_ & demand::Thermal) {
        if (m.sys.Array(m.cThermal, m.items)) {
            for (auto& it : m.items) {
                double cdeg = it.value / 10.0 - 273.15; // tenths of Kelvin
                if (cdeg > -50 && cdeg < 150) s.thermalZonesC.emplace_back(it.name, cdeg);
            }
        }
    }
}

void Sampler::CollectMemory(Snapshot& s) {
    Impl& m = *impl_;
    auto& mm = s.mem;
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        mm.total = ms.ullTotalPhys;
        mm.available = ms.ullAvailPhys;
    }
    PERFORMANCE_INFORMATION pi{sizeof pi};
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        uint64_t pg = pi.PageSize;
        mm.commit = pi.CommitTotal * pg;
        mm.commitLimit = pi.CommitLimit * pg;
        mm.commitPeak = pi.CommitPeak * pg;
        mm.systemCache = pi.SystemCache * pg;
        mm.pagedPool = pi.KernelPaged * pg;
        mm.nonPagedPool = pi.KernelNonpaged * pg;
    }
    static uint64_t installed = [] { ULONGLONG kb = 0; return GetPhysicallyInstalledSystemMemory(&kb) ? kb * 1024ULL : 0ULL; }();
    mm.installed = installed;
    mm.hardwareReserved = installed > mm.total ? installed - mm.total : 0;
    double v;
    auto get = [&](int c) -> uint64_t { return m.sys.Value(c, v, PDH_FMT_DOUBLE) ? static_cast<uint64_t>(v) : 0; };
    mm.standby = get(m.cStandbyCore) + get(m.cStandbyNormal) + get(m.cStandbyReserve);
    mm.modified = get(m.cModified);
    mm.free = get(m.cFreeZero);
    mm.cached = mm.standby + mm.modified;
    if (uint64_t pp = get(m.cPoolPaged)) mm.pagedPool = pp;
    if (uint64_t np = get(m.cPoolNp)) mm.nonPagedPool = np;
    if (m.sys.Value(m.cPagesSec, v)) mm.pagesPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cFaultsSec, v)) mm.pageFaultsPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cPageReads, v)) mm.pageReadsPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cPageWrites, v)) mm.pageWritesPerSec = static_cast<float>(v);
    if (m.sys.Value(m.cPagefile, v)) mm.pagefileUsagePct = static_cast<float>(v);
}

void Sampler::CollectDisks(Snapshot& s) {
    Impl& m = *impl_;
    if (!m.disk) {
        m.disk = std::make_unique<PdhQuery>();
        m.dIdle = m.disk->Add(L"\\PhysicalDisk(*)\\% Idle Time");
        m.dLat = m.disk->Add(L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer");
        m.dLatR = m.disk->Add(L"\\PhysicalDisk(*)\\Avg. Disk sec/Read");
        m.dLatW = m.disk->Add(L"\\PhysicalDisk(*)\\Avg. Disk sec/Write");
        m.dRead = m.disk->Add(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
        m.dWrite = m.disk->Add(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
        m.dQueue = m.disk->Add(L"\\PhysicalDisk(*)\\Current Disk Queue Length");
        m.dReads = m.disk->Add(L"\\PhysicalDisk(*)\\Disk Reads/sec");
        m.dWrites = m.disk->Add(L"\\PhysicalDisk(*)\\Disk Writes/sec");
        m.disk->Collect();
        return; // rate counters need two samples
    }
    if (!m.disk->Collect()) return;
    s.diskCollected = true;
    std::map<std::wstring, DiskSnap> map;
    auto apply = [&](int c, auto fn) {
        if (!m.disk->Array(c, m.items)) return;
        for (auto& it : m.items) {
            if (it.name == L"_Total") continue;
            auto& d = map[it.name];
            fn(d, it.value);
        }
    };
    apply(m.dIdle, [](DiskSnap& d, double x) { d.activePct = static_cast<float>(std::clamp(100.0 - x, 0.0, 100.0)); });
    apply(m.dLat, [](DiskSnap& d, double x) { d.latencyMs = static_cast<float>(x * 1000.0); });
    apply(m.dLatR, [](DiskSnap& d, double x) { d.readLatencyMs = static_cast<float>(x * 1000.0); });
    apply(m.dLatW, [](DiskSnap& d, double x) { d.writeLatencyMs = static_cast<float>(x * 1000.0); });
    apply(m.dRead, [](DiskSnap& d, double x) { d.readBps = static_cast<float>(x); });
    apply(m.dWrite, [](DiskSnap& d, double x) { d.writeBps = static_cast<float>(x); });
    apply(m.dQueue, [](DiskSnap& d, double x) { d.queue = static_cast<float>(x); });
    apply(m.dReads, [](DiskSnap& d, double x) { d.readsPerSec = static_cast<float>(x); });
    apply(m.dWrites, [](DiskSnap& d, double x) { d.writesPerSec = static_cast<float>(x); });
    for (auto& [name, d] : map) {
        d.instance = name;
        d.index = _wtoi(name.c_str());
        size_t sp = name.find(L' ');
        d.letters = sp == std::wstring::npos ? L"" : name.substr(sp + 1);
        s.disks.push_back(d);
    }
    std::sort(s.disks.begin(), s.disks.end(), [](const DiskSnap& a, const DiskSnap& b) { return a.index < b.index; });
}

void Sampler::CollectNetwork(Snapshot& s, double interval) {
    Impl& m = *impl_;
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    for (ULONG i = 0; i < t->NumEntries; ++i) {
        const MIB_IF_ROW2& r = t->Table[i];
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
        if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        // Hide disconnected Bluetooth PAN and similar adapters that Task Manager also hides.
        if (r.MediaConnectState != MediaConnectStateConnected && r.Type != IF_TYPE_ETHERNET_CSMACD && r.Type != IF_TYPE_IEEE80211) continue;
        NicSnap n;
        n.luid = r.InterfaceLuid.Value;
        n.ifIndex = r.InterfaceIndex;
        n.alias = r.Alias;
        n.description = r.Description;
        n.ifType = r.Type;
        n.wireless = r.Type == IF_TYPE_IEEE80211;
        n.connected = r.MediaConnectState == MediaConnectStateConnected && r.OperStatus == IfOperStatusUp;
        n.speedTx = r.TransmitLinkSpeed;
        n.speedRx = r.ReceiveLinkSpeed;
        n.inOctets = r.InOctets;
        n.outOctets = r.OutOctets;
        n.inPackets = r.InUcastPkts + r.InNUcastPkts;
        n.outPackets = r.OutUcastPkts + r.OutNUcastPkts;
        n.inErrors = r.InErrors;
        n.outErrors = r.OutErrors;
        n.inDiscards = r.InDiscards;
        n.outDiscards = r.OutDiscards;
        auto it = m.nicPrev.find(n.luid);
        if (it != m.nicPrev.end() && interval > 0) {
            if (n.inOctets >= it->second.in) n.recvBps = (n.inOctets - it->second.in) / interval;
            if (n.outOctets >= it->second.out) n.sendBps = (n.outOctets - it->second.out) / interval;
            if (n.inPackets >= it->second.inP) n.recvPps = static_cast<float>((n.inPackets - it->second.inP) / interval);
            if (n.outPackets >= it->second.outP) n.sendPps = static_cast<float>((n.outPackets - it->second.outP) / interval);
        }
        m.nicPrev[n.luid] = {n.inOctets, n.outOctets, n.inPackets, n.outPackets};
        s.nics.push_back(std::move(n));
    }
    FreeMibTable(t);
}

namespace {
GpuEngineType EngineTypeFrom(const std::wstring& s) {
    if (s.rfind(L"3D", 0) == 0 || s.rfind(L"Graphics", 0) == 0) return GpuEngineType::ThreeD;
    if (s.rfind(L"Copy", 0) == 0) return GpuEngineType::Copy;
    if (s.rfind(L"VideoDecode", 0) == 0) return GpuEngineType::VideoDecode;
    if (s.rfind(L"VideoEncode", 0) == 0) return GpuEngineType::VideoEncode;
    if (s.rfind(L"VideoProcessing", 0) == 0) return GpuEngineType::VideoProcessing;
    if (s.rfind(L"Compute", 0) == 0 || s.rfind(L"Cuda", 0) == 0) return GpuEngineType::Compute;
    return GpuEngineType::Other;
}
uint64_t ParseLuid(const std::wstring& name) {
    size_t p = name.find(L"luid_0x");
    if (p == std::wstring::npos) return 0;
    uint64_t hi = wcstoull(name.c_str() + p + 7, nullptr, 16);
    size_t q = name.find(L"_0x", p + 7);
    if (q == std::wstring::npos) return 0;
    uint64_t lo = wcstoull(name.c_str() + q + 3, nullptr, 16);
    return (hi << 32) | (lo & 0xFFFFFFFF);
}
uint32_t ParsePid(const std::wstring& name) {
    return name.rfind(L"pid_", 0) == 0 ? static_cast<uint32_t>(wcstoul(name.c_str() + 4, nullptr, 10)) : 0;
}
} // namespace

void Sampler::CollectGpu(Snapshot& s, uint32_t dem) {
    Impl& m = *impl_;
    if (!m.gpuStaticLoaded) {
        m.gpuStatic = QueryGpus();
        m.gpuStaticLoaded = true;
    }
    if (!m.gpu) {
        m.gpu = std::make_unique<PdhQuery>();
        m.gEngine = m.gpu->Add(L"\\GPU Engine(*)\\Utilization Percentage");
        m.gProcDed = m.gpu->Add(L"\\GPU Process Memory(*)\\Dedicated Usage");
        m.gProcShared = m.gpu->Add(L"\\GPU Process Memory(*)\\Shared Usage");
        m.gAdDed = m.gpu->Add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
        m.gAdShared = m.gpu->Add(L"\\GPU Adapter Memory(*)\\Shared Usage");
        m.gpu->Collect();
        return;
    }
    if (!m.gpu->Collect()) return;
    s.gpuCollected = true;

    for (size_t i = 0; i < m.gpuStatic.size(); ++i) {
        GpuSnap g;
        g.luid = m.gpuStatic[i].luid;
        g.name = m.gpuStatic[i].name;
        g.dedicatedTotal = m.gpuStatic[i].dedicatedVideo;
        g.sharedTotal = m.gpuStatic[i].sharedSystem;
        s.gpus.push_back(std::move(g));
    }
    auto adapterIndex = [&](uint64_t luid) -> int {
        for (size_t i = 0; i < s.gpus.size(); ++i)
            if (s.gpus[i].luid == luid) return static_cast<int>(i);
        return -1;
    };

    // Engine utilization: sum per (adapter, engine) across processes; per process take the busiest engine.
    m.procs.gpu.clear();
    struct EngKey { int adapter; uint32_t eng; };
    std::map<std::pair<int, uint32_t>, std::pair<GpuEngineType, double>> engines;
    std::map<std::pair<uint32_t, std::pair<int, uint32_t>>, double> procEng;
    if (m.gpu->Array(m.gEngine, m.items)) {
        for (auto& it : m.items) {
            uint64_t luid = ParseLuid(it.name);
            int ai = adapterIndex(luid);
            if (ai < 0) continue;
            size_t e = it.name.find(L"_eng_");
            size_t t = it.name.find(L"_engtype_");
            if (e == std::wstring::npos || t == std::wstring::npos) continue;
            uint32_t eng = static_cast<uint32_t>(wcstoul(it.name.c_str() + e + 5, nullptr, 10));
            std::wstring typeName = it.name.substr(t + 9);
            auto key = std::make_pair(ai, eng);
            auto& en = engines[key];
            en.first = EngineTypeFrom(typeName);
            en.second += it.value;
            if (en.second > 100) en.second = 100;
            if (s.gpus[ai].engines.size() <= eng) s.gpus[ai].engines.resize(eng + 1);
            s.gpus[ai].engines[eng].name = typeName;
            s.gpus[ai].engines[eng].type = en.first;
            s.gpus[ai].engines[eng].index = eng;
            uint32_t pid = ParsePid(it.name);
            if (pid) procEng[{pid, key}] += it.value;
        }
    }
    for (auto& [key, en] : engines) {
        auto& g = s.gpus[key.first];
        g.engines[key.second].util = static_cast<float>(en.second);
        g.util = std::max(g.util, static_cast<float>(en.second));
    }
    // Remove engine slots that never reported (sparse ordinals).
    for (auto& g : s.gpus)
        g.engines.erase(std::remove_if(g.engines.begin(), g.engines.end(), [](const GpuEngineUtil& e) { return e.name.empty(); }), g.engines.end());

    if (dem & demand::GpuProcess) {
        for (auto& [k, util] : procEng) {
            auto& u = m.procs.gpu[k.first];
            if (util > u.util) {
                u.util = static_cast<float>(std::min(100.0, util));
                u.adapter = static_cast<uint8_t>(k.second.first);
                u.engine = engines[k.second].first;
            }
        }
        if (m.gpu->LargeArray(m.gProcDed, m.large)) {
            for (auto& [name, val] : m.large)
                if (uint32_t pid = ParsePid(name)) m.procs.gpu[pid].dedicated += static_cast<uint64_t>(val);
        }
        if (m.gpu->LargeArray(m.gProcShared, m.large)) {
            for (auto& [name, val] : m.large)
                if (uint32_t pid = ParsePid(name)) m.procs.gpu[pid].shared += static_cast<uint64_t>(val);
        }
    }
    if (m.gpu->LargeArray(m.gAdDed, m.large)) {
        for (auto& [name, val] : m.large) {
            int ai = adapterIndex(ParseLuid(name));
            if (ai >= 0) s.gpus[ai].dedicatedUsed += static_cast<uint64_t>(val);
        }
    }
    if (m.gpu->LargeArray(m.gAdShared, m.large)) {
        for (auto& [name, val] : m.large) {
            int ai = adapterIndex(ParseLuid(name));
            if (ai >= 0) s.gpus[ai].sharedUsed += static_cast<uint64_t>(val);
        }
    }

    // Driver telemetry (temperature, fan, clocks). Queried every 2nd sample to limit kernel calls.
    if (dem & demand::GpuSensors) {
        bool refresh = (m.gpuSensorTick++ % 2) == 0 || m.lastSensors.size() != s.gpus.size();
        if (refresh) m.lastSensors = s.gpus;
        for (size_t i = 0; i < s.gpus.size(); ++i) {
            auto& g = s.gpus[i];
            auto& c = m.lastSensors[i];
            if (refresh)
                QueryGpuSensors(g.luid, c.temperatureC, c.fanRpm, c.powerPct, c.memoryFreqMHz, c.coreFreqMHz, c.coreMaxFreqMHz, c.coreVoltage,
                                c.memBandwidthBps, c.pcieBandwidthBps);
            g.temperatureC = c.temperatureC; g.fanRpm = c.fanRpm; g.powerPct = c.powerPct; g.memoryFreqMHz = c.memoryFreqMHz;
            g.coreFreqMHz = c.coreFreqMHz; g.coreMaxFreqMHz = c.coreMaxFreqMHz; g.coreVoltage = c.coreVoltage;
        }
    }
}

void Sampler::CollectServices(Snapshot& /*s*/) {
    Impl& m = *impl_;
    uint64_t now = GetTickCount64();
    if (m.services && now - m.servicesTick < 10000) return;
    m.servicesTick = now;
    auto map = std::make_shared<std::unordered_map<uint32_t, std::wstring>>();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm) {
        DWORD needed = 0, count = 0, resume = 0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, nullptr, 0, &needed, &count, &resume, nullptr);
        std::vector<uint8_t> buf(needed + 4096);
        resume = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buf.data(), static_cast<DWORD>(buf.size()), &needed, &count,
                                  &resume, nullptr)) {
            auto* e = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
            for (DWORD i = 0; i < count; ++i) {
                DWORD pid = e[i].ServiceStatusProcess.dwProcessId;
                if (!pid) continue;
                auto& str = (*map)[pid];
                if (!str.empty()) str += L", ";
                str += e[i].lpDisplayName;
            }
        }
        CloseServiceHandle(scm);
    }
    m.services = map;
}

} // namespace nx
