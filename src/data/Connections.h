#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nx {

struct Connection {
    bool tcp = true, v6 = false;
    std::wstring localAddr, remoteAddr;
    uint16_t localPort = 0, remotePort = 0;
    uint32_t state = 0;      // MIB_TCP_STATE (TCP only)
    uint32_t pid = 0;
    uint64_t createTime = 0; // FILETIME
    uint64_t Key() const;
};

std::vector<Connection> QueryConnections();
const wchar_t* TcpStateName(uint32_t state);

// Per-process network byte accounting from the Microsoft-Windows-Kernel-Network ETW provider.
// Requires administrator rights. Counts TCP and UDP payload bytes per process.
class NetTrace {
public:
    NetTrace();
    ~NetTrace();
    bool Start();
    void Stop();
    bool Active() const { return active_; }
    const std::wstring& Error() const { return error_; }
    // Returns bytes (sent, received) per PID since the last call and resets the counters.
    void Drain(std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& out);

    void OnEvent(uint16_t id, const void* data, uint32_t size);

private:
    uint64_t session_ = 0, trace_ = 0;
    std::thread thread_;
    std::atomic<bool> active_{false};
    std::wstring error_;
    std::mutex lock_;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> bytes_;
};

// Network diagnostics.
struct PingResult { int sent = 0, received = 0; double minMs = 0, avgMs = 0, maxMs = 0; std::wstring address, error; };
PingResult Ping(const std::wstring& host, int count, int timeoutMs);
struct DnsResult { std::vector<std::wstring> addresses; double ms = 0; std::wstring error, canonicalName; };
DnsResult DnsLookup(const std::wstring& host);
std::wstring ReverseLookup(const std::wstring& address);

} // namespace nx
