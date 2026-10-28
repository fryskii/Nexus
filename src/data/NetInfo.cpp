#include "core/Format.h"
#include "core/Win.h"
#include "data/Hardware.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <wlanapi.h>

#include <map>

namespace nx {

namespace {

std::wstring SockaddrText(const SOCKADDR* sa) {
    wchar_t buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET) InetNtopW(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, INET6_ADDRSTRLEN);
    else if (sa->sa_family == AF_INET6) InetNtopW(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, INET6_ADDRSTRLEN);
    return buf;
}

// Wi-Fi SSIDs via the WLAN API, loaded only if present.
std::map<std::wstring, std::wstring> QueryWifiSsids() {
    std::map<std::wstring, std::wstring> out; // adapter GUID string -> SSID
    HMODULE m = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return out;
    using Open_t = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
    using Close_t = DWORD(WINAPI*)(HANDLE, PVOID);
    using Enum_t = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
    using Query_t = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
    using Free_t = VOID(WINAPI*)(PVOID);
    auto open = reinterpret_cast<Open_t>(GetProcAddress(m, "WlanOpenHandle"));
    auto close = reinterpret_cast<Close_t>(GetProcAddress(m, "WlanCloseHandle"));
    auto enumIf = reinterpret_cast<Enum_t>(GetProcAddress(m, "WlanEnumInterfaces"));
    auto query = reinterpret_cast<Query_t>(GetProcAddress(m, "WlanQueryInterface"));
    auto freeMem = reinterpret_cast<Free_t>(GetProcAddress(m, "WlanFreeMemory"));
    if (open && close && enumIf && query && freeMem) {
        DWORD ver = 0;
        HANDLE h = nullptr;
        if (open(2, nullptr, &ver, &h) == ERROR_SUCCESS) {
            PWLAN_INTERFACE_INFO_LIST list = nullptr;
            if (enumIf(h, nullptr, &list) == ERROR_SUCCESS && list) {
                for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                    auto& itf = list->InterfaceInfo[i];
                    if (itf.isState != wlan_interface_state_connected) continue;
                    DWORD size = 0;
                    PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
                    if (query(h, &itf.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<PVOID*>(&ca), nullptr) ==
                            ERROR_SUCCESS && ca) {
                        auto& ssid = ca->wlanAssociationAttributes.dot11Ssid;
                        std::string s(reinterpret_cast<const char*>(ssid.ucSSID), std::min<ULONG>(ssid.uSSIDLength, 32));
                        wchar_t g[64];
                        StringFromGUID2(itf.InterfaceGuid, g, 64);
                        out[ToLower(g)] = FromUtf8(s) + L" (signal " + std::to_wstring(ca->wlanAssociationAttributes.wlanSignalQuality) + L"%)";
                        freeMem(ca);
                    }
                }
                freeMem(list);
            }
            close(h, nullptr);
        }
    }
    FreeLibrary(m);
    return out;
}

} // namespace

std::vector<NicStatic> QueryNics() {
    std::vector<NicStatic> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG r;
    for (int i = 0; i < 4; ++i) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (r != ERROR_BUFFER_OVERFLOW) break;
    }
    if (r != NO_ERROR) return out;
    auto ssids = QueryWifiSsids();
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        NicStatic n;
        n.luid = a->Luid.Value;
        n.ifIndex = a->IfIndex;
        n.alias = a->FriendlyName ? a->FriendlyName : L"";
        n.description = a->Description ? a->Description : L"";
        n.dnsSuffix = a->DnsSuffix ? a->DnsSuffix : L"";
        n.adapterName = FromUtf8(a->AdapterName ? a->AdapterName : "");
        n.ifType = a->IfType;
        n.wireless = a->IfType == IF_TYPE_IEEE80211;
        n.up = a->OperStatus == IfOperStatusUp;
        n.mtu = a->Mtu;
        n.dhcp = (a->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        n.speedTx = a->TransmitLinkSpeed;
        n.speedRx = a->ReceiveLinkSpeed;
        n.connectionType = n.wireless ? L"Wi-Fi" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? L"Ethernet" : a->IfType == IF_TYPE_PPP ? L"PPP" :
                           a->IfType == 243 || a->IfType == 244 /*WWANPP, WWANPP2*/ ? L"Cellular" : L"Other";
        wchar_t mac[64] = {};
        for (ULONG i = 0; i < a->PhysicalAddressLength && i < 8; ++i)
            swprintf(mac + wcslen(mac), 64 - wcslen(mac), i ? L"-%02X" : L"%02X", a->PhysicalAddress[i]);
        n.mac = mac;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            std::wstring t = SockaddrText(u->Address.lpSockaddr);
            if (u->Address.lpSockaddr->sa_family == AF_INET) n.ipv4.push_back(t + L"/" + std::to_wstring(u->OnLinkPrefixLength));
            else n.ipv6.push_back(t);
        }
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) n.gateways.push_back(SockaddrText(g->Address.lpSockaddr));
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) n.dns.push_back(SockaddrText(d->Address.lpSockaddr));
        if (n.wireless) {
            auto it = ssids.find(ToLower(n.adapterName));
            if (it != ssids.end()) n.ssid = it->second;
        }
        out.push_back(std::move(n));
    }
    return out;
}

} // namespace nx
