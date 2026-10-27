#pragma once
// Snapshot data model shared between the collector thread and the UI thread.
// A Snapshot is immutable once published; the UI only ever holds shared_ptr<const Snapshot>.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nx {

enum class ProcCategory : uint8_t { App, Background, Windows };
enum class ProcStatus : uint8_t { Running, Suspended, NotResponding, Terminated };

// Per-process metadata that does not change for the lifetime of a process instance
// (identified by PID + creation time). Acquired once, shared across snapshots.
struct ProcMeta {
    std::wstring name;          // image name as reported by the kernel
    std::wstring path;          // full Win32 path ("" if inaccessible)
    std::wstring commandLine;
    std::wstring user;          // DOMAIN\User
    std::wstring userShort;
    std::wstring description;   // FileDescription
    std::wstring company;       // CompanyName (Task Manager "Publisher" column)
    std::wstring fileVersion;
    std::wstring packageName;   // packaged (UWP/MSIX) apps
    uint16_t machine = 0;       // IMAGE_FILE_MACHINE_* of the process
    int8_t integrity = -1;      // 0 untrusted,1 low,2 medium,3 high,4 system,5 protected; -1 unknown
    bool elevated = false;
    bool uacVirtualization = false;
    bool accessDenied = false;  // could not open even with PROCESS_QUERY_LIMITED_INFORMATION
    bool isProtected = false;   // protected process / PPL
    bool isCritical = false;    // IsProcessCritical (terminating would bugcheck)
    bool inWindowsDir = false;
    bool isService = false;     // running in session 0 under a service account
    std::wstring DisplayName() const { return description.empty() ? name : description; }
};

enum class GpuEngineType : uint8_t { None, ThreeD, Copy, VideoDecode, VideoEncode, VideoProcessing, Compute, Other };
const wchar_t* GpuEngineTypeName(GpuEngineType t);

struct ProcSample {
    uint32_t pid = 0, ppid = 0, session = 0;
    uint32_t threads = 0, handles = 0, gdiObjects = 0, userObjects = 0;
    int32_t basePriority = 0;
    uint32_t priorityClass = 0;   // 0 = unknown
    uint64_t createTime = 0;      // FILETIME (UTC)
    uint64_t cpuTime = 0, kernelTime = 0, cycles = 0;
    uint64_t workingSet = 0, privateWorkingSet = 0, privateBytes = 0, peakWorkingSet = 0;
    uint64_t pagedPool = 0, nonPagedPool = 0, virtualSize = 0;
    uint32_t pageFaults = 0, pageFaultDelta = 0, hardFaults = 0;
    int64_t workingSetDelta = 0;
    uint64_t ioReadBytes = 0, ioWriteBytes = 0, ioOtherBytes = 0, ioReadOps = 0, ioWriteOps = 0, ioOtherOps = 0;
    float cpu = 0, cpuKernel = 0;                      // percent of the whole machine
    float ioReadRate = 0, ioWriteRate = 0, ioOtherRate = 0; // bytes/s
    float gpu = 0;                                     // max engine utilization percent
    GpuEngineType gpuEngine = GpuEngineType::None;
    uint8_t gpuAdapter = 0;
    uint64_t gpuDedicated = 0, gpuShared = 0;
    float netSend = -1, netRecv = -1;                  // bytes/s, -1 = not measured (needs ETW)
    float power = 0;                                   // heuristic power score (see docs/LIMITATIONS.md)
    float powerTrend = 0;                              // ~2 minute exponential average of power
    ProcStatus status = ProcStatus::Running;
    ProcCategory category = ProcCategory::Background;
    bool efficiencyMode = false;
    bool isNew = false;
    bool wow64 = false;
    HWND mainWindow = nullptr;
    std::shared_ptr<const std::wstring> windowTitle;   // apps only
    std::shared_ptr<const ProcMeta> meta;
    uint64_t Key() const { return (static_cast<uint64_t>(pid) << 32) ^ (createTime & 0xFFFFFFFFull) ^ (createTime >> 32); }
    const std::wstring& Name() const;
};

struct CpuSnap {
    float total = 0, kernel = 0;       // % Processor Utility (frequency-aware, like Task Manager)
    float performancePct = 0;          // % Processor Performance (relative to base frequency)
    float performanceLimitPct = 100;   // < 100 => limited by power/thermal policy
    double currentMHz = 0;
    std::vector<float> cores;          // per logical processor utilization
    std::vector<float> coresKernel;
    std::vector<float> coreMHz;        // per logical processor effective frequency
    uint32_t processes = 0, threads = 0, handles = 0;
    uint64_t uptimeSec = 0;
    float queueLength = 0, contextSwitchesPerSec = 0, systemCallsPerSec = 0, interruptsPerSec = 0, dpcPct = 0, interruptPct = 0;
};

