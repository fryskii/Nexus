#include "core/Smbios.h"
#include "core/Format.h"

#include <cstdio>
#include <cstring>

namespace nx {

namespace {

struct Reader {
    const uint8_t* p;  // start of structure
    uint8_t len;       // formatted area length
    const uint8_t* strings;
    const uint8_t* end;

    uint8_t U8(size_t off) const { return off < len ? p[off] : 0; }
    uint16_t U16(size_t off) const { return off + 1 < len ? static_cast<uint16_t>(p[off] | (p[off + 1] << 8)) : 0; }
    uint32_t U32(size_t off) const {
        return off + 3 < len ? static_cast<uint32_t>(p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (static_cast<uint32_t>(p[off + 3]) << 24)) : 0;
    }
    bool Has(size_t off, size_t n) const { return off + n <= len; }
    std::wstring Str(size_t off) const {
        uint8_t idx = U8(off);
        if (idx == 0) return {};
        const uint8_t* s = strings;
        for (uint8_t i = 1; s < end && *s; ++i) {
            const uint8_t* e = s;
            while (e < end && *e) ++e;
            if (i == idx) {
                std::string a(reinterpret_cast<const char*>(s), static_cast<size_t>(e - s));
                // Firmware strings are nominally ASCII; trim padding spaces.
                return Trim(FromUtf8(a));
            }
            s = e + 1;
        }
        return {};
    }
};

std::wstring Uuid(const uint8_t* u) {
    bool allFF = true, all0 = true;
    for (int i = 0; i < 16; ++i) { allFF &= u[i] == 0xFF; all0 &= u[i] == 0; }
    if (allFF || all0) return {};
    wchar_t buf[48];
    // SMBIOS 2.6+: first three fields little-endian.
    std::swprintf(buf, 48, L"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[3], u[2], u[1], u[0], u[5],
                  u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return buf;
}

} // namespace

std::wstring SmbiosMemoryDevice::FormFactorName() const {
    switch (formFactor) {
    case 0x03: return L"SIMM";
    case 0x05: return L"Chip";
    case 0x08: return L"Proprietary";
    case 0x09: return L"DIMM";
    case 0x0B: return L"Row of chips";
    case 0x0C: return L"RIMM";
    case 0x0D: return L"SODIMM";
    case 0x0F: return L"FB-DIMM";
    case 0x10: return L"Die";
    case 0x11: return L"CAMM";
    default: return L"Other";
    }
}

std::wstring SmbiosMemoryDevice::MemoryTypeName() const {
    switch (memoryType) {
    case 0x0F: return L"SDRAM";
    case 0x12: return L"DDR";
    case 0x13: return L"DDR2";
    case 0x14: return L"DDR2 FB-DIMM";
    case 0x18: return L"DDR3";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x1F: return L"Logical non-volatile";
    case 0x20: return L"HBM";
    case 0x21: return L"HBM2";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    case 0x24: return L"HBM3";
    default: return L"Unknown";
    }
}

uint32_t SmbiosInfo::SlotsUsed() const {
    uint32_t n = 0;
    for (auto& m : memory) n += m.sizeBytes ? 1 : 0;
    return n;
}

SmbiosInfo ParseSmbios(const uint8_t* data, size_t size) {
    SmbiosInfo info;
    if (!data || size < 8) return info;
    info.majorVersion = data[1];
    info.minorVersion = data[2];
    uint32_t tableLen = static_cast<uint32_t>(data[4] | (data[5] << 8) | (data[6] << 16) | (static_cast<uint32_t>(data[7]) << 24));
    const uint8_t* p = data + 8;
    const uint8_t* end = p + (tableLen < size - 8 ? tableLen : size - 8);

    while (p + 4 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 4 || p + len > end) break;
        // Locate the end of the string-set (double NUL).
        const uint8_t* s = p + len;
        const uint8_t* q = s;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        if (q + 1 >= end) break;
        Reader r{p, len, s, q + 1};

        switch (type) {
        case 0:
            info.biosVendor = r.Str(0x04);
            info.biosVersion = r.Str(0x05);
            info.biosDate = r.Str(0x08);
            break;
        case 1:
            info.systemManufacturer = r.Str(0x04);
            info.systemProduct = r.Str(0x05);
            info.systemVersion = r.Str(0x06);
            info.systemSerial = r.Str(0x07);
            if (r.Has(0x08, 16)) info.systemUuid = Uuid(p + 0x08);
            info.systemSku = r.Str(0x19);
            info.systemFamily = r.Str(0x1A);
            break;
        case 2:
            if (info.boardManufacturer.empty()) {
                info.boardManufacturer = r.Str(0x04);
                info.boardProduct = r.Str(0x05);
                info.boardVersion = r.Str(0x06);
                info.boardSerial = r.Str(0x07);
            }
            break;
        case 4: {
            SmbiosProcessor cpu;
            cpu.socket = r.Str(0x04);
            cpu.manufacturer = r.Str(0x07);
            cpu.version = r.Str(0x10);
            uint8_t v = r.U8(0x11);
            if (v & 0x80) cpu.voltage = (v & 0x7F) / 10.0;
            cpu.externalClockMHz = r.U16(0x12);
            cpu.maxSpeedMHz = r.U16(0x14);
            cpu.currentSpeedMHz = r.U16(0x16);
            cpu.coreCount = r.U8(0x23);
            cpu.threadCount = r.U8(0x25);
            if (cpu.coreCount == 0xFF && r.Has(0x2A, 2)) cpu.coreCount = r.U16(0x2A);
            if (cpu.threadCount == 0xFF && r.Has(0x2E, 2)) cpu.threadCount = r.U16(0x2E);
            // Status bit 6 = socket populated.
            if (r.U8(0x18) & 0x40) info.processors.push_back(std::move(cpu));
            break;
        }
        case 16:
            if (r.U8(0x05) == 0x03) { // system memory
                info.memorySlots += r.U16(0x0D);
                uint32_t maxKb = r.U32(0x07);
                uint64_t maxBytes = maxKb == 0x80000000u && r.Has(0x0F, 8)
                                        ? (static_cast<uint64_t>(r.U32(0x0F)) | (static_cast<uint64_t>(r.U32(0x13)) << 32))
                                        : static_cast<uint64_t>(maxKb) * 1024ULL;
                info.maxMemoryCapacityBytes += maxBytes;
            }
            break;
        case 17: {
            SmbiosMemoryDevice m;
            m.totalWidth = r.U16(0x08);
            m.dataWidth = r.U16(0x0A);
            uint16_t sz = r.U16(0x0C);
            if (sz == 0x7FFF && r.Has(0x1C, 4)) m.sizeBytes = static_cast<uint64_t>(r.U32(0x1C) & 0x7FFFFFFF) * 1024ULL * 1024ULL;
            else if (sz != 0 && sz != 0xFFFF) m.sizeBytes = (sz & 0x8000) ? static_cast<uint64_t>(sz & 0x7FFF) * 1024ULL
                                                                           : static_cast<uint64_t>(sz) * 1024ULL * 1024ULL;
            m.formFactor = r.U8(0x0E);
            m.deviceLocator = r.Str(0x10);
            m.bankLocator = r.Str(0x11);
            m.memoryType = r.U8(0x12);
            m.speedMTs = r.U16(0x15);
            m.manufacturer = r.Str(0x17);
            m.serial = r.Str(0x18);
            m.partNumber = r.Str(0x1A);
            m.rank = r.U8(0x1B) & 0x0F;
            m.configuredSpeedMTs = r.U16(0x20);
            m.configuredVoltageMv = r.U16(0x26);
            if (m.speedMTs == 0xFFFF && r.Has(0x54, 4)) m.speedMTs = r.U32(0x54);
            if (m.configuredSpeedMTs == 0xFFFF && r.Has(0x58, 4)) m.configuredSpeedMTs = r.U32(0x58);
            info.memory.push_back(std::move(m));
            break;
        }
        case 127:
            p = end; // end-of-table
            continue;
        default: break;
        }
        p = q + 2;
    }
    info.valid = true;
    return info;
}

} // namespace nx
