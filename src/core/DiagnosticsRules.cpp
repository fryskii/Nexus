#include "core/DiagnosticsRules.h"
#include "core/Format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nx {

double Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (float x : v) s += x;
    return s / static_cast<double>(v.size());
}

double FractionAtLeast(const std::vector<float>& v, double threshold) {
    if (v.empty()) return 0;
    size_t n = 0;
    for (float x : v) n += x >= threshold ? 1 : 0;
    return static_cast<double>(n) / static_cast<double>(v.size());
}

void LinearFit(const std::vector<float>& v, double& slope, double& r2) {
    slope = 0;
    r2 = 0;
    size_t n = v.size();
    if (n < 3) return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(i), y = v[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y;
    }
    double dn = static_cast<double>(n);
    double den = dn * sxx - sx * sx;
    if (den == 0) return;
    slope = (dn * sxy - sx * sy) / den;
    double vy = dn * syy - sy * sy;
    if (vy <= 0) { r2 = 0; return; }
    double r = (dn * sxy - sx * sy) / std::sqrt(den * vy);
    r2 = r * r;
}

namespace {
std::wstring F(const wchar_t* fmt, double a) {
    wchar_t b[256];
    std::swprintf(b, 256, fmt, a);
    return b;
}
std::wstring F2(const wchar_t* fmt, double a, double c) {
    wchar_t b[256];
    std::swprintf(b, 256, fmt, a, c);
    return b;
}
std::wstring Window(size_t samples, double interval) {
    double s = static_cast<double>(samples) * interval;
    if (s >= 120) return F(L"%.0f min", s / 60.0);
    return F(L"%.0f s", s);
}
float MinOf(const std::vector<float>& v) { return v.empty() ? 0.f : *std::min_element(v.begin(), v.end()); }
float MaxOf(const std::vector<float>& v) { return v.empty() ? 0.f : *std::max_element(v.begin(), v.end()); }
} // namespace

