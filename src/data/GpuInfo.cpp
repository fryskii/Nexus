#include "core/Format.h"
#include "core/Win.h"
#include "data/Hardware.h"

#include <dxgi.h>
#include <devpropdef.h>
#include <setupapi.h>

#include <cstring>

// DEVPKEY definitions (from devpkey.h / pciprop.h) declared locally to avoid INITGUID ordering issues.
static const DEVPROPKEY kDevDriverVersion = {{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 3};
static const DEVPROPKEY kDevDriverDate = {{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 2};
static const DEVPROPKEY kDevLocationInfo = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 15};
static const DEVPROPKEY kDevHardwareIds = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 3};
static const DEVPROPKEY kDevInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
static const GUID kDisplayClassGuid = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}}; // GUID_DEVCLASS_DISPLAY
static const GUID kPciPropGuid = {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}};

namespace nx {

namespace {

// --- D3DKMT (WDDM kernel thunk) definitions, from the WDK's d3dkmthk.h ---
using D3DKMT_HANDLE = UINT;
struct D3DKMT_OPENADAPTERFROMLUID { LUID AdapterLuid; D3DKMT_HANDLE hAdapter; };
struct D3DKMT_CLOSEADAPTER { D3DKMT_HANDLE hAdapter; };
struct D3DKMT_QUERYADAPTERINFO { D3DKMT_HANDLE hAdapter; UINT Type; VOID* pPrivateDriverData; UINT PrivateDriverDataSize; };
constexpr UINT KMTQAITYPE_DRIVERVERSION = 13;
constexpr UINT KMTQAITYPE_NODEPERFDATA = 61;
constexpr UINT KMTQAITYPE_ADAPTERPERFDATA = 62;
constexpr UINT KMTQAITYPE_ADAPTERPERFDATA_CAPS = 63;
constexpr UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;
#pragma pack(push, 8)
struct D3DKMT_ADAPTER_PERFDATA {
    UINT32 PhysicalAdapterIndex;
    ULONGLONG MemoryFrequency, MaxMemoryFrequency, MaxMemoryFrequencyOC, MemoryBandwidth, PCIEBandwidth;
    ULONG FanRPM, Power, Temperature;
    UCHAR PowerStateOverride;
};
struct D3DKMT_ADAPTER_PERFDATACAPS {
    UINT32 PhysicalAdapterIndex;
    ULONGLONG MaxMemoryBandwidth, MaxPCIEBandwidth;
    ULONG MaxFanRPM, TemperatureMax, TemperatureWarning;
};
struct D3DKMT_NODE_PERFDATA {
    UINT32 NodeOrdinal, PhysicalAdapterIndex;
    ULONGLONG Frequency, MaxFrequency, MaxFrequencyOC;
    ULONG Voltage, VoltageMax, VoltageMaxOC;
    ULONGLONG MaxTransitionLatency;
};
#pragma pack(pop)

using OpenFromLuid_t = LONG(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
using Close_t = LONG(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
using Query_t = LONG(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);

struct Kmt {
    OpenFromLuid_t open = nullptr;
    Close_t close = nullptr;
    Query_t query = nullptr;
};
const Kmt& K() {
    static Kmt k = [] {
        Kmt x;
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        if (!g) g = LoadLibraryW(L"gdi32.dll");
        if (g) {
            x.open = reinterpret_cast<OpenFromLuid_t>(GetProcAddress(g, "D3DKMTOpenAdapterFromLuid"));
            x.close = reinterpret_cast<Close_t>(GetProcAddress(g, "D3DKMTCloseAdapter"));
            x.query = reinterpret_cast<Query_t>(GetProcAddress(g, "D3DKMTQueryAdapterInfo"));
        }
        return x;
    }();
    return k;
}

class KmtAdapter {
public:
    explicit KmtAdapter(uint64_t luid) {
        if (!K().open) return;
        D3DKMT_OPENADAPTERFROMLUID o{};
        o.AdapterLuid.LowPart = static_cast<DWORD>(luid & 0xFFFFFFFF);
        o.AdapterLuid.HighPart = static_cast<LONG>(luid >> 32);
        if (K().open(&o) == 0) h_ = o.hAdapter;
    }
    ~KmtAdapter() {
        if (h_ && K().close) { D3DKMT_CLOSEADAPTER c{h_}; K().close(&c); }
    }
    bool Ok() const { return h_ != 0; }
    template <typename T>
    bool Query(UINT type, T& data) const {
        if (!h_ || !K().query) return false;
        D3DKMT_QUERYADAPTERINFO q{h_, type, &data, sizeof(T)};
        return K().query(&q) == 0;
    }
private:
    D3DKMT_HANDLE h_ = 0;
};

std::wstring PropString(HDEVINFO set, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    wchar_t buf[2048];
    DWORD size = 0;
    if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, reinterpret_cast<BYTE*>(buf), sizeof buf - sizeof(wchar_t), &size, 0)) return {};
    if (type == DEVPROP_TYPE_STRING || type == DEVPROP_TYPE_STRING_LIST) {
        buf[size / sizeof(wchar_t)] = 0;
        return buf;
    }
    if (type == DEVPROP_TYPE_FILETIME && size == sizeof(FILETIME)) {
        FILETIME f;
        memcpy(&f, buf, sizeof f);
        uint64_t ft = FileTimeToU64(f);
        return FormatFileTime(ft).substr(0, 10);
    }
    return {};
}

bool PropU32(HDEVINFO set, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key, uint32_t& out) {
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    return SetupDiGetDevicePropertyW(set, &dev, &key, &type, reinterpret_cast<BYTE*>(&out), sizeof out, &size, 0) && size == sizeof out;
}

std::wstring PcieText(uint32_t speed, uint32_t width) {
    if (!speed && !width) return {};
    static const wchar_t* gt[] = {L"", L"2.5 GT/s (Gen1)", L"5 GT/s (Gen2)", L"8 GT/s (Gen3)", L"16 GT/s (Gen4)", L"32 GT/s (Gen5)", L"64 GT/s (Gen6)"};
    std::wstring s = speed < 7 ? gt[speed] : L"Gen" + std::to_wstring(speed);
    if (width) s += L" x" + std::to_wstring(width);
    return s;
}

} // namespace

std::vector<GpuStatic> QueryGpus() {
    std::vector<GpuStatic> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.PutVoid()))) return out;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.Put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(ad->GetDesc1(&d))) continue;
        GpuStatic g;
        g.luid = (static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        bool dup = false;
        for (auto& e : out) dup |= e.luid == g.luid;
        if (dup) continue;
        g.name = d.Description;
        g.vendorId = d.VendorId;
        g.deviceId = d.DeviceId;
        g.dedicatedVideo = d.DedicatedVideoMemory;
        g.dedicatedSystem = d.DedicatedSystemMemory;
        g.sharedSystem = d.SharedSystemMemory;
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        g.vendor = d.VendorId == 0x10DE ? L"NVIDIA" : d.VendorId == 0x1002 ? L"AMD" : d.VendorId == 0x8086 ? L"Intel" :
                   d.VendorId == 0x1414 ? L"Microsoft" : d.VendorId == 0x5143 ? L"Qualcomm" : L"";
        if (g.software) continue;

        KmtAdapter ka(g.luid);
        if (ka.Ok()) {
            UINT ver = 0;
            if (ka.Query(KMTQAITYPE_DRIVERVERSION, ver) && ver) {
                g.wddm = std::to_wstring(ver / 1000) + L"." + std::to_wstring((ver % 1000) / 100);
                g.directX = ver >= 2000 ? L"DirectX 12" : ver >= 1200 ? L"DirectX 11" : L"DirectX 10";
            }
            UINT caps27 = 0;
            if (ka.Query(KMTQAITYPE_WDDM_2_7_CAPS, caps27)) {
                g.hagsSupported = (caps27 & 1) ? 1 : 0;
                g.hagsEnabled = (caps27 & 2) ? 1 : 0;
            }
            D3DKMT_ADAPTER_PERFDATACAPS pc{};
            if (ka.Query(KMTQAITYPE_ADAPTERPERFDATA_CAPS, pc)) {
                if (pc.MaxFanRPM) g.maxFanRpm = pc.MaxFanRPM;
                if (pc.TemperatureMax) g.tempMaxC = pc.TemperatureMax / 10.0;
                if (pc.TemperatureWarning) g.tempWarnC = pc.TemperatureWarning / 10.0;
                if (pc.MaxMemoryBandwidth) g.maxMemBandwidth = static_cast<double>(pc.MaxMemoryBandwidth);
                if (pc.MaxPCIEBandwidth) g.maxPcieBandwidth = static_cast<double>(pc.MaxPCIEBandwidth);
            }
        }
        out.push_back(std::move(g));
    }

    // Driver details and PCIe link from the device tree.
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClassGuid, nullptr, nullptr, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{sizeof dev};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            std::wstring hw = PropString(set, dev, kDevHardwareIds);
            unsigned ven = 0, devId = 0;
            size_t pv = hw.find(L"VEN_"), pd = hw.find(L"DEV_");
            if (pv != std::wstring::npos) ven = wcstoul(hw.c_str() + pv + 4, nullptr, 16);
            if (pd != std::wstring::npos) devId = wcstoul(hw.c_str() + pd + 4, nullptr, 16);
            for (auto& g : out) {
                if (g.vendorId != ven || g.deviceId != devId || !g.driverVersion.empty()) continue;
                g.driverVersion = PropString(set, dev, kDevDriverVersion);
                g.driverDate = PropString(set, dev, kDevDriverDate);
                g.location = PropString(set, dev, kDevLocationInfo);
                g.pnpId = PropString(set, dev, kDevInstanceId);
                uint32_t cs = 0, cw = 0, ms = 0, mw = 0;
                DEVPROPKEY k{kPciPropGuid, 9};
                PropU32(set, dev, k, cs);
                k.pid = 10; PropU32(set, dev, k, cw);
                k.pid = 7; PropU32(set, dev, k, ms);
                k.pid = 8; PropU32(set, dev, k, mw);
                g.pcieCurrent = PcieText(cs, cw);
                g.pcieMax = PcieText(ms, mw);
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    return out;
}

