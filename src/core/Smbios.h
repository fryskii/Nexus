#pragma once
// SMBIOS (DMTF DSP0134) table parser. Input is the raw buffer returned by
// GetSystemFirmwareTable('RSMB', 0, ...), i.e. a RawSMBIOSData header followed by the table.
// Pure byte parsing with full bounds checking: malformed firmware data must never crash NEXUS.
#include <cstdint>
#include <string>
#include <vector>

namespace nx {

struct SmbiosMemoryDevice {
    std::wstring deviceLocator, bankLocator, manufacturer, partNumber, serial;
    uint64_t sizeBytes = 0;          // 0 = slot empty
    uint32_t speedMTs = 0;           // maximum capable speed (MT/s)
    uint32_t configuredSpeedMTs = 0; // configured speed (MT/s)
    uint16_t configuredVoltageMv = 0;
    uint8_t formFactor = 0, memoryType = 0, rank = 0;
    uint16_t dataWidth = 0, totalWidth = 0;
    std::wstring FormFactorName() const;
    std::wstring MemoryTypeName() const;
};

struct SmbiosProcessor {
    std::wstring socket, manufacturer, version;
    uint32_t maxSpeedMHz = 0, currentSpeedMHz = 0, externalClockMHz = 0;
    double voltage = 0; // volts, 0 if unknown
    uint16_t coreCount = 0, threadCount = 0;
};

struct SmbiosInfo {
    bool valid = false;
    uint8_t majorVersion = 0, minorVersion = 0;
    std::wstring biosVendor, biosVersion, biosDate;
    std::wstring systemManufacturer, systemProduct, systemVersion, systemSerial, systemSku, systemFamily, systemUuid;
    std::wstring boardManufacturer, boardProduct, boardVersion, boardSerial;
    std::vector<SmbiosProcessor> processors;
    std::vector<SmbiosMemoryDevice> memory; // includes empty slots
    uint32_t memorySlots = 0;              // from Type 16 (sum over system-memory arrays), 0 if unknown
    uint64_t maxMemoryCapacityBytes = 0;

    uint32_t SlotsUsed() const;
};

SmbiosInfo ParseSmbios(const uint8_t* data, size_t size);

} // namespace nx
