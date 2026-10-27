#include "core/Win.h"
#include "data/Connections.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <algorithm>
#include <mutex>

namespace nx {

namespace {
void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    });
}
} // namespace

DnsResult DnsLookup(const std::wstring& host) {
    EnsureWinsock();
    DnsResult r;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    ADDRINFOW* res = nullptr;
    uint64_t t0 = QpcNow();
    int e = GetAddrInfoW(host.c_str(), nullptr, &hints, &res);
    r.ms = QpcToSeconds(QpcNow() - t0) * 1000.0;
    if (e != 0) {
        r.error = LastErrorMessage(static_cast<DWORD>(e));
        return r;
    }
    for (auto* a = res; a; a = a->ai_next) {
        if (a->ai_canonname && r.canonicalName.empty()) r.canonicalName = a->ai_canonname;
        wchar_t buf[INET6_ADDRSTRLEN] = {};
        if (a->ai_family == AF_INET) InetNtopW(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, buf, INET6_ADDRSTRLEN);
        else if (a->ai_family == AF_INET6) InetNtopW(AF_INET6, &reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr, buf, INET6_ADDRSTRLEN);
        if (buf[0] && std::find(r.addresses.begin(), r.addresses.end(), buf) == r.addresses.end()) r.addresses.push_back(buf);
    }
    FreeAddrInfoW(res);
    return r;
}

std::wstring ReverseLookup(const std::wstring& address) {
    EnsureWinsock();
    sockaddr_storage ss{};
    int len = 0;
    if (InetPtonW(AF_INET, address.c_str(), &reinterpret_cast<sockaddr_in*>(&ss)->sin_addr) == 1) {
        ss.ss_family = AF_INET;
        len = sizeof(sockaddr_in);
    } else if (InetPtonW(AF_INET6, address.c_str(), &reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr) == 1) {
        ss.ss_family = AF_INET6;
        len = sizeof(sockaddr_in6);
    } else {
        return {};
    }
    wchar_t host[NI_MAXHOST];
    if (GetNameInfoW(reinterpret_cast<sockaddr*>(&ss), len, host, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) == 0) return host;
    return {};
}

PingResult Ping(const std::wstring& host, int count, int timeoutMs) {
    EnsureWinsock();
    PingResult r;
    DnsResult d = DnsLookup(host);
    if (d.addresses.empty()) {
        r.error = d.error.empty() ? L"Host not found" : d.error;
        return r;
    }
    r.address = d.addresses.front();
    in_addr a4{};
    sockaddr_in6 a6{};
    bool v6 = false;
    if (InetPtonW(AF_INET, r.address.c_str(), &a4) != 1) {
        if (InetPtonW(AF_INET6, r.address.c_str(), &a6.sin6_addr) != 1) { r.error = L"Unsupported address"; return r; }
        a6.sin6_family = AF_INET6;
        v6 = true;
    }
    HANDLE h = v6 ? Icmp6CreateFile() : IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) { r.error = LastErrorMessage(); return r; }
    char payload[32] = "NEXUS latency probe............";
    std::vector<uint8_t> reply(sizeof(ICMPV6_ECHO_REPLY) + sizeof(ICMP_ECHO_REPLY) + sizeof payload + 64);
    double total = 0;
    r.minMs = 1e9;
    for (int i = 0; i < count; ++i) {
        ++r.sent;
        uint64_t t0 = QpcNow();
        DWORD n;
        if (v6) {
            sockaddr_in6 src{};
            src.sin6_family = AF_INET6;
            n = Icmp6SendEcho2(h, nullptr, nullptr, nullptr, &src, &a6, payload, sizeof payload, nullptr, reply.data(),
                               static_cast<DWORD>(reply.size()), static_cast<DWORD>(timeoutMs));
        } else {
            n = IcmpSendEcho(h, a4.S_un.S_addr, payload, sizeof payload, nullptr, reply.data(), static_cast<DWORD>(reply.size()),
                             static_cast<DWORD>(timeoutMs));
        }
        double ms = QpcToSeconds(QpcNow() - t0) * 1000.0;
        bool ok = n > 0;
        if (ok && !v6) {
            auto* er = reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data());
            ok = er->Status == IP_SUCCESS;
            if (ok && er->RoundTripTime > 0) ms = er->RoundTripTime; // kernel-measured when >= 1 ms
        } else if (ok) {
            auto* er = reinterpret_cast<ICMPV6_ECHO_REPLY*>(reply.data());
            ok = er->Status == IP_SUCCESS;
            if (ok && er->RoundTripTime > 0) ms = er->RoundTripTime;
        }
        if (ok) {
            ++r.received;
            total += ms;
            r.minMs = std::min(r.minMs, ms);
            r.maxMs = std::max(r.maxMs, ms);
        }
        if (i + 1 < count) Sleep(250);
    }
    IcmpCloseHandle(h);
    if (r.received) r.avgMs = total / r.received;
    else { r.minMs = 0; r.error = L"No replies (host unreachable or ICMP blocked)"; }
    return r;
}

} // namespace nx
