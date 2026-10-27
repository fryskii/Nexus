#include "core/Format.h"
#include "core/Win.h"
#include "data/Hardware.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace nx {

std::wstring DiskStatic::Letters() const {
    std::wstring s;
    for (auto& v : volumes) {
        if (v.letter.empty()) continue;
        if (!s.empty()) s += L' ';
        s += v.letter;
    }
    return s;
}

namespace {

std::wstring DescStr(const std::vector<uint8_t>& buf, DWORD off) {
    if (!off || off >= buf.size()) return {};
    const char* s = reinterpret_cast<const char*>(buf.data() + off);
    size_t n = strnlen(s, buf.size() - off);
    return Trim(FromUtf8(std::string(s, n)));
}

const wchar_t* BusName(STORAGE_BUS_TYPE t) {
    switch (t) {
    case BusTypeScsi: return L"SCSI";
    case BusTypeAtapi: return L"ATAPI";
    case BusTypeAta: return L"ATA";
    case BusType1394: return L"IEEE 1394";
    case BusTypeSsa: return L"SSA";
    case BusTypeFibre: return L"Fibre Channel";
    case BusTypeUsb: return L"USB";
    case BusTypeRAID: return L"RAID";
    case BusTypeiScsi: return L"iSCSI";
    case BusTypeSas: return L"SAS";
    case BusTypeSata: return L"SATA";
    case BusTypeSd: return L"SD";
    case BusTypeMmc: return L"MMC";
    case BusTypeVirtual: return L"Virtual";
    case BusTypeFileBackedVirtual: return L"File-backed virtual";
    case BusTypeSpaces: return L"Storage Spaces";
    case BusTypeNvme: return L"NVMe";
    case BusTypeSCM: return L"Storage class memory";
    case BusTypeUfs: return L"UFS";
    default: return L"Unknown";
    }
}

Handle OpenDisk(int index, DWORD access) {
    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", index);
    return Handle(CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
}

std::wstring GptTypeName(const GUID& g) {
    static const struct { GUID g; const wchar_t* n; } known[] = {
        {{0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}}, L"EFI System"},
        {{0xE3C9E316, 0x0B5C, 0x4DB8, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}}, L"Microsoft Reserved"},
        {{0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}}, L"Basic data"},
        {{0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}}, L"Recovery"},
        {{0x5808C8AA, 0x7E8F, 0x42E0, {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}}, L"LDM metadata"},
        {{0xAF9B60A0, 0x1431, 0x4F62, {0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD}}, L"LDM data"},
        {{0xE75CAF8F, 0xF680, 0x4CEE, {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}}, L"Storage Spaces"},
        {{0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}}, L"Linux filesystem"},
        {{0x0657FD6D, 0xA4AB, 0x43C4, {0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F}}, L"Linux swap"},
    };
    for (auto& k : known)
        if (memcmp(&k.g, &g, sizeof(GUID)) == 0) return k.n;
    return L"Other";
}

const wchar_t* SmartName(int id) {
    switch (id) {
    case 1: return L"Raw read error rate";
    case 3: return L"Spin-up time";
    case 4: return L"Start/stop count";
    case 5: return L"Reallocated sectors count";
    case 7: return L"Seek error rate";
    case 9: return L"Power-on hours";
    case 10: return L"Spin retry count";
    case 12: return L"Power cycle count";
    case 170: return L"Available reserved space";
    case 171: return L"Program fail count";
    case 172: return L"Erase fail count";
    case 173: return L"Wear leveling count";
    case 174: return L"Unexpected power loss count";
    case 177: return L"Wear range delta / Wear leveling";
    case 179: return L"Used reserved block count";
    case 181: return L"Program fail count";
    case 182: return L"Erase fail count";
    case 183: return L"SATA downshift / Runtime bad block";
    case 184: return L"End-to-end error";
    case 187: return L"Reported uncorrectable errors";
    case 188: return L"Command timeout";
    case 190: return L"Airflow temperature";
    case 192: return L"Power-off retract count";
    case 193: return L"Load cycle count";
    case 194: return L"Temperature";
    case 195: return L"Hardware ECC recovered";
    case 196: return L"Reallocation event count";
    case 197: return L"Current pending sector count";
    case 198: return L"Uncorrectable sector count";
    case 199: return L"UltraDMA CRC error count";
    case 231: return L"SSD life left";
    case 232: return L"Endurance remaining";
    case 233: return L"Media wearout indicator";
    case 241: return L"Total LBAs written";
    case 242: return L"Total LBAs read";
    default: return L"Vendor specific";
    }
}

uint64_t Le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

} // namespace