std::vector<DiagAlert> EvaluateDiagnostics(const DiagInput& in) {
    std::vector<DiagAlert> out;
    // Clamp so sample counts derived from the interval stay finite (NaN/Inf/0 fall back to 1 s).
    const double iv = std::isfinite(in.intervalSec) && in.intervalSec > 0 ? std::clamp(in.intervalSec, 0.05, 3600.0) : 1.0;
    const size_t minSustain = static_cast<size_t>(std::ceil(60.0 / iv)); // at least 60 s of evidence

    // --- System-wide CPU saturation ---
    if (in.totalCpu.size() >= static_cast<size_t>(std::ceil(30.0 / iv))) {
        double frac = FractionAtLeast(in.totalCpu, 90.0);
        if (frac >= 0.8) {
            DiagAlert a;
            a.severity = DiagAlert::Warning;
            a.category = L"CPU";
            a.title = L"Processor is saturated";
            a.evidence.push_back(F(L"Total CPU ≥ 90%% in %.0f%% of samples", frac * 100));
            a.evidence.push_back(F(L"Average %.0f%%", Mean(in.totalCpu)) + L" over " + Window(in.totalCpu.size(), iv));
            a.reasoning = L"Sustained near-100% utilization means runnable threads are queuing for CPU time, which shows up as sluggish input and longer task durations.";
            a.suggestion = L"Sort Processes by CPU to find the dominant consumers; see per-process alerts below.";
            out.push_back(std::move(a));
        }
    }

    // --- Per-process sustained CPU and memory growth ---
    const double singleCore = 100.0 / std::max(1u, in.logicalCpus);
    for (auto& p : in.procs) {
        if (p.pid == 0) continue; // System Idle Process
        if (p.cpu.size() >= minSustain) {
            // A process is flagged if it keeps at least one full logical core (or 25% of the machine) busy.
            double thr = std::min(25.0, std::max(singleCore * 0.9, 5.0));
            double frac = FractionAtLeast(p.cpu, thr);
            if (frac >= 0.8) {
                DiagAlert a;
                a.severity = Mean(p.cpu) >= 50.0 ? DiagAlert::Warning : DiagAlert::Info;
                a.category = L"CPU";
                a.pid = p.pid;
                a.title = p.name + L" is consistently using high CPU";
                a.evidence.push_back(F(L"≥ %.1f%% CPU", thr) + F(L" in %.0f%% of samples over ", frac * 100) + Window(p.cpu.size(), iv));
                a.evidence.push_back(F2(L"Average %.1f%%, peak %.1f%%", Mean(p.cpu), MaxOf(p.cpu)));
                a.evidence.push_back(F(L"PID %.0f", p.pid));
                a.reasoning = L"The process kept at least one logical processor busy for most of the observation window rather than in short bursts.";
                a.suggestion = L"If this is unexpected, inspect its threads in the process details window to see which code is running, or check for updates.";
                out.push_back(std::move(a));
            }
        }
        if (p.privateMB.size() >= static_cast<size_t>(std::ceil(300.0 / iv))) { // >= 5 minutes of data
            double slope, r2;
            LinearFit(p.privateMB, slope, r2);
            double growth = p.privateMB.back() - p.privateMB.front();
            double perMin = slope * (60.0 / iv);
            if (growth >= 100.0 && perMin >= 1.0 && r2 >= 0.85) {
                DiagAlert a;
                a.severity = growth >= 1024.0 ? DiagAlert::Warning : DiagAlert::Info;
                a.category = L"Memory";
                a.pid = p.pid;
                a.title = p.name + L" memory usage is steadily increasing";
                a.evidence.push_back(F2(L"Private bytes grew %.0f MB (%.0f MB → ", growth, p.privateMB.front()) +
                                     F(L"%.0f MB) over ", p.privateMB.back()) + Window(p.privateMB.size(), iv));
                a.evidence.push_back(F2(L"Trend %.1f MB/min, linear fit R² = %.2f", perMin, r2));
                a.reasoning = L"Growth is steady and nearly linear rather than fluctuating. This pattern is consistent with a memory leak, but caches and long-running workloads can grow legitimately - it is not proof of a defect.";
                a.suggestion = L"Keep observing; if growth continues without bound, restart the application and report it to the vendor.";
                out.push_back(std::move(a));
            }
        }
    }

    // --- Memory pressure / paging ---
    if (in.commitPct.size() >= minSustain / 2 && !in.commitPct.empty()) {
        double commitHigh = FractionAtLeast(in.commitPct, 90.0);
        std::vector<float> lowAvail;
        for (float a : in.availPct) lowAvail.push_back(a <= 10.0f ? 1.0f : 0.0f);
        double availLow = Mean(lowAvail);
        double faults = Mean(in.hardFaults);
        if (commitHigh >= 0.6 || (availLow >= 0.6 && faults >= 500)) {
            DiagAlert a;
            a.severity = (commitHigh >= 0.6 && MaxOf(in.commitPct) >= 97.0) || faults >= 2000 ? DiagAlert::Critical : DiagAlert::Warning;
            a.category = L"Memory";
            a.title = faults >= 500 ? L"Memory pressure is causing heavy paging" : L"Memory commit is near its limit";
            a.evidence.push_back(F2(L"Commit charge %.0f%% average, %.0f%% peak", Mean(in.commitPct), MaxOf(in.commitPct)));
            if (!in.availPct.empty()) a.evidence.push_back(F(L"Available physical memory as low as %.1f%%", MinOf(in.availPct)));
            if (!in.hardFaults.empty()) a.evidence.push_back(F(L"Hard page faults averaging %.0f pages/s", faults));
            a.reasoning = L"When commit approaches the limit, allocations can fail; when available memory is low and hard faults are high, Windows is reading pages back from disk, which slows the whole system.";
            a.suggestion = L"Close memory-heavy applications (sort Processes by Memory) or increase RAM / page file size.";
            out.push_back(std::move(a));
        }
    }

    // --- Disk latency ---
    for (auto& d : in.disks) {
        if (d.latencyMs.size() < minSustain / 2 || d.latencyMs.empty()) continue;
        // Only consider samples where the disk was actually busy; idle disks can report noisy averages.
        std::vector<float> busyLat;
        for (size_t i = 0; i < d.latencyMs.size() && i < d.activePct.size(); ++i)
            if (d.activePct[i] >= 20.0f) busyLat.push_back(d.latencyMs[i]);
        if (busyLat.size() < 10) continue;
        double avg = Mean(busyLat);
        double frac = FractionAtLeast(busyLat, 100.0);
        if (avg >= 50.0 || frac >= 0.25) {
            DiagAlert a;
            a.severity = avg >= 200.0 ? DiagAlert::Critical : DiagAlert::Warning;
            a.category = L"Disk";
            a.title = d.name + L" shows abnormally high response time";
            a.evidence.push_back(F(L"Average response time %.0f ms while busy", avg) + L" (" + std::to_wstring(busyLat.size()) + L" busy samples)");
            a.evidence.push_back(F(L"%.0f%% of busy samples ≥ 100 ms", frac * 100));
            a.evidence.push_back(F(L"Peak %.0f ms", MaxOf(busyLat)));
            a.reasoning = L"Healthy SSDs typically respond in under 10 ms and HDDs in under 30 ms under normal load. Sustained high latency indicates the device is overloaded or degraded; heavy queued I/O alone can also cause it.";
            a.suggestion = L"Check Storage › Health for SMART/NVMe status and identify the processes driving I/O.";
            out.push_back(std::move(a));
        }
    }

    // --- Network adapter errors ---
    for (auto& ad : in.adapters) {
        uint64_t bad = ad.errors + ad.discards;
        if (bad == 0) continue;
        double ratio = ad.packets ? static_cast<double>(bad) / static_cast<double>(ad.packets) : 1.0;
        if (bad >= 10 && ratio >= 0.001) {
            DiagAlert a;
            a.severity = ratio >= 0.01 ? DiagAlert::Warning : DiagAlert::Info;
            a.category = L"Network";
            a.title = ad.name + L" is reporting packet errors";
            a.evidence.push_back(std::to_wstring(ad.errors) + L" errors and " + std::to_wstring(ad.discards) + L" discards across " +
                                 std::to_wstring(ad.packets) + L" packets since monitoring started");
            a.evidence.push_back(F(L"Error rate %.3f%%", ratio * 100.0));
            a.reasoning = L"Interface error counters are reported by the network driver. A persistent non-zero error rate can indicate a cabling, signal or driver problem; discards alone may simply mean buffer overflow under load.";
            a.suggestion = L"For wired links try another cable/port; for Wi-Fi check signal strength; update the adapter driver.";
            out.push_back(std::move(a));
        }
    }

    // --- Low free space ---
    for (auto& v : in.volumes) {
        if (v.totalBytes == 0) continue;
        double pct = 100.0 * static_cast<double>(v.freeBytes) / static_cast<double>(v.totalBytes);
        if (pct < 10.0 || v.freeBytes < 5ULL * 1024 * 1024 * 1024) {
            DiagAlert a;
            a.severity = pct < 3.0 ? DiagAlert::Critical : DiagAlert::Warning;
            a.category = L"Storage";
            a.title = L"Low free space on " + v.name;
            a.evidence.push_back(FormatBytes(v.freeBytes) + L" free of " + FormatBytes(v.totalBytes) + F(L" (%.1f%%)", pct));
            a.reasoning = L"Windows needs free space for updates, the page file and temporary files; very low space can cause failures and slowdowns.";
            a.suggestion = L"Use Storage Sense or Disk Cleanup to free space.";
            out.push_back(std::move(a));
        }
    }

    // --- Storage health (NVMe SMART / health log) ---
    for (auto& h : in.storageHealth) {
        std::vector<std::wstring> ev;
        DiagAlert::Severity sev = DiagAlert::Info;
        if (h.criticalWarning) {
            sev = DiagAlert::Critical;
            if (h.criticalWarning & 0x01) ev.push_back(L"Available spare below threshold");
            if (h.criticalWarning & 0x02) ev.push_back(L"Temperature outside threshold");
            if (h.criticalWarning & 0x04) ev.push_back(L"Reliability degraded due to media errors");
            if (h.criticalWarning & 0x08) ev.push_back(L"Media placed in read-only mode");
            if (h.criticalWarning & 0x10) ev.push_back(L"Volatile memory backup failed");
        }
        if (h.percentUsed >= 90) {
            sev = std::max(sev, h.percentUsed >= 100 ? DiagAlert::Warning : DiagAlert::Info);
            ev.push_back(L"Drive reports " + std::to_wstring(h.percentUsed) + L"% of rated endurance used");
        }
        if (ev.empty()) continue;
        DiagAlert a;
        a.severity = sev;
        a.category = L"Storage";
        a.title = h.device + L" health warning";
        a.evidence = std::move(ev);
        a.reasoning = L"These flags are self-reported by the drive firmware through the NVMe health log. A critical-warning bit is the drive's own statement that a threshold was crossed.";
        a.suggestion = L"Back up important data and consult the drive vendor's diagnostic tool.";
        out.push_back(std::move(a));
    }

    // --- Thermal ---
    if (in.maxThermalZoneC >= 90.0) {
        DiagAlert a;
        a.severity = in.maxThermalZoneC >= 100.0 ? DiagAlert::Critical : DiagAlert::Warning;
        a.category = L"Thermal";
        a.title = L"High ACPI thermal zone temperature";
        a.evidence.push_back(F(L"Thermal zone reading %.0f °C", in.maxThermalZoneC));
        a.reasoning = L"ACPI thermal zones are firmware-defined sensors that may not correspond exactly to CPU die temperature, but readings this high typically precede thermal throttling.";
        a.suggestion = L"Check cooling: vents, fans, dust and ambient temperature.";
        out.push_back(std::move(a));
    }
    for (size_t i = 0; i < in.gpuTempsC.size(); ++i) {
        if (in.gpuTempsC[i] < 90.0) continue;
        DiagAlert a;
        a.severity = DiagAlert::Warning;
        a.category = L"Thermal";
        a.title = L"GPU " + std::to_wstring(i) + L" is running hot";
        a.evidence.push_back(F(L"Driver-reported temperature %.0f °C", in.gpuTempsC[i]));
        a.reasoning = L"Most GPUs begin reducing clocks between 83 and 95 °C.";
        a.suggestion = L"Improve airflow or check the GPU fan curve.";
        out.push_back(std::move(a));
    }

    // --- Stability (event log evidence) ---
    if (in.appCrashes24h >= 3) {
        DiagAlert a;
        a.severity = in.appCrashes24h >= 10 ? DiagAlert::Warning : DiagAlert::Info;
        a.category = L"Stability";
        a.title = L"Applications have crashed or hung recently";
        a.evidence.push_back(std::to_wstring(in.appCrashes24h) + L" Application Error / Application Hang events in the last 24 hours");
        a.reasoning = L"Repeated crash events are recorded by Windows Error Reporting in the Application log.";
        a.suggestion = L"Open System Diagnostics › Crashes to see which applications and modules are involved.";
        out.push_back(std::move(a));
    }
    if (in.unexpectedShutdowns30d >= 1) {
        DiagAlert a;
        a.severity = in.unexpectedShutdowns30d >= 3 ? DiagAlert::Warning : DiagAlert::Info;
        a.category = L"Stability";
        a.title = L"Unexpected shutdowns recorded";
        a.evidence.push_back(std::to_wstring(in.unexpectedShutdowns30d) + L" Kernel-Power event 41 entries in the last 30 days");
        if (!in.unexpectedShutdownTimes.empty()) {
            std::wstring when;
            for (auto& t : in.unexpectedShutdownTimes) when += (when.empty() ? L"" : L", ") + t;
            bool all = in.unexpectedShutdownTimes.size() >= static_cast<size_t>(in.unexpectedShutdowns30d);
            a.evidence.push_back((all ? L"When: " : L"Most recent: ") + when);
        }
        a.reasoning = L"Event 41 means Windows restarted without a clean shutdown (power loss, hard reset, hang or bugcheck). The event alone does not identify the "
                      L"cause. NEXUS only reports what Windows recorded.";
        a.suggestion = L"Correlate with bugcheck (1001) entries and power events in the event list.";
        out.push_back(std::move(a));
    }
    if (in.whea30d >= 1) {
        DiagAlert a;
        a.severity = DiagAlert::Warning;
        a.category = L"Stability";
        a.title = L"Hardware error events (WHEA) recorded";
        a.evidence.push_back(std::to_wstring(in.whea30d) + L" WHEA-Logger events in the last 30 days");
        a.reasoning = L"The Windows Hardware Error Architecture logs corrected and uncorrected errors reported by the CPU, memory controller or PCIe devices. Corrected errors can be benign in small numbers.";
        a.suggestion = L"Review the event details to see which component reported the errors.";
        out.push_back(std::move(a));
    }

    std::stable_sort(out.begin(), out.end(), [](const DiagAlert& a, const DiagAlert& b) { return a.severity > b.severity; });
    return out;
}

