#include "core/Win.h"
#include "data/SystemData.h"

#include <winevt.h>

#include <unordered_map>

namespace nx {

namespace {
struct EvtH {
    EVT_HANDLE h = nullptr;
    EvtH() = default;
    explicit EvtH(EVT_HANDLE x) : h(x) {}
    ~EvtH() { if (h) EvtClose(h); }
    EvtH(const EvtH&) = delete;
    EvtH& operator=(const EvtH&) = delete;
};

// The next batch of a query's results; false at the end (ERROR_NO_MORE_ITEMS) or on failure. On a slow
// disk the service can take longer than the timeout to reach the next match. A call that times out
// leaves the query where it got to, so only a minute without progress ends it (ERROR_TIMEOUT).
bool NextBatch(EVT_HANDLE q, EVT_HANDLE* evs, DWORD size, DWORD& got) {
    for (int waits = 0; waits < 30; ++waits) {
        got = 0;
        if (EvtNext(q, size, evs, 2000, 0, &got)) {
            if (got) return true;
            SetLastError(ERROR_NO_MORE_ITEMS);
            return false;
        }
        if (GetLastError() != ERROR_TIMEOUT) return false;
    }
    return false;
}

// Provider, ID, level and time of an event, rendered into e with a context from SystemContext().
EVT_HANDLE SystemContext() {
    const wchar_t* paths[] = {L"Event/System/Provider/@Name", L"Event/System/EventID", L"Event/System/Level", L"Event/System/TimeCreated/@SystemTime"};
    return EvtCreateRenderContext(4, paths, EvtRenderContextValues);
}

bool RenderSystem(EVT_HANDLE ctx, EVT_HANDLE ev, std::vector<uint8_t>& buf, EventEntry& e) {
    DWORD used = 0, props = 0;
    if (!EvtRender(ctx, ev, EvtRenderEventValues, static_cast<DWORD>(buf.size()), buf.data(), &used, &props)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
        buf.resize(used);
        if (!EvtRender(ctx, ev, EvtRenderEventValues, static_cast<DWORD>(buf.size()), buf.data(), &used, &props)) return false;
    }
    auto* v = reinterpret_cast<EVT_VARIANT*>(buf.data());
    e.provider = v[0].Type == EvtVarTypeString && v[0].StringVal ? v[0].StringVal : L"";
    e.id = v[1].Type == EvtVarTypeUInt16 ? v[1].UInt16Val : 0;
    e.level = v[2].Type == EvtVarTypeByte ? v[2].ByteVal : 0;
    e.time = v[3].Type == EvtVarTypeFileTime ? v[3].FileTimeVal : 0;
    return true;
}

std::wstring FormatMsg(EVT_HANDLE publisher, EVT_HANDLE ev) {
    DWORD used = 0;
    EvtFormatMessage(publisher, ev, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
    if (!used || used > 65536) return {};
    std::wstring s(used, L'\0');
    if (!EvtFormatMessage(publisher, ev, 0, 0, nullptr, EvtFormatMessageEvent, used, s.data(), &used)) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s;
}

std::wstring VariantText(const EVT_VARIANT& v) {
    switch (v.Type & EVT_VARIANT_TYPE_MASK) {
    case EvtVarTypeString: return v.StringVal ? v.StringVal : L"";
    case EvtVarTypeAnsiString: {
        if (!v.AnsiStringVal) return {};
        std::string a = v.AnsiStringVal;
        return std::wstring(a.begin(), a.end());
    }
    case EvtVarTypeByte: return std::to_wstring(v.ByteVal);
    case EvtVarTypeUInt16: return std::to_wstring(v.UInt16Val);
    case EvtVarTypeUInt32: return std::to_wstring(v.UInt32Val);
    case EvtVarTypeUInt64: return std::to_wstring(v.UInt64Val);
    case EvtVarTypeInt32: return std::to_wstring(v.Int32Val);
    case EvtVarTypeInt64: return std::to_wstring(v.Int64Val);
    case EvtVarTypeHexInt32: { wchar_t b[16]; swprintf(b, 16, L"0x%08x", v.UInt32Val); return b; }
    case EvtVarTypeHexInt64: { wchar_t b[24]; swprintf(b, 24, L"0x%016llx", static_cast<unsigned long long>(v.UInt64Val)); return b; }
    case EvtVarTypeBoolean: return v.BooleanVal ? L"true" : L"false";
    default: return {};
    }
}
} // namespace

const wchar_t* EventLevelName(uint8_t l) {
    switch (l) {
    case 1: return L"Critical";
    case 2: return L"Error";
    case 3: return L"Warning";
    case 4: case 0: return L"Information";
    case 5: return L"Verbose";
    default: return L"";
    }
}

std::vector<EventEntry> QueryEvents(const wchar_t* channel, const wchar_t* xpath, size_t max, bool formatMessage) {
    std::vector<EventEntry> out;
    EvtH q(EvtQuery(nullptr, channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection));
    if (!q.h) return out;
    EvtH sysCtx(SystemContext());
    EvtH userCtx(EvtCreateRenderContext(0, nullptr, EvtRenderContextUser));
    std::unordered_map<std::wstring, EVT_HANDLE> publishers;
    std::vector<uint8_t> buf(8192);
    EVT_HANDLE evs[32];
    DWORD got = 0;
    while (out.size() < max && NextBatch(q.h, evs, 32, got)) {
        for (DWORD i = 0; i < got; ++i) {
            EvtH ev(evs[i]);
            if (out.size() >= max) continue;
            EventEntry e;
            e.channel = channel;
            if (!RenderSystem(sysCtx.h, ev.h, buf, e)) continue;
            // EventData values (positional).
            DWORD used = 0, props = 0;
            std::vector<uint8_t> ub(4096);
            if (!EvtRender(userCtx.h, ev.h, EvtRenderEventValues, static_cast<DWORD>(ub.size()), ub.data(), &used, &props) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER && used < (1 << 20)) {
                ub.resize(used);
                if (!EvtRender(userCtx.h, ev.h, EvtRenderEventValues, static_cast<DWORD>(ub.size()), ub.data(), &used, &props)) props = 0;
            }
            auto* uv = reinterpret_cast<EVT_VARIANT*>(ub.data());
            for (DWORD k = 0; k < props && k < 32; ++k) e.data.push_back(VariantText(uv[k]));
            if (formatMessage && !e.provider.empty()) {
                auto it = publishers.find(e.provider);
                if (it == publishers.end()) it = publishers.emplace(e.provider, EvtOpenPublisherMetadata(nullptr, e.provider.c_str(), nullptr, 0, 0)).first;
                if (it->second) e.message = FormatMsg(it->second, ev.h);
            }
            out.push_back(std::move(e));
        }
    }
    for (auto& [n, h] : publishers)
        if (h) EvtClose(h);
    return out;
}

bool ScanEvents(const wchar_t* channel, const wchar_t* xpath, const std::function<bool(const EventEntry&)>& fn) {
    EvtH ctx(SystemContext());
    EvtH q(ctx.h ? EvtQuery(nullptr, channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection) : nullptr);
    if (!q.h) return false;
    std::vector<uint8_t> buf(8192);
    EventEntry e;
    e.channel = channel;
    EVT_HANDLE evs[64];
    DWORD got = 0;
    for (;;) {
        if (!NextBatch(q.h, evs, 64, got)) return GetLastError() == ERROR_NO_MORE_ITEMS;
        bool more = true;
        for (DWORD i = 0; i < got; ++i) {
            EvtH ev(evs[i]);
            if (more && RenderSystem(ctx.h, ev.h, buf, e)) more = fn(e);
        }
        if (!more) return true;
    }
}

} // namespace nx
