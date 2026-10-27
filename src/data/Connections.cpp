#include "data/Connections.h"
#include "core/Win.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstddef>
#include <cstring>

namespace nx {

uint64_t Connection::Key() const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    mix(localAddr.data(), localAddr.size() * sizeof(wchar_t));
    mix(remoteAddr.data(), remoteAddr.size() * sizeof(wchar_t));
    mix(&localPort, 2); mix(&remotePort, 2); mix(&pid, 4); mix(&tcp, 1);
    return h;
}

const wchar_t* TcpStateName(uint32_t s) {
    switch (s) {
    case MIB_TCP_STATE_CLOSED: return L"Closed";
    case MIB_TCP_STATE_LISTEN: return L"Listening";
    case MIB_TCP_STATE_SYN_SENT: return L"SYN sent";
    case MIB_TCP_STATE_SYN_RCVD: return L"SYN received";
    case MIB_TCP_STATE_ESTAB: return L"Established";
    case MIB_TCP_STATE_FIN_WAIT1: return L"FIN wait 1";
    case MIB_TCP_STATE_FIN_WAIT2: return L"FIN wait 2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"Close wait";
    case MIB_TCP_STATE_CLOSING: return L"Closing";
    case MIB_TCP_STATE_LAST_ACK: return L"Last ACK";
    case MIB_TCP_STATE_TIME_WAIT: return L"Time wait";
    case MIB_TCP_STATE_DELETE_TCB: return L"Delete TCB";
    default: return L"";
    }
}

namespace {
std::wstring Addr4(DWORD a) {
    wchar_t b[32];
    in_addr ia;
    ia.S_un.S_addr = a;
    InetNtopW(AF_INET, &ia, b, 32);
    return b;
}
std::wstring Addr6(const UCHAR* a) {
    wchar_t b[64];
    InetNtopW(AF_INET6, a, b, 64);
    return b;
}
uint16_t Port(DWORD p) { return ntohs(static_cast<u_short>(p & 0xFFFF)); }

template <typename F>
bool GetTable(F fn, std::vector<uint8_t>& buf) {
    DWORD size = static_cast<DWORD>(buf.size());
    for (int i = 0; i < 5; ++i) {
        DWORD r = fn(buf.empty() ? nullptr : buf.data(), &size);
        if (r == NO_ERROR) return true;
        if (r != ERROR_INSUFFICIENT_BUFFER) return false;
        buf.resize(size + 4096);
        size = static_cast<DWORD>(buf.size());
    }
    return false;
}
} // namespace

