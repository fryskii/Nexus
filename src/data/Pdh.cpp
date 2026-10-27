#include "data/Pdh.h"

#include <pdhmsg.h>

namespace nx {

PdhQuery::PdhQuery() {
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) query_ = nullptr;
}

PdhQuery::~PdhQuery() {
    if (query_) PdhCloseQuery(query_);
}

int PdhQuery::Add(const wchar_t* path) {
    if (!query_) return -1;
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(query_, path, 0, &c) != ERROR_SUCCESS) return -1;
    counters_.push_back(c);
    return static_cast<int>(counters_.size() - 1);
}

bool PdhQuery::Collect() {
    return query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS;
}

bool PdhQuery::Value(int idx, double& out, DWORD fmt) const {
    if (idx < 0 || idx >= static_cast<int>(counters_.size())) return false;
    PDH_FMT_COUNTERVALUE v{};
    if (PdhGetFormattedCounterValue(counters_[idx], fmt, nullptr, &v) != ERROR_SUCCESS) return false;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return false;
    out = (fmt & PDH_FMT_LARGE) ? static_cast<double>(v.largeValue) : v.doubleValue;
    return true;
}

bool PdhQuery::Array(int idx, std::vector<Item>& out, DWORD fmt) const {
    out.clear();
    if (idx < 0 || idx >= static_cast<int>(counters_.size())) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(counters_[idx], fmt, &size, &count, nullptr);
    if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return false;
    if (buf_.size() < size) buf_.resize(size);
    s = PdhGetFormattedCounterArrayW(counters_[idx], fmt, &size, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data()));
    if (s != ERROR_SUCCESS) return false;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        out.push_back({items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue});
    }
    return true;
}

bool PdhQuery::LargeArray(int idx, std::vector<std::pair<std::wstring, int64_t>>& out) const {
    out.clear();
    if (idx < 0 || idx >= static_cast<int>(counters_.size())) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(counters_[idx], PDH_FMT_LARGE, &size, &count, nullptr);
    if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return false;
    if (buf_.size() < size) buf_.resize(size);
    s = PdhGetFormattedCounterArrayW(counters_[idx], PDH_FMT_LARGE, &size, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data()));
    if (s != ERROR_SUCCESS) return false;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        out.emplace_back(items[i].szName ? items[i].szName : L"", items[i].FmtValue.largeValue);
    }
    return true;
}

} // namespace nx
