#pragma once
// Static (rarely changing) hardware descriptions. Gathered lazily, once, on a worker thread.
#include "core/Smbios.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx {

struct CacheInfo { int level = 0; wchar_t type = L'U'; uint64_t size = 0; uint32_t count = 0; uint32_t lineSize = 0; uint32_t associativity = 0; };

struct CpuStatic {
    std::wstring brand, vendor, socket;
    uint32_t family = 0, model = 0, stepping = 0;
    uint32_t sockets = 0, cores = 0, logical = 0, numaNodes = 0;
    uint32_t performanceCores = 0, efficiencyCores = 0; // hybrid CPUs (EfficiencyClass)
    std::vector<uint8_t> coreEfficiencyClass;           // per logical processor
    std::vector<uint32_t> logicalToCore;                // per logical processor -> core index
    double baseMHz = 0, maxMHz = 0;                     // registry ~MHz / CallNtPowerInformation
    double smbiosVoltage = 0;                           // nominal from SMBIOS (not live)
    uint32_t smbiosMaxMHz = 0, externalClockMHz = 0;
    std::vector<CacheInfo> caches;
    std::vector<std::wstring> features;                 // instruction set extensions
    bool virtualizationFirmware = false, hypervisorPresent = false, slat = false;
    std::wstring virtualizationVendor;                  // VT-x / AMD-V
};

struct MemStatic {
    uint64_t installed = 0;
    uint32_t slotsTotal = 0, slotsUsed = 0;
    uint32_t speedMTs = 0, configuredSpeedMTs = 0;
    std::wstring formFactor, type;
    std::vector<SmbiosMemoryDevice> modules;
};

struct DiskStatic {
    int index = -1;
    std::wstring model, serial, firmware, vendor, busType;
    bool ssd = false, seekPenaltyKnown = false, removable = false, isSystem = false, hasPageFile = false;
    uint64_t size = 0;
    struct Partition { uint32_t number = 0; uint64_t offset = 0, length = 0; std::wstring style, type, name; bool boot = false; };
    std::vector<Partition> partitions;
    std::wstring partitionStyle;
    struct Volume { std::wstring letter, label, fileSystem, guidPath; uint64_t total = 0, free = 0; uint32_t clusterSize = 0; bool bitlocker = false; };
    std::vector<Volume> volumes;
    std::wstring Letters() const;
};

struct StorageHealth {
    bool available = false;
    std::wstring source;        // "NVMe health log", "ATA SMART", "Storage temperature property"
    std::wstring unavailableReason;
    double temperatureC = -1, temperatureWarnC = -1, temperatureCritC = -1;
    int percentUsed = -1, availableSpare = -1, spareThreshold = -1;
    uint8_t criticalWarning = 0;
    uint64_t dataReadBytes = 0, dataWrittenBytes = 0, powerOnHours = 0, powerCycles = 0, unsafeShutdowns = 0, mediaErrors = 0, errorLogEntries = 0;
    struct Attr { int id = 0; std::wstring name; int current = 0, worst = 0, threshold = 0; uint64_t raw = 0; bool failing = false; };
    std::vector<Attr> smart;    // ATA SMART attributes
    std::wstring healthEstimate; // "Good", "Caution", "Bad" with basis
};

struct GpuStatic {
    uint64_t luid = 0;
    std::wstring name, driverVersion, driverDate, location, pnpId, vendor;
    uint32_t vendorId = 0, deviceId = 0;
    uint64_t dedicatedVideo = 0, dedicatedSystem = 0, sharedSystem = 0;
    std::wstring directX, featureLevel, wddm;
    bool software = false;
    int hagsSupported = -1, hagsEnabled = -1;
    std::wstring pcieCurrent, pcieMax;  // "Gen4 x16"
    double maxMemBandwidth = -1, maxPcieBandwidth = -1, maxFanRpm = -1, tempMaxC = -1, tempWarnC = -1;
};

struct NicStatic {
    uint64_t luid = 0;
    uint32_t ifIndex = 0;
    std::wstring alias, description, mac, dnsSuffix, adapterName;
    std::vector<std::wstring> ipv4, ipv6, gateways, dns;
    bool dhcp = false, wireless = false, up = false;
    uint32_t mtu = 0, ifType = 0;
    uint64_t speedTx = 0, speedRx = 0;
    std::wstring ssid;  // Wi-Fi network name when available
    std::wstring connectionType;
};

struct SystemStatic {
    std::wstring computerName, osName, displayVersion, buildLab, edition, installDate, registeredOwner;
    uint32_t major = 0, minor = 0, build = 0, ubr = 0;
    std::wstring architecture, bootMode, secureBoot, tpm, tpmVersion, vbs, hvci, bitlocker, kernelDebug;
    uint64_t bootTimeFt = 0;
    SmbiosInfo smbios;
};

CpuStatic QueryCpuStatic();
MemStatic QueryMemStatic(const SmbiosInfo& smbios);
SmbiosInfo QuerySmbios();
std::vector<DiskStatic> QueryDisks();
StorageHealth QueryStorageHealth(int diskIndex);
std::vector<GpuStatic> QueryGpus();
std::vector<NicStatic> QueryNics();
SystemStatic QuerySystem();

// Live hardware telemetry for a GPU adapter via D3DKMT (WDDM 2.4+); fields < 0 when unsupported.
struct GpuSensors;
bool QueryGpuSensors(uint64_t luid, double& tempC, double& fanRpm, double& powerPct, double& memFreqMHz, double& coreFreqMHz,
                     double& coreMaxMHz, double& voltage, double& memBw, double& pcieBw);

} // namespace nx
