#pragma once
// Evidence-based diagnostic rules. Inputs are plain metric series collected while NEXUS runs;
// output alerts always carry the measured evidence and the reasoning used, and never claim a
// hardware fault is confirmed - only that a symptom was observed.
#include <cstdint>
#include <string>
#include <vector>

namespace nx {

struct DiagInput {
    double intervalSec = 1.0;   // seconds between samples in every series below
    unsigned logicalCpus = 1;

    struct Proc {
        uint32_t pid = 0;
        std::wstring name;
        std::vector<float> cpu;       // percent of total machine
        std::vector<float> privateMB; // private bytes in MB
    };
    std::vector<Proc> procs;

    std::vector<float> totalCpu;      // percent
    std::vector<float> commitPct;     // commit charge / commit limit * 100
    std::vector<float> availPct;      // available physical / total physical * 100
    std::vector<float> hardFaults;    // Memory\Pages/sec (hard page faults resolved from disk)

    struct Disk { std::wstring name; std::vector<float> latencyMs; std::vector<float> activePct; };
    std::vector<Disk> disks;

    struct Adapter { std::wstring name; uint64_t errors = 0; uint64_t discards = 0; uint64_t packets = 0; };
    std::vector<Adapter> adapters;   // deltas since monitoring started

    struct Volume { std::wstring name; uint64_t freeBytes = 0, totalBytes = 0; };
    std::vector<Volume> volumes;

    struct Health { std::wstring device; uint8_t criticalWarning = 0; int percentUsed = -1; int availableSpare = -1; int spareThreshold = -1; double tempC = -1; };
    std::vector<Health> storageHealth;

    double maxThermalZoneC = -1;     // ACPI thermal zone (not necessarily die temperature)
    std::vector<double> gpuTempsC;
    int appCrashes24h = -1;           // Application Error (1000) / hangs (1002), -1 unknown
    int unexpectedShutdowns30d = -1;  // Kernel-Power 41, -1 unknown
    std::vector<std::wstring> unexpectedShutdownTimes; // local times of the most recent ones, newest first
    int whea30d = -1;                 // WHEA-Logger events, -1 unknown
};

// The event-log counts above, from one newest-first pass over each log instead of a query per count.
// Each query also matches every event older than its longest window, so a pass can stop there rather
// than read the rest of the log: the Event Log service reads only as far as it is asked.
struct EventLogTally {
    static constexpr const wchar_t* kSystemQuery =
        L"*[System[(Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41) or Provider[@Name='Microsoft-Windows-WHEA-Logger'] or "
        L"((Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 86400000]) or TimeCreated[timediff(@SystemTime) > 2592000000]]]";
    static constexpr const wchar_t* kApplicationQuery = L"*[System[EventID=1000 or EventID=1002 or TimeCreated[timediff(@SystemTime) > 86400000]]]";

    int crashes24h = 0;     // Application Error (1000) and Application Hang (1002), last 24 hours
    int kernelPower30d = 0; // Kernel-Power 41, last 30 days
    int whea30d = 0;        // WHEA-Logger, last 30 days
    int errors24h = 0;      // System log critical and error events, last 24 hours
    std::vector<uint64_t> kernelPowerTimes; // FILETIME (UTC) of the newest Kernel-Power 41 events, at most 5

    // Feed each log newest first, times as FILETIME (UTC). False once the pass can stop: after a run of
    // events older than the window rather than the first one, as a clock that was wrong at boot can misdate a few.
    bool AddSystem(const std::wstring& provider, uint32_t id, uint8_t level, uint64_t time, uint64_t now);
    bool AddApplication(uint32_t id, uint64_t time, uint64_t now);

private:
    int systemOlder_ = 0, applicationOlder_ = 0; // events in a row older than the window
};

struct DiagAlert {
    enum Severity { Info = 0, Warning = 1, Critical = 2 } severity = Info;
    std::wstring category;   // CPU, Memory, Disk, Network, Storage, Thermal, Stability
    std::wstring title;
    std::vector<std::wstring> evidence;
    std::wstring reasoning;
    std::wstring suggestion;
    uint32_t pid = 0;
};

std::vector<DiagAlert> EvaluateDiagnostics(const DiagInput& in);

// Statistics helpers (exposed for tests).
double Mean(const std::vector<float>& v);
double FractionAtLeast(const std::vector<float>& v, double threshold);
// Least-squares slope (units per sample) and R^2.
void LinearFit(const std::vector<float>& v, double& slope, double& r2);

} // namespace nx