std::vector<DiskStatic> QueryDisks() {
    std::vector<DiskStatic> disks;
    wchar_t sysDrive[4] = L"C:";
    GetEnvironmentVariableW(L"SystemDrive", sysDrive, 4);

    for (int i = 0; i < 64; ++i) {
        Handle h = OpenDisk(i, 0);
        if (!h) continue;
        DiskStatic d;
        d.index = i;
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<uint8_t> buf(4096);
        DWORD ret = 0;
        if (DeviceIoControl(h.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr) &&
            ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            auto* dd = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
            d.vendor = DescStr(buf, dd->VendorIdOffset);
            d.model = DescStr(buf, dd->ProductIdOffset);
            d.firmware = DescStr(buf, dd->ProductRevisionOffset);
            d.serial = DescStr(buf, dd->SerialNumberOffset);
            d.busType = BusName(dd->BusType);
            d.removable = dd->RemovableMedia != 0;
            if (!d.vendor.empty() && d.model.rfind(d.vendor, 0) != 0) d.model = d.vendor + L" " + d.model;
        }
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
        if (DeviceIoControl(h.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &ret, nullptr) && ret >= sizeof sp) {
            d.seekPenaltyKnown = true;
            d.ssd = !sp.IncursSeekPenalty;
        }
        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h.Get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &ret, nullptr)) d.size = static_cast<uint64_t>(geo.DiskSize.QuadPart);

        // Full partition layout needs read access (administrator); fall back to volume extents.
        Handle hr = OpenDisk(i, GENERIC_READ);
        if (hr) {
            std::vector<uint8_t> lb(64 * 1024);
            if (DeviceIoControl(hr.Get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, lb.data(), static_cast<DWORD>(lb.size()), &ret, nullptr)) {
                auto* L = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(lb.data());
                d.partitionStyle = L->PartitionStyle == PARTITION_STYLE_GPT ? L"GPT" : L->PartitionStyle == PARTITION_STYLE_MBR ? L"MBR" : L"RAW";
                for (DWORD p = 0; p < L->PartitionCount && p < 128; ++p) {
                    auto& pe = L->PartitionEntry[p];
                    if (pe.PartitionLength.QuadPart == 0) continue;
                    if (pe.PartitionStyle == PARTITION_STYLE_MBR && pe.Mbr.PartitionType == 0) continue;
                    DiskStatic::Partition part;
                    part.number = pe.PartitionNumber;
                    part.offset = static_cast<uint64_t>(pe.StartingOffset.QuadPart);
                    part.length = static_cast<uint64_t>(pe.PartitionLength.QuadPart);
                    if (pe.PartitionStyle == PARTITION_STYLE_GPT) {
                        part.style = L"GPT";
                        part.type = GptTypeName(pe.Gpt.PartitionType);
                        part.name.assign(pe.Gpt.Name, wcsnlen(pe.Gpt.Name, 36));
                    } else {
                        part.style = L"MBR";
                        part.type = FormatHex(pe.Mbr.PartitionType, 2);
                        part.boot = pe.Mbr.BootIndicator != 0;
                    }
                    d.partitions.push_back(part);
                }
            }
        }
        disks.push_back(std::move(d));
    }

    // Volumes -> disks.
    wchar_t vol[MAX_PATH];
    HANDLE fv = FindFirstVolumeW(vol, MAX_PATH);
    if (fv != INVALID_HANDLE_VALUE) {
        do {
            DiskStatic::Volume v;
            v.guidPath = vol;
            wchar_t names[1024];
            DWORD nl = 0;
            if (GetVolumePathNamesForVolumeNameW(vol, names, 1024, &nl) && names[0]) {
                v.letter = names;
                if (v.letter.size() == 3 && v.letter[1] == L':') v.letter.resize(2);
            }
            wchar_t label[MAX_PATH] = {}, fs[64] = {};
            DWORD serial = 0, maxComp = 0, flags = 0;
            if (GetVolumeInformationW(vol, label, MAX_PATH, &serial, &maxComp, &flags, fs, 64)) {
                v.label = label;
                v.fileSystem = fs;
            }
            ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
            if (GetDiskFreeSpaceExW(vol, &freeAvail, &total, &freeTotal)) {
                v.total = total.QuadPart;
                v.free = freeTotal.QuadPart;
            }
            DWORD spc = 0, bps = 0, fc = 0, tc = 0;
            if (GetDiskFreeSpaceW(vol, &spc, &bps, &fc, &tc)) v.clusterSize = spc * bps;

            std::wstring dev = vol;
            if (!dev.empty() && dev.back() == L'\\') dev.pop_back();
            Handle vh(CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            if (vh) {
                std::vector<uint8_t> eb(sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT));
                DWORD ret = 0;
                if (DeviceIoControl(vh.Get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, eb.data(), static_cast<DWORD>(eb.size()), &ret, nullptr)) {
                    auto* ex = reinterpret_cast<VOLUME_DISK_EXTENTS*>(eb.data());
                    if (ex->NumberOfDiskExtents >= 1) {
                        int dn = static_cast<int>(ex->Extents[0].DiskNumber);
                        for (auto& d : disks) {
                            if (d.index != dn) continue;
                            if (!v.letter.empty() && _wcsicmp(v.letter.c_str(), sysDrive) == 0) d.isSystem = true;
                            if (!v.letter.empty()) {
                                std::wstring pf = v.letter + L"\\pagefile.sys";
                                if (GetFileAttributesW(pf.c_str()) != INVALID_FILE_ATTRIBUTES) d.hasPageFile = true;
                            }
                            if (d.partitions.empty() || d.partitionStyle.empty()) {
                                // Derive a partial layout from extents when the full layout is not accessible.
                                DiskStatic::Partition part;
                                part.offset = static_cast<uint64_t>(ex->Extents[0].StartingOffset.QuadPart);
                                part.length = static_cast<uint64_t>(ex->Extents[0].ExtentLength.QuadPart);
                                part.type = L"Volume";
                                part.name = v.letter;
                                d.partitions.push_back(part);
                            }
                            d.volumes.push_back(v);
                        }
                    }
                }
            }
        } while (FindNextVolumeW(fv, vol, MAX_PATH));
        FindVolumeClose(fv);
    }
    for (auto& d : disks) {
        std::sort(d.partitions.begin(), d.partitions.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        std::sort(d.volumes.begin(), d.volumes.end(), [](const auto& a, const auto& b) { return a.letter < b.letter; });
    }
    return disks;
}