std::vector<Connection> QueryConnections() {
    std::vector<Connection> out;
    std::vector<uint8_t> buf(64 * 1024);
    if (GetTable([](void* p, DWORD* s) { return GetExtendedTcpTable(p, s, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0); }, buf)) {
        auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_MODULE*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            auto& r = t->table[i];
            Connection c;
            c.localAddr = Addr4(r.dwLocalAddr); c.localPort = Port(r.dwLocalPort);
            c.remoteAddr = Addr4(r.dwRemoteAddr); c.remotePort = Port(r.dwRemotePort);
            c.state = r.dwState; c.pid = r.dwOwningPid; c.createTime = static_cast<uint64_t>(r.liCreateTimestamp.QuadPart);
            out.push_back(std::move(c));
        }
    }
    if (GetTable([](void* p, DWORD* s) { return GetExtendedTcpTable(p, s, FALSE, AF_INET6, TCP_TABLE_OWNER_MODULE_ALL, 0); }, buf)) {
        auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_MODULE*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            auto& r = t->table[i];
            Connection c;
            c.v6 = true;
            c.localAddr = Addr6(r.ucLocalAddr); c.localPort = Port(r.dwLocalPort);
            c.remoteAddr = Addr6(r.ucRemoteAddr); c.remotePort = Port(r.dwRemotePort);
            c.state = r.dwState; c.pid = r.dwOwningPid; c.createTime = static_cast<uint64_t>(r.liCreateTimestamp.QuadPart);
            out.push_back(std::move(c));
        }
    }
    if (GetTable([](void* p, DWORD* s) { return GetExtendedUdpTable(p, s, FALSE, AF_INET, UDP_TABLE_OWNER_MODULE, 0); }, buf)) {
        auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_MODULE*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            auto& r = t->table[i];
            Connection c;
            c.tcp = false;
            c.localAddr = Addr4(r.dwLocalAddr); c.localPort = Port(r.dwLocalPort);
            c.pid = r.dwOwningPid; c.createTime = static_cast<uint64_t>(r.liCreateTimestamp.QuadPart);
            out.push_back(std::move(c));
        }
    }
    if (GetTable([](void* p, DWORD* s) { return GetExtendedUdpTable(p, s, FALSE, AF_INET6, UDP_TABLE_OWNER_MODULE, 0); }, buf)) {
        auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_MODULE*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            auto& r = t->table[i];
            Connection c;
            c.tcp = false;
            c.v6 = true;
            c.localAddr = Addr6(r.ucLocalAddr); c.localPort = Port(r.dwLocalPort);
            c.pid = r.dwOwningPid; c.createTime = static_cast<uint64_t>(r.liCreateTimestamp.QuadPart);
            out.push_back(std::move(c));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// ETW per-process network accounting
// ---------------------------------------------------------------------------------------------
namespace {
// Microsoft-Windows-Kernel-Network {7DD42A49-5329-4832-8DFD-43D979153A88}
const GUID kKernelNetwork = {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
const wchar_t kSessionName[] = L"NEXUS Network Activity";

struct Props {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[64];
};

void WINAPI EventCallback(PEVENT_RECORD rec) {
    auto* self = static_cast<NetTrace*>(rec->UserContext);
    if (self && IsEqualGUID(rec->EventHeader.ProviderId, kKernelNetwork))
        self->OnEvent(rec->EventHeader.EventDescriptor.Id, rec->UserData, rec->UserDataLength);
}
} // namespace

NetTrace::NetTrace() = default;
NetTrace::~NetTrace() { Stop(); }

void NetTrace::OnEvent(uint16_t id, const void* data, uint32_t size) {
    if (size < 8) return;
    uint32_t pid, bytes;
    std::memcpy(&pid, data, 4);
    std::memcpy(&bytes, static_cast<const uint8_t*>(data) + 4, 4);
    bool send;
    switch (id) {
    case 10: case 26: case 42: case 58: send = true; break;   // TCP/UDP send IPv4/IPv6
    case 11: case 27: case 43: case 59: send = false; break;  // TCP/UDP receive IPv4/IPv6
    default: return;
    }
    std::lock_guard<std::mutex> l(lock_);
    auto& e = bytes_[pid];
    (send ? e.first : e.second) += bytes;
}

void NetTrace::Drain(std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard<std::mutex> l(lock_);
    out.swap(bytes_);
    bytes_.clear();
}

bool NetTrace::Start() {
    Props pr{};
    pr.p.Wnode.BufferSize = sizeof pr;
    pr.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    pr.p.Wnode.ClientContext = 1; // QPC timestamps
    pr.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    pr.p.LoggerNameOffset = offsetof(Props, name);
    pr.p.BufferSize = 64;        // KB
    pr.p.MinimumBuffers = 4;
    pr.p.MaximumBuffers = 16;
    pr.p.FlushTimer = 1;
    TRACEHANDLE h = 0;
    ULONG r = StartTraceW(&h, kSessionName, &pr.p);
    if (r == ERROR_ALREADY_EXISTS) {
        // A previous NEXUS instance crashed without stopping its session; reclaim it.
        Props stop{};
        stop.p.Wnode.BufferSize = sizeof stop;
        stop.p.LoggerNameOffset = offsetof(Props, name);
        ControlTraceW(0, kSessionName, &stop.p, EVENT_TRACE_CONTROL_STOP);
        pr.p.Wnode.BufferSize = sizeof pr;
        r = StartTraceW(&h, kSessionName, &pr.p);
    }
    if (r != ERROR_SUCCESS) {
        error_ = L"Could not start network trace session: " + LastErrorMessage(r);
        return false;
    }
    session_ = h;
    r = EnableTraceEx2(h, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0x10 | 0x20, 0, 0, nullptr);
    if (r != ERROR_SUCCESS) {
        error_ = L"Could not enable kernel network provider: " + LastErrorMessage(r);
        Stop();
        return false;
    }
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<wchar_t*>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = EventCallback;
    lf.Context = this;
    TRACEHANDLE th = OpenTraceW(&lf);
    if (th == static_cast<TRACEHANDLE>(~0ULL)) { // INVALID_PROCESSTRACE_HANDLE
        error_ = L"Could not open network trace: " + LastErrorMessage();
        Stop();
        return false;
    }
    trace_ = th;
    active_ = true;
    thread_ = std::thread([th] {
        TRACEHANDLE t = th;
        ProcessTrace(&t, 1, nullptr, nullptr);
    });
    return true;
}

void NetTrace::Stop() {
    if (session_) {
        Props stop{};
        stop.p.Wnode.BufferSize = sizeof stop;
        stop.p.LoggerNameOffset = offsetof(Props, name);
        ControlTraceW(session_, nullptr, &stop.p, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (trace_) {
        CloseTrace(trace_);
        trace_ = 0;
    }
    if (thread_.joinable()) thread_.join();
    active_ = false;
}

} // namespace nx
