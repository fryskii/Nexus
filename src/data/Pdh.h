#pragma once
// Thin RAII wrapper over the Performance Data Helper (PDH) API. Counter paths are added with
// PdhAddEnglishCounter so they work on every display language.
#include <windows.h>
#include <pdh.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nx {

class PdhQuery {
public:
    PdhQuery();
    ~PdhQuery();
    PdhQuery(const PdhQuery&) = delete;
    PdhQuery& operator=(const PdhQuery&) = delete;

    // Returns an index usable with Value()/Array(), or -1 if the counter doesn't exist on this system.
    int Add(const wchar_t* englishPath);
    bool Collect();
    bool Ok() const { return query_ != nullptr; }

    bool Value(int idx, double& out, DWORD fmt = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100) const;
    struct Item { std::wstring name; double value; };
    // Reuses the caller's vector to minimise allocations.
    bool Array(int idx, std::vector<Item>& out, DWORD fmt = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100) const;
    bool LargeArray(int idx, std::vector<std::pair<std::wstring, int64_t>>& out) const;

private:
    PDH_HQUERY query_ = nullptr;
    std::vector<PDH_HCOUNTER> counters_;
    mutable std::vector<uint8_t> buf_;
};

} // namespace nx
