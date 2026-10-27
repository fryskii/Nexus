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
    int whea30d = -1;                 // WHEA-Logger events, -1 unknown
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