namespace {
constexpr uint64_t kDay = 864000000000ull; // in FILETIME units (100 ns)
constexpr int kOlderRun = 200;             // events in a row older than the window that end a pass
// An event dated in the future (the clock was changed) counts as new.
uint64_t Age(uint64_t time, uint64_t now) { return now > time ? now - time : 0; }
} // namespace

bool EventLogTally::AddSystem(const std::wstring& provider, uint32_t id, uint8_t level, uint64_t time, uint64_t now) {
    uint64_t age = Age(time, now);
    if (age > 30 * kDay) return ++systemOlder_ < kOlderRun;
    systemOlder_ = 0;
    std::wstring p = ToLower(provider);
    if (p == L"microsoft-windows-kernel-power" && id == 41) {
        ++kernelPower30d;
        if (kernelPowerTimes.size() < 5) kernelPowerTimes.push_back(time);
    }
    if (p == L"microsoft-windows-whea-logger") ++whea30d;
    if ((level == 1 || level == 2) && age <= kDay) ++errors24h;
    return true;
}

bool EventLogTally::AddApplication(uint32_t id, uint64_t time, uint64_t now) {
    if (Age(time, now) > kDay) return ++applicationOlder_ < kOlderRun;
    applicationOlder_ = 0;
    if (id == 1000 || id == 1002) ++crashes24h;
    return true;
}

} // namespace nx