StorageHealth QueryStorageHealth(int index) {
    StorageHealth h;
    Handle hd = OpenDisk(index, 0);
    if (!hd) { h.unavailableReason = L"Device could not be opened"; return h; }
    DWORD ret = 0;

    // 1) Temperature property (Windows 10+, no special rights on most drivers).
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceTemperatureProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<uint8_t> b(1024);
        if (DeviceIoControl(hd.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, b.data(), static_cast<DWORD>(b.size()), &ret, nullptr) &&
            ret >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) {
            auto* t = reinterpret_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(b.data());
            // Drivers that don't implement the sensor report 0 or garbage; accept only plausible values.
            if (t->InfoCount > 0 && t->TemperatureInfo[0].Temperature > 0 && t->TemperatureInfo[0].Temperature < 150) {
                h.temperatureC = t->TemperatureInfo[0].Temperature;
                h.temperatureWarnC = t->WarningTemperature ? t->WarningTemperature : -1;
                h.temperatureCritC = t->CriticalTemperature ? t->CriticalTemperature : -1;
                h.available = true;
                h.source = L"Storage temperature property";
            }
        }
    }

    // 2) NVMe health information log page (SMART equivalent for NVMe).
    {
        constexpr DWORD kLog = 512;
        std::vector<uint8_t> b(FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + kLog);
        auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(b.data());
        q->PropertyId = StorageDeviceProtocolSpecificProperty;
        q->QueryType = PropertyStandardQuery;
        auto* psd = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
        psd->ProtocolType = ProtocolTypeNvme;
        psd->DataType = NVMeDataTypeLogPage;
        psd->ProtocolDataRequestValue = 2; // SMART / Health Information
        psd->ProtocolDataRequestSubValue = 0;
        psd->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
        psd->ProtocolDataLength = kLog;
        if (DeviceIoControl(hd.Get(), IOCTL_STORAGE_QUERY_PROPERTY, b.data(), static_cast<DWORD>(b.size()), b.data(), static_cast<DWORD>(b.size()), &ret, nullptr)) {
            auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(b.data());
            auto* data = &desc->ProtocolSpecificData;
            if (data->ProtocolDataOffset && data->ProtocolDataLength >= kLog &&
                reinterpret_cast<uint8_t*>(data) + data->ProtocolDataOffset + kLog <= b.data() + b.size()) {
                const uint8_t* log = reinterpret_cast<uint8_t*>(data) + data->ProtocolDataOffset;
                h.available = true;
                h.source = L"NVMe health information log";
                h.criticalWarning = log[0];
                uint16_t k = static_cast<uint16_t>(log[1] | (log[2] << 8));
                if (k > 273 && k < 273 + 150) h.temperatureC = k - 273.0;
                h.availableSpare = log[3];
                h.spareThreshold = log[4];
                h.percentUsed = log[5];
                h.dataReadBytes = Le64(log + 32) * 512000ULL;
                h.dataWrittenBytes = Le64(log + 48) * 512000ULL;
                h.powerCycles = Le64(log + 112);
                h.powerOnHours = Le64(log + 128);
                h.unsafeShutdowns = Le64(log + 144);
                h.mediaErrors = Le64(log + 160);
                h.errorLogEntries = Le64(log + 176);
                if (h.criticalWarning) h.healthEstimate = L"Bad — drive reports a critical warning";
                else if (h.percentUsed >= 100 || (h.availableSpare >= 0 && h.availableSpare <= h.spareThreshold)) h.healthEstimate = L"Caution — endurance or spare exhausted";
                else h.healthEstimate = L"Good — " + std::to_wstring(100 - std::min(100, h.percentUsed)) + L"% endurance remaining (drive-reported)";
                return h;
            }
        }
    }

    // 3) ATA SMART (requires administrator: read/write handle).
    Handle hrw = OpenDisk(index, GENERIC_READ | GENERIC_WRITE);
    if (hrw) {
        SENDCMDINPARAMS in{};
        in.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
        in.irDriveRegs.bSectorCountReg = 1;
        in.irDriveRegs.bSectorNumberReg = 1;
        in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
        in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
        in.irDriveRegs.bCommandReg = SMART_CMD;
        in.cBufferSize = READ_ATTRIBUTE_BUFFER_SIZE;
        std::vector<uint8_t> out(sizeof(SENDCMDOUTPARAMS) + READ_ATTRIBUTE_BUFFER_SIZE);
        std::vector<uint8_t> thr(sizeof(SENDCMDOUTPARAMS) + READ_THRESHOLD_BUFFER_SIZE);
        bool okA = DeviceIoControl(hrw.Get(), SMART_RCV_DRIVE_DATA, &in, sizeof in - 1, out.data(), static_cast<DWORD>(out.size()), &ret, nullptr) != FALSE;
        in.irDriveRegs.bFeaturesReg = READ_THRESHOLDS;
        bool okT = DeviceIoControl(hrw.Get(), SMART_RCV_DRIVE_DATA, &in, sizeof in - 1, thr.data(), static_cast<DWORD>(thr.size()), &ret, nullptr) != FALSE;
        if (okA) {
            const uint8_t* a = reinterpret_cast<SENDCMDOUTPARAMS*>(out.data())->bBuffer + 2;
            const uint8_t* t = okT ? reinterpret_cast<SENDCMDOUTPARAMS*>(thr.data())->bBuffer + 2 : nullptr;
            bool anyFail = false;
            for (int i = 0; i < 30; ++i) {
                const uint8_t* e = a + i * 12;
                if (!e[0]) continue;
                StorageHealth::Attr at;
                at.id = e[0];
                at.name = SmartName(e[0]);
                at.current = e[3];
                at.worst = e[4];
                at.raw = static_cast<uint64_t>(e[5]) | (static_cast<uint64_t>(e[6]) << 8) | (static_cast<uint64_t>(e[7]) << 16) |
                         (static_cast<uint64_t>(e[8]) << 24) | (static_cast<uint64_t>(e[9]) << 32) | (static_cast<uint64_t>(e[10]) << 40);
                if (t) {
                    for (int j = 0; j < 30; ++j)
                        if (t[j * 12] == e[0]) { at.threshold = t[j * 12 + 1]; break; }
                }
                at.failing = at.threshold > 0 && at.current <= at.threshold;
                anyFail |= at.failing;
                if (at.id == 194 && h.temperatureC < 0) h.temperatureC = static_cast<double>(at.raw & 0xFF);
                if (at.id == 9) h.powerOnHours = at.raw & 0xFFFFFFFF;
                if (at.id == 12) h.powerCycles = at.raw;
                h.smart.push_back(at);
            }
            if (!h.smart.empty()) {
                h.available = true;
                h.source = L"ATA SMART";
                uint64_t realloc = 0, pending = 0;
                for (auto& at : h.smart) {
                    if (at.id == 5) realloc = at.raw;
                    if (at.id == 197) pending = at.raw;
                }
                if (anyFail) h.healthEstimate = L"Bad — an attribute is at or below its failure threshold";
                else if (realloc > 0 || pending > 0) h.healthEstimate = L"Caution — reallocated or pending sectors present";
                else h.healthEstimate = L"Good — all attributes above thresholds";
                return h;
            }
        }
    } else if (!h.available) {
        h.unavailableReason = IsElevated() ? L"The drive or its driver does not expose SMART/health data"
                                           : L"Run NEXUS as administrator to read SMART data for this drive";
    }
    if (h.available && h.healthEstimate.empty())
        h.healthEstimate = IsElevated() ? L"Unknown — only temperature is exposed" : L"Unknown — administrator rights needed for SMART";
    return h;
}

} // namespace nx
