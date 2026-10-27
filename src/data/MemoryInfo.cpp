#include "core/Format.h"
#include "core/Win.h"
#include "data/Hardware.h"


#include <algorithm>

namespace nx {

SmbiosInfo QuerySmbios() {
    const DWORD sig = 0x52534D42; // 'RSMB'
    UINT size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (!size || size > 16 * 1024 * 1024) return {};
    std::vector<uint8_t> buf(size);
    if (GetSystemFirmwareTable(sig, 0, buf.data(), size) != size) return {};
    return ParseSmbios(buf.data(), buf.size());
}

MemStatic QueryMemStatic(const SmbiosInfo& smbios) {
    MemStatic m;
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb)) m.installed = kb * 1024ULL;
    m.modules = smbios.memory;
    m.slotsTotal = smbios.memorySlots ? smbios.memorySlots : static_cast<uint32_t>(smbios.memory.size());
    m.slotsUsed = smbios.SlotsUsed();
    for (auto& d : smbios.memory) {
        if (!d.sizeBytes) continue;
        m.speedMTs = std::max(m.speedMTs, d.speedMTs);
        if (d.configuredSpeedMTs) m.configuredSpeedMTs = m.configuredSpeedMTs ? std::min(m.configuredSpeedMTs, d.configuredSpeedMTs) : d.configuredSpeedMTs;
        if (m.formFactor.empty()) m.formFactor = d.FormFactorName();
        if (m.type.empty()) m.type = d.MemoryTypeName();
    }
    return m;
}

SystemStatic QuerySystem() {
    SystemStatic s;
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) s.computerName.assign(buf, n);

    OSVERSIONINFOEXW vi{sizeof vi};
    if (nt::Get().GetVersion && nt::Get().GetVersion(&vi) == 0) {
        s.major = vi.dwMajorVersion;
        s.minor = vi.dwMinorVersion;
        s.build = vi.dwBuildNumber;
    }
    RegKey k;
    if (k.Open(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")) {
        k.GetString(L"ProductName", s.osName);
        k.GetString(L"DisplayVersion", s.displayVersion);
        if (s.displayVersion.empty()) k.GetString(L"ReleaseId", s.displayVersion);
        k.GetString(L"BuildLabEx", s.buildLab);
        k.GetString(L"EditionID", s.edition);
        k.GetString(L"RegisteredOwner", s.registeredOwner);
        DWORD ubr = 0;
        if (k.GetDword(L"UBR", ubr)) s.ubr = ubr;
        DWORD inst = 0;
        if (k.GetDword(L"InstallDate", inst)) s.installDate = FormatFileTime((static_cast<uint64_t>(inst) + 11644473600ULL) * 10000000ULL);
    }
    // ProductName still says "Windows 10" on Windows 11; Microsoft documents build >= 22000 as Windows 11.
    if (s.build >= 22000) {
        size_t p = s.osName.find(L"Windows 10");
        if (p != std::wstring::npos) s.osName.replace(p, 10, L"Windows 11");
    }

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: s.architecture = L"x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: s.architecture = L"ARM64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: s.architecture = L"x86"; break;
    default: s.architecture = L"Unknown";
    }

    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    if (GetFirmwareType(&ft)) s.bootMode = ft == FirmwareTypeUefi ? L"UEFI" : ft == FirmwareTypeBios ? L"Legacy BIOS" : L"Unknown";

    RegKey sb;
    DWORD sbv = 0;
    if (sb.Open(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State") && sb.GetDword(L"UEFISecureBootEnabled", sbv))
        s.secureBoot = sbv ? L"On" : L"Off";
    else
        s.secureBoot = ft == FirmwareTypeUefi ? L"Unknown" : L"Unsupported (legacy BIOS)";

    // TPM Base Services (tbs.dll), loaded dynamically: TPM_DEVICE_INFO from tbs.h.
    struct TpmDeviceInfo { UINT32 structVersion, tpmVersion, tpmInterfaceType, tpmImpRevision; };
    using GetDeviceInfo_t = UINT32(WINAPI*)(UINT32, PVOID);
    s.tpm = L"Not detected";
    if (HMODULE tbs = LoadLibraryExW(L"tbs.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (auto gdi = reinterpret_cast<GetDeviceInfo_t>(GetProcAddress(tbs, "Tbsi_GetDeviceInfo"))) {
            TpmDeviceInfo tdi{};
            if (gdi(sizeof tdi, &tdi) == 0) {
                s.tpm = L"Present";
                s.tpmVersion = tdi.tpmVersion == 2 ? L"2.0" : tdi.tpmVersion == 1 ? L"1.2" : L"Unknown";
            }
        }
        FreeLibrary(tbs);
    }

    RegKey dg;
    DWORD vbs = 0, hv = 0;
    if (dg.Open(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard")) {
        s.vbs = dg.GetDword(L"EnableVirtualizationBasedSecurity", vbs) ? (vbs ? L"Enabled (configured)" : L"Disabled (configured)") : L"Not configured";
    }
    RegKey hk;
    if (hk.Open(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity") && hk.GetDword(L"Enabled", hv))
        s.hvci = hv ? L"On (Memory integrity)" : L"Off";
    else
        s.hvci = L"Not configured";

    s.bootTimeFt = NowFileTime() - GetTickCount64() * 10000ULL;
    s.smbios = QuerySmbios();
    return s;
}

} // namespace nx
