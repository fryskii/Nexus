#include "data/History.h"
#include "core/Format.h"
#include "core/SafeFile.h"
#include "core/Win.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

namespace nx {

HistoryPoint MakeHistoryPoint(const Snapshot& s) {
    HistoryPoint p;
    p.time = s.time;
    p.cpu = s.cpu.total;
    p.memPct = s.mem.total ? 100.f * static_cast<float>(s.mem.InUse()) / static_cast<float>(s.mem.total) : 0.f;
    p.commitPct = s.mem.commitLimit ? 100.f * static_cast<float>(s.mem.commit) / static_cast<float>(s.mem.commitLimit) : 0.f;
    p.memUsed = static_cast<double>(s.mem.InUse());
    for (auto& d : s.disks) {
        p.diskRead += d.readBps;
        p.diskWrite += d.writeBps;
        p.diskActiveMax = std::max(p.diskActiveMax, d.activePct);
    }
    for (auto& n : s.nics) {
        p.netSend += n.sendBps;
        p.netRecv += n.recvBps;
    }
    for (auto& g : s.gpus) p.gpu = std::max(p.gpu, g.util);
    p.processes = s.cpu.processes;
    return p;
}

namespace {
std::string IsoTime(uint64_t ft) {
    std::wstring w = FormatFileTime(ft);
    std::string s = ToUtf8(w);
    if (s.size() > 10) s[10] = 'T';
    return s + "Z";
}
} // namespace

std::string HistoryCsvHeader() {
    return "time_utc,cpu_pct,memory_pct,memory_used_bytes,commit_pct,disk_read_Bps,disk_write_Bps,disk_active_max_pct,net_send_Bps,net_recv_Bps,gpu_pct,processes\n";
}

std::string HistoryCsvLine(const HistoryPoint& p) {
    char b[512];
    std::snprintf(b, sizeof b, "%s,%.2f,%.2f,%.0f,%.2f,%.0f,%.0f,%.1f,%.0f,%.0f,%.2f,%u\n", IsoTime(p.time).c_str(), p.cpu, p.memPct, p.memUsed,
                  p.commitPct, p.diskRead, p.diskWrite, p.diskActiveMax, p.netSend, p.netRecv, p.gpu, p.processes);
    return b;
}

std::recursive_mutex& DiskWriteLock() {
    static auto* m = new std::recursive_mutex; // never destroyed: a background writer may still hold it while NEXUS exits
    return *m;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::wstring& error) {
    // Write a new temporary file and rename it over the target: an existing file (which could be a
    // link planted by another program) is never opened for writing. See SafeFile.h.
    SafeWriteGuard guard;
    if (!guard.Prepare(path, &error)) return false;
    std::wstring tmp = path + L".tmp";
    DeleteFileW(tmp.c_str()); // leftover from an interrupted save; removes a link itself, never its target
    HANDLE h = guard.Open(tmp, GENERIC_WRITE, CREATE_NEW, &error);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { error = LastErrorMessage(); DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = LastErrorMessage();
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool ExportHistoryCsv(const std::wstring& path, const std::vector<HistoryPoint>& pts, std::wstring& error) {
    std::string s = HistoryCsvHeader();
    s.reserve(pts.size() * 120 + s.size());
    for (auto& p : pts) s += HistoryCsvLine(p);
    return WriteFileAtomic(path, s, error);
}

bool ExportHistoryJson(const std::wstring& path, const std::vector<HistoryPoint>& pts, std::wstring& error) {
    std::string s = "{\n  \"generator\": \"NEXUS " NEXUS_VERSION_STR "\",\n  \"samples\": [\n";
    char b[640];
    for (size_t i = 0; i < pts.size(); ++i) {
        auto& p = pts[i];
        std::snprintf(b, sizeof b,
                      "    {\"time\":\"%s\",\"cpu\":%.2f,\"memoryPct\":%.2f,\"memoryUsed\":%.0f,\"commitPct\":%.2f,\"diskRead\":%.0f,\"diskWrite\":%.0f,"
                      "\"diskActiveMax\":%.1f,\"netSend\":%.0f,\"netRecv\":%.0f,\"gpu\":%.2f,\"processes\":%u}%s\n",
                      IsoTime(p.time).c_str(), p.cpu, p.memPct, p.memUsed, p.commitPct, p.diskRead, p.diskWrite, p.diskActiveMax, p.netSend, p.netRecv,
                      p.gpu, p.processes, i + 1 < pts.size() ? "," : "");
        s += b;
    }
    s += "  ]\n}\n";
    return WriteFileAtomic(path, s, error);
}

HistoryRecorder::~HistoryRecorder() { Flush(); }

void HistoryRecorder::Configure(bool enabled, const std::wstring& folder, int retentionDays, int every) {
    if (enabled_ && !enabled) Flush();
    enabled_ = enabled;
    retentionDays_ = std::max(1, retentionDays);
    every_ = std::max(1, every);
    folder_ = folder;
    // Elevated, a folder chosen in the (user-editable) settings file must be in the user's profile,
    // so an edited setting cannot make an administrator write or prune files in system folders.
    if (!folder_.empty() && IsElevated() && !IsInsideUserProfile(folder_)) folder_.clear();
    if (folder_.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            folder_ = std::wstring(p) + L"\\NEXUS\\History";
            CoTaskMemFree(p);
        }
    }
    if (enabled_) {
        SHCreateDirectoryExW(nullptr, folder_.c_str(), nullptr);
        Prune();
    }
}

void HistoryRecorder::Add(const HistoryPoint& p) {
    if (!enabled_) return;
    if (++counter_ % every_) return;
    pending_ += HistoryCsvLine(p);
}

HistoryRecorder::Chunk HistoryRecorder::TakePending() {
    Chunk c;
    if (!enabled_ || pending_.empty() || folder_.empty()) return c;
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\nexus-%04u-%02u-%02u.csv", st.wYear, st.wMonth, st.wDay);
    c.path = folder_ + name;
    c.data.swap(pending_);
    return c;
}

void HistoryRecorder::Append(const Chunk& c) {
    if (c.data.empty()) return;
    SafeWriteGuard guard;
    if (!guard.Prepare(c.path)) return;
    HANDLE h = guard.Open(c.path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    bool exists = GetFileSizeEx(h, &size) && size.QuadPart > 0;
    std::string data = exists ? c.data : HistoryCsvHeader() + c.data;
    DWORD w = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
    CloseHandle(h);
}

void HistoryRecorder::Flush() {
    Chunk c = TakePending();
    std::lock_guard<std::recursive_mutex> l(DiskWriteLock());
    Append(c);
}

void HistoryRecorder::Prune() {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((folder_ + L"\\nexus-*.csv").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    uint64_t cutoff = NowFileTime() - static_cast<uint64_t>(retentionDays_) * 86400ULL * 10000000ULL;
    do {
        if (FileTimeToU64(fd.ftLastWriteTime) < cutoff) DeleteFileW((folder_ + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

} // namespace nx
