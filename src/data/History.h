#pragma once
// Optional on-disk performance recording (off by default) and CSV/JSON export.
#include "data/Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nx {

struct HistoryPoint {
    uint64_t time = 0; // FILETIME UTC
    float cpu = 0, memPct = 0, commitPct = 0, gpu = 0;
    double memUsed = 0, diskRead = 0, diskWrite = 0, netSend = 0, netRecv = 0;
    float diskActiveMax = 0;
    uint32_t processes = 0;
};

HistoryPoint MakeHistoryPoint(const Snapshot& s);

class HistoryRecorder {
public:
    ~HistoryRecorder();
    // folder empty = %LOCALAPPDATA%\NEXUS\History
    void Configure(bool enabled, const std::wstring& folder, int retentionDays, int everyNSamples);
    bool Enabled() const { return enabled_; }
    const std::wstring& Folder() const { return folder_; }
    void Add(const HistoryPoint& p);
    void Flush();

private:
    void Prune();
    bool enabled_ = false;
    std::wstring folder_;
    int retentionDays_ = 7, every_ = 5, counter_ = 0;
    std::string pending_;
    uint64_t lastFlush_ = 0;
};

bool ExportHistoryCsv(const std::wstring& path, const std::vector<HistoryPoint>& points, std::wstring& error);
bool ExportHistoryJson(const std::wstring& path, const std::vector<HistoryPoint>& points, std::wstring& error);
bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::wstring& error);
std::string HistoryCsvHeader();
std::string HistoryCsvLine(const HistoryPoint& p);

} // namespace nx
