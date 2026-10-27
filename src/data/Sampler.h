#pragma once
// Background collector thread. Produces immutable Snapshots at the configured interval and
// notifies the UI with a posted message. Collection never blocks the UI thread.
#include "data/Hardware.h"
#include "data/Model.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace nx {

class NetTrace;

class Sampler {
public:
    Sampler(HWND notifyWindow, UINT notifyMessage);
    ~Sampler();

    void Start();
    void Stop();

    void SetIntervalMs(uint32_t ms) { intervalMs_ = ms < 100 ? 100 : ms; Wake(); }
    uint32_t IntervalMs() const { return intervalMs_; }
    void SetPaused(bool p) { paused_ = p; Wake(); }
    bool Paused() const { return paused_; }
    void RefreshNow() { refreshNow_ = true; Wake(); }
    void SetDemand(uint32_t flags) { demand_ = flags; }
    uint32_t Demand() const { return demand_; }
    // Per-process network accounting via ETW (requires administrator rights).
    void SetNetTraceEnabled(bool on) { wantNetTrace_ = on; }
    bool NetTraceActive() const { return netTraceActive_; }
    std::wstring NetTraceStatus() const;

    SnapshotPtr Latest() const;
    const CpuStatic& Cpu() const { return cpu_; }

private:
    void Run();
    void Wake();
    void CollectSystem(Snapshot& s);
    void CollectMemory(Snapshot& s);
    void CollectDisks(Snapshot& s);
    void CollectNetwork(Snapshot& s, double interval);
    void CollectGpu(Snapshot& s, uint32_t demand);
    void CollectServices(Snapshot& s);

    HWND notify_;
    UINT msg_;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> quit_{false}, paused_{false}, refreshNow_{false}, wantNetTrace_{false}, netTraceActive_{false};
    std::atomic<uint32_t> intervalMs_{1000};
    std::atomic<uint32_t> demand_{demand::All};
    mutable std::mutex lock_;
    SnapshotPtr latest_;
    CpuStatic cpu_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nx