bool QueryGpuSensors(uint64_t luid, double& tempC, double& fanRpm, double& powerPct, double& memFreqMHz, double& coreFreqMHz, double& coreMaxMHz,
                     double& voltage, double& memBw, double& pcieBw) {
    tempC = fanRpm = powerPct = memFreqMHz = coreFreqMHz = coreMaxMHz = voltage = memBw = pcieBw = -1;
    KmtAdapter ka(luid);
    if (!ka.Ok()) return false;
    bool any = false;
    D3DKMT_ADAPTER_PERFDATA pd{};
    pd.PhysicalAdapterIndex = 0;
    if (ka.Query(KMTQAITYPE_ADAPTERPERFDATA, pd)) {
        any = true;
        if (pd.Temperature) tempC = pd.Temperature / 10.0;
        if (pd.FanRPM) fanRpm = pd.FanRPM;
        if (pd.Power) powerPct = pd.Power / 10.0;
        if (pd.MemoryFrequency) memFreqMHz = pd.MemoryFrequency / 1e6;
        memBw = static_cast<double>(pd.MemoryBandwidth);
        pcieBw = static_cast<double>(pd.PCIEBandwidth);
    }
    D3DKMT_NODE_PERFDATA nd{};
    nd.NodeOrdinal = 0; // node 0 is the 3D/graphics engine on all WDDM drivers we know of
    nd.PhysicalAdapterIndex = 0;
    if (ka.Query(KMTQAITYPE_NODEPERFDATA, nd)) {
        any = true;
        if (nd.Frequency) coreFreqMHz = nd.Frequency / 1e6;
        if (nd.MaxFrequency) coreMaxMHz = nd.MaxFrequency / 1e6;
        if (nd.Voltage) voltage = nd.Voltage / 1000.0;
    }
    return any;
}

} // namespace nx