struct MemSnap {
    uint64_t total = 0, available = 0, installed = 0, hardwareReserved = 0;
    uint64_t commit = 0, commitLimit = 0, commitPeak = 0;
    uint64_t standby = 0, modified = 0, free = 0, cached = 0, compressed = 0;
    uint64_t pagedPool = 0, nonPagedPool = 0, systemCache = 0;
    float pagesPerSec = 0, pageFaultsPerSec = 0, pageReadsPerSec = 0, pageWritesPerSec = 0, pagefileUsagePct = 0;
    uint64_t InUse() const { return total > available ? total - available : 0; }
};

struct DiskSnap {
    int index = -1;             // physical drive number
    std::wstring instance;      // PDH instance e.g. "0 C: D:"
    std::wstring letters;       // "C: D:"
    float activePct = 0, readBps = 0, writeBps = 0, latencyMs = 0, readLatencyMs = 0, writeLatencyMs = 0, queue = 0;
    float readsPerSec = 0, writesPerSec = 0;
};

struct NicSnap {
    uint64_t luid = 0;
    uint32_t ifIndex = 0;
    std::wstring alias, description;
    uint32_t ifType = 0;
    bool wireless = false, connected = false;
    uint64_t speedTx = 0, speedRx = 0;           // bits/s
    double sendBps = 0, recvBps = 0;             // bytes/s
    uint64_t inOctets = 0, outOctets = 0, inPackets = 0, outPackets = 0;
    uint64_t inErrors = 0, outErrors = 0, inDiscards = 0, outDiscards = 0;
    float sendPps = 0, recvPps = 0;
};

struct GpuEngineUtil { std::wstring name; GpuEngineType type = GpuEngineType::Other; uint32_t index = 0; float util = 0; };

struct GpuSnap {
    uint64_t luid = 0;
    std::wstring name;
    float util = 0;
    std::vector<GpuEngineUtil> engines;
    uint64_t dedicatedUsed = 0, dedicatedTotal = 0, sharedUsed = 0, sharedTotal = 0;
    // Driver-reported telemetry (D3DKMT adapter perf data, WDDM 2.4+); negative = unavailable.
    double temperatureC = -1, fanRpm = -1, powerPct = -1, memoryFreqMHz = -1, coreFreqMHz = -1, coreMaxFreqMHz = -1, coreVoltage = -1;
    double memBandwidthBps = -1, pcieBandwidthBps = -1;
};

struct Snapshot {
    uint64_t seq = 0;
    uint64_t time = 0;               // FILETIME UTC
    double interval = 1.0;           // seconds since previous sample
    CpuSnap cpu;
    MemSnap mem;
    std::vector<DiskSnap> disks;
    std::vector<NicSnap> nics;
    std::vector<GpuSnap> gpus;
    std::vector<ProcSample> procs;   // sorted by PID
    std::vector<uint32_t> terminated; // PIDs that disappeared since the previous snapshot
    std::vector<std::pair<std::wstring, double>> thermalZonesC;
    std::shared_ptr<const std::unordered_map<uint32_t, std::wstring>> servicesByPid;
    bool gpuCollected = false, netPerProcess = false, diskCollected = false;
    double collectMs = 0;            // time taken to collect this snapshot

    const ProcSample* FindProc(uint32_t pid) const;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// What the UI currently needs. Expensive collectors are skipped unless demanded.
namespace demand {
constexpr uint32_t GpuProcess = 1u << 0;  // per-process GPU engine counters
constexpr uint32_t GpuSystem = 1u << 1;   // adapter utilization + memory
constexpr uint32_t GpuSensors = 1u << 2;  // D3DKMT temperature/fan/power
constexpr uint32_t Disk = 1u << 3;
constexpr uint32_t PerCore = 1u << 4;
constexpr uint32_t AppWindows = 1u << 5;  // classify apps / window titles / hung detection
constexpr uint32_t Thermal = 1u << 6;
constexpr uint32_t Services = 1u << 7;
constexpr uint32_t ProcessExtra = 1u << 8; // GDI/USER objects, priority, efficiency mode
constexpr uint32_t Network = 1u << 9;
constexpr uint32_t All = 0xFFFFFFFFu;
} // namespace demand

} // namespace nx
