#pragma once
#include "data/Model.h"

#include <unordered_map>

namespace nx {

// Acquires immutable metadata for a process. `h` may be null (access denied).
std::shared_ptr<ProcMeta> AcquireProcMeta(uint32_t pid, const std::wstring& name, uint32_t session, HANDLE h);
const wchar_t* IntegrityName(int level);
const wchar_t* PriorityClassName(uint32_t cls);
const wchar_t* MachineName(uint16_t machine);

// Enumerates processes with one NtQuerySystemInformation call (the same mechanism Task Manager
// uses), falling back to Toolhelp32 + per-process queries if that call is unavailable.
class ProcessCollector {
public:
    ProcessCollector();
    ~ProcessCollector();
    void Collect(Snapshot& snap, double intervalSec, uint32_t demandFlags, unsigned logicalCpus);

    // Per-process GPU/network inputs merged into the snapshot by the sampler.
    struct GpuUsage { float util = 0; GpuEngineType engine = GpuEngineType::None; uint8_t adapter = 0; uint64_t dedicated = 0, shared = 0; };
    std::unordered_map<uint32_t, GpuUsage> gpu;
    struct NetUsage { double send = 0, recv = 0; };
    std::unordered_map<uint32_t, NetUsage> net;
    bool netValid = false;

private:
    struct Prev {
        uint64_t createTime = 0;
        uint64_t cpuTime = 0, kernelTime = 0, ioR = 0, ioW = 0, ioO = 0, ws = 0;
        uint32_t pageFaults = 0;
        std::shared_ptr<ProcMeta> meta;
        HANDLE handle = nullptr;
        uint32_t priorityClass = 0, gdi = 0, user = 0;
        bool efficiency = false;
        uint32_t extraAge = 1000;
        float powerAvg = 0;
        bool seen = false;
    };
    bool CollectNt(Snapshot& snap, double interval, uint32_t demandFlags, unsigned cpus);
    bool CollectToolhelp(Snapshot& snap, double interval, uint32_t demandFlags, unsigned cpus);
    void Finish(Snapshot& snap, uint32_t demandFlags);
    void ClassifyWindows(Snapshot& snap);

    std::vector<uint8_t> buf_;
    std::unordered_map<uint32_t, Prev> prev_;
    bool first_ = true;
    uint64_t pass_ = 0;
};

} // namespace nx
