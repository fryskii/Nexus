#include "core/Format.h"
#include "core/Win.h"
#include "data/Hardware.h"

#include <powrprof.h>

#include <algorithm>
#include <cstring>
#include <set>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace nx {

namespace {

void Cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#if defined(_MSC_VER)
    int x[4];
    __cpuidex(x, static_cast<int>(leaf), static_cast<int>(sub));
    for (int i = 0; i < 4; ++i) r[i] = static_cast<uint32_t>(x[i]);
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

struct PROCESSOR_POWER_INFORMATION_NX { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; };

} // namespace

CpuStatic QueryCpuStatic() {
    CpuStatic c;
    uint32_t r[4];
    Cpuid(0, 0, r);
    uint32_t maxLeaf = r[0];
    char vendor[13] = {};
    std::memcpy(vendor, &r[1], 4);
    std::memcpy(vendor + 4, &r[3], 4);
    std::memcpy(vendor + 8, &r[2], 4);
    c.vendor = FromUtf8(vendor);

    Cpuid(0x80000000, 0, r);
    uint32_t maxExt = r[0];
    if (maxExt >= 0x80000004) {
        char brand[49] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            Cpuid(0x80000002 + i, 0, r);
            std::memcpy(brand + i * 16, r, 16);
        }
        c.brand = Trim(FromUtf8(brand));
    }

    uint32_t ecx1 = 0, edx1 = 0, ebx7 = 0, ecx7 = 0, edx7 = 0, eax7_1 = 0, ecxExt = 0, edxExt = 0;
    if (maxLeaf >= 1) {
        Cpuid(1, 0, r);
        uint32_t baseFamily = (r[0] >> 8) & 0xF, baseModel = (r[0] >> 4) & 0xF;
        c.family = baseFamily == 0xF ? baseFamily + ((r[0] >> 20) & 0xFF) : baseFamily;
        c.model = (baseFamily == 0x6 || baseFamily == 0xF) ? (((r[0] >> 16) & 0xF) << 4) + baseModel : baseModel;
        c.stepping = r[0] & 0xF;
        ecx1 = r[2];
        edx1 = r[3];
        c.hypervisorPresent = (ecx1 >> 31) & 1;
    }
    if (maxLeaf >= 7) {
        Cpuid(7, 0, r);
        ebx7 = r[1]; ecx7 = r[2]; edx7 = r[3];
        uint32_t sub = r[0];
        if (sub >= 1) { Cpuid(7, 1, r); eax7_1 = r[0]; }
    }
    if (maxExt >= 0x80000001) {
        Cpuid(0x80000001, 0, r);
        ecxExt = r[2]; edxExt = r[3];
    }
    struct F { uint32_t* reg; int bit; const wchar_t* name; };
    const F feats[] = {
        {&edx1, 23, L"MMX"},   {&edx1, 25, L"SSE"},       {&edx1, 26, L"SSE2"},     {&ecx1, 0, L"SSE3"},       {&ecx1, 9, L"SSSE3"},
        {&ecx1, 19, L"SSE4.1"}, {&ecx1, 20, L"SSE4.2"},   {&ecx1, 12, L"FMA3"},     {&ecx1, 25, L"AES-NI"},    {&ecx1, 1, L"PCLMULQDQ"},
        {&ecx1, 28, L"AVX"},   {&ecx1, 29, L"F16C"},      {&ecx1, 30, L"RDRAND"},   {&ecx1, 22, L"MOVBE"},     {&ecx1, 23, L"POPCNT"},
        {&ecx1, 5, L"VMX"},    {&ebx7, 5, L"AVX2"},       {&ebx7, 3, L"BMI1"},      {&ebx7, 8, L"BMI2"},       {&ebx7, 16, L"AVX-512F"},
        {&ebx7, 17, L"AVX-512DQ"}, {&ebx7, 30, L"AVX-512BW"}, {&ebx7, 31, L"AVX-512VL"}, {&ebx7, 28, L"AVX-512CD"}, {&ebx7, 29, L"SHA"},
        {&ebx7, 18, L"RDSEED"}, {&ebx7, 19, L"ADX"},      {&ebx7, 11, L"RTM"},      {&ebx7, 2, L"SGX"},        {&ecx7, 1, L"AVX-512VBMI"},
        {&ecx7, 11, L"AVX-512VNNI"}, {&ecx7, 8, L"GFNI"}, {&ecx7, 9, L"VAES"},      {&ecx7, 10, L"VPCLMULQDQ"}, {&edx7, 22, L"AMX-BF16"},
        {&edx7, 24, L"AMX-TILE"}, {&edx7, 25, L"AMX-INT8"}, {&eax7_1, 4, L"AVX-VNNI"}, {&ecxExt, 6, L"SSE4a"}, {&ecxExt, 2, L"SVM"},
        {&ecxExt, 5, L"LZCNT"}, {&edxExt, 29, L"x86-64"}, {&edxExt, 20, L"NX"},
    };
    for (auto& f : feats)
        if ((*f.reg >> f.bit) & 1) c.features.push_back(f.name);
    c.virtualizationVendor = ((ecx1 >> 5) & 1) ? L"VT-x" : ((ecxExt >> 2) & 1) ? L"AMD-V" : L"";

    c.virtualizationFirmware = IsProcessorFeaturePresent(21 /*PF_VIRT_FIRMWARE_ENABLED*/) != FALSE;
    c.slat = IsProcessorFeaturePresent(20 /*PF_SECOND_LEVEL_ADDRESS_TRANSLATION*/) != FALSE;

    // Topology and caches.
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    std::vector<uint8_t> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
        std::vector<CacheInfo> caches;
        uint32_t coreIndex = 0;
        for (size_t off = 0; off < len;) {
            auto* e = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (e->Size == 0) break;
            switch (e->Relationship) {
            case RelationProcessorPackage: ++c.sockets; break;
            case RelationNumaNode: ++c.numaNodes; break;
            case RelationProcessorCore: {
                ++c.cores;
                // EfficiencyClass is the byte after Flags (older SDK headers name it Reserved[0]).
                uint8_t eff = reinterpret_cast<const BYTE*>(&e->Processor)[1];
                uint32_t lpCount = 0;
                for (WORD g = 0; g < e->Processor.GroupCount; ++g) {
                    KAFFINITY mask = e->Processor.GroupMask[g].Mask;
                    for (int b = 0; b < 64; ++b) {
                        if (!(mask & (static_cast<KAFFINITY>(1) << b))) continue;
                        size_t lp = static_cast<size_t>(e->Processor.GroupMask[g].Group) * 64 + b;
                        if (c.coreEfficiencyClass.size() <= lp) { c.coreEfficiencyClass.resize(lp + 1); c.logicalToCore.resize(lp + 1); }
                        c.coreEfficiencyClass[lp] = eff;
                        c.logicalToCore[lp] = coreIndex;
                        ++lpCount;
                    }
                }
                c.logical += lpCount;
                ++coreIndex;
                break;
            }
            case RelationCache: {
                auto& ch = e->Cache;
                wchar_t t = ch.Type == CacheData ? L'D' : ch.Type == CacheInstruction ? L'I' : ch.Type == CacheTrace ? L'T' : L'U';
                bool found = false;
                for (auto& ci : caches) {
                    if (ci.level == ch.Level && ci.type == t && ci.size == ch.CacheSize) { ++ci.count; found = true; break; }
                }
                if (!found) {
                    CacheInfo ci;
                    ci.level = ch.Level; ci.type = t; ci.size = ch.CacheSize; ci.count = 1; ci.lineSize = ch.LineSize;
                    ci.associativity = ch.Associativity;
                    caches.push_back(ci);
                }
                break;
            }
            default: break;
            }
            off += e->Size;
        }
        std::sort(caches.begin(), caches.end(), [](const CacheInfo& a, const CacheInfo& b) { return a.level != b.level ? a.level < b.level : a.type < b.type; });
        c.caches = caches;
        // Hybrid architecture: the highest efficiency class are performance cores.
        if (!c.coreEfficiencyClass.empty()) {
            uint8_t maxClass = *std::max_element(c.coreEfficiencyClass.begin(), c.coreEfficiencyClass.end());
            if (maxClass > 0) {
                std::set<uint32_t> pc, ec;
                for (size_t i = 0; i < c.coreEfficiencyClass.size(); ++i)
                    (c.coreEfficiencyClass[i] == maxClass ? pc : ec).insert(c.logicalToCore[i]);
                c.performanceCores = static_cast<uint32_t>(pc.size());
                c.efficiencyCores = static_cast<uint32_t>(ec.size());
            }
        }
    }
    if (!c.logical) {
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        c.logical = si.dwNumberOfProcessors;
    }

    RegKey k;
    DWORD mhz = 0;
    if (k.Open(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0") && k.GetDword(L"~MHz", mhz)) c.baseMHz = mhz;
    std::vector<PROCESSOR_POWER_INFORMATION_NX> ppi(c.logical ? c.logical : 1);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), static_cast<ULONG>(ppi.size() * sizeof(ppi[0]))) == 0) {
        for (auto& p : ppi) c.maxMHz = std::max<double>(c.maxMHz, p.MaxMhz);
        if (!c.baseMHz) c.baseMHz = ppi[0].MaxMhz;
    }
    return c;
}

} // namespace nx
