// libFuzzer harness for the platform-neutral core: SMBIOS parsing, the process filter language,
// settings files, size parsing, number formatting and the diagnostics rules. Build on a Linux or
// macOS host with clang (see tests/fuzz/README.md); the first input byte picks the target.
#include "core/DiagnosticsRules.h"
#include "core/Filter.h"
#include "core/Regex.h"
#include "core/Format.h"
#include "core/Settings.h"
#include "core/Smbios.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

using namespace nx;

namespace {

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    size_t Left() const { return n_ - i_; }
    uint8_t U8() { return i_ < n_ ? d_[i_++] : 0; }
    uint32_t U32() { uint32_t v = 0; for (int k = 0; k < 4; ++k) v |= uint32_t(U8()) << (8 * k); return v; }
    uint64_t U64() { return uint64_t(U32()) | (uint64_t(U32()) << 32); }
    double F64() { uint64_t b = U64(); double v; std::memcpy(&v, &b, sizeof v); return v; } // includes NaN/Inf
    float F32() { uint32_t b = U32(); float v; std::memcpy(&v, &b, sizeof v); return v; }
    std::wstring WStr(size_t maxLen) {
        size_t len = std::min<size_t>(U8() % (maxLen + 1), Left() / 2);
        std::wstring s;
        for (size_t k = 0; k < len; ++k) s.push_back(static_cast<wchar_t>(U8() | (U8() << 8)));
        return s;
    }
    std::wstring Rest() {
        std::wstring s;
        while (Left() >= 2) s.push_back(static_cast<wchar_t>(U8() | (U8() << 8)));
        return s;
    }
    const uint8_t* Ptr() const { return d_ + i_; }

private:
    const uint8_t* d_;
    size_t n_, i_ = 0;
};

void FuzzFilter(Reader& r) {
    // A query followed by a few subjects to match against.
    std::wstring q = r.WStr(200);
    ProcessFilter f;
    std::wstring err;
    f.Compile(q, &err);
    for (int k = 0; k < 4 && r.Left(); ++k) {
        std::wstring name = r.WStr(40), desc = r.WStr(20), pub = r.WStr(20), path = r.WStr(40), user = r.WStr(10), cmd = r.WStr(60);
        FilterSubject s;
        s.pid = r.U32(); s.ppid = r.U32();
        s.name = &name; s.description = (k & 1) ? &desc : nullptr; s.publisher = &pub; s.path = (k & 2) ? nullptr : &path;
        s.user = &user; s.commandLine = &cmd;
        s.cpu = r.F64(); s.gpu = r.F64(); s.ioRate = r.F64(); s.memory = r.U64(); s.threads = r.U32(); s.handles = r.U32();
        f.Matches(s);
    }
}

void FuzzSettings(const uint8_t* d, size_t n) {
    Settings s;
    s.Parse(std::string(reinterpret_cast<const char*>(d), n));
    std::string out = s.Serialize();
    Settings t;
    t.Parse(out);
    if (t.Serialize() != out) __builtin_trap(); // serialization must round-trip
    s.GetString("ui.theme"); s.GetInt("window.w"); s.GetDouble("ui.scale"); s.GetBool("ui.insights"); s.GetList("nav.hidden");
}

void FuzzFormat(Reader& r) {
    double v = r.F64();
    uint64_t u = r.U64();
    FormatBytes(u, r.U8() % 4);
    FormatMB(u);
    FormatRateBytes(v);
    FormatRateBits(v);
    FormatNetRate(v, static_cast<RateUnit>(r.U8() % 2));
    FormatPercent(v, r.U8() % 4);
    FormatFreqMHz(v);
    FormatTemp(v, static_cast<TempUnit>(r.U8() % 2));
    FormatUptime(u);
    FormatDuration100ns(u);
    FormatCount(u);
    FormatHex(u, r.U8() % 20);
    FormatLatencyMs(v);
    std::wstring s = r.Rest();
    uint64_t size = 0;
    ParseSize(s, size);
    Trim(s);
    ToLower(s);
    ContainsI(s, s.substr(0, s.size() / 2));
    std::string u8 = ToUtf8(s);
    FromUtf8(u8);
}

void FuzzDiagnostics(Reader& r) {
    DiagInput in;
    in.intervalSec = r.F64();
    in.logicalCpus = r.U8();
    auto series = [&](std::vector<float>& v) { size_t n = r.U8() % 64; for (size_t k = 0; k < n && r.Left(); ++k) v.push_back(r.F32()); };
    for (int p = r.U8() % 6; p > 0; --p) {
        DiagInput::Proc pr;
        pr.pid = r.U32();
        pr.name = r.WStr(16);
        series(pr.cpu);
        series(pr.privateMB);
        in.procs.push_back(std::move(pr));
    }
    series(in.totalCpu); series(in.commitPct); series(in.availPct); series(in.hardFaults);
    for (int k = r.U8() % 3; k > 0; --k) { DiagInput::Disk dk; dk.name = r.WStr(8); series(dk.latencyMs); series(dk.activePct); in.disks.push_back(dk); }
    for (int k = r.U8() % 3; k > 0; --k) in.adapters.push_back({r.WStr(8), r.U64(), r.U64(), r.U64()});
    for (int k = r.U8() % 3; k > 0; --k) in.volumes.push_back({r.WStr(8), r.U64(), r.U64()});
    for (int k = r.U8() % 3; k > 0; --k) {
        DiagInput::Health h;
        h.device = r.WStr(8); h.criticalWarning = r.U8(); h.percentUsed = int(r.U32()); h.availableSpare = int(r.U32());
        h.spareThreshold = int(r.U32()); h.tempC = r.F64();
        in.storageHealth.push_back(h);
    }
    in.maxThermalZoneC = r.F64();
    for (int k = r.U8() % 3; k > 0; --k) in.gpuTempsC.push_back(r.F64());
    in.appCrashes24h = int(r.U32()); in.unexpectedShutdowns30d = int(r.U32()); in.whea30d = int(r.U32());
    EvaluateDiagnostics(in);
}

// Differential check of SafeRegex against std::regex (ECMAScript, icase) on short ASCII inputs,
// where std::regex cannot blow up. Patterns either engine rejects are skipped.
void FuzzRegexDiff(Reader& r) {
    static const char alphabet[] = "ab.*+?|()[]^$-\\dwsbDWSB{}0123,AZx :=\n";
    auto pick = [&](size_t maxLen, const char* chars, size_t nchars) {
        std::wstring s;
        size_t len = r.U8() % (maxLen + 1);
        for (size_t k = 0; k < len && r.Left(); ++k) s.push_back(static_cast<wchar_t>(chars[r.U8() % nchars]));
        return s;
    };
    std::wstring pat = pick(12, alphabet, sizeof alphabet - 1);
    static const char textChars[] = "abAB01 _-x\n";
    std::wstring text = pick(12, textChars, sizeof textChars - 1);
    if (pat.empty()) return;
    // Keep std::regex (a backtracking engine) out of its exponential cases.
    if (std::count_if(pat.begin(), pat.end(), [](wchar_t c) { return c == L'*' || c == L'+' || c == L'?' || c == L'{'; }) > 3) return;
    for (const wchar_t* slow : {L")*", L")+", L")?", L"){"}) // quantified groups: exponential in libstdc++
        if (pat.find(slow) != std::wstring::npos) return;
    // libstdc++ honours POSIX [. .] [= =] [: :] inside classes even in ECMAScript mode; browsers do not.
    for (const wchar_t* posix : {L"[.", L"[=", L"[:"})
        if (pat.find(posix) != std::wstring::npos) return;
    SafeRegex mine;
    if (!mine.Compile(pat)) return;
    std::wregex ref;
    try {
        ref = std::wregex(pat, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error&) {
        return;
    }
    bool want;
    try {
        want = std::regex_search(text, ref);
    } catch (const std::regex_error&) {
        return;
    }
    if (mine.Search(text) != want) {
        std::fprintf(stderr, "regex mismatch: /%ls/ on \"%ls\": std=%d safe=%d\n", pat.c_str(), text.c_str(), want, !want);
        __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    uint8_t which = data[0] % 6;
    Reader r(data + 1, size - 1);
    switch (which) {
    case 0: {
        SmbiosInfo info = ParseSmbios(data + 1, size - 1);
        info.SlotsUsed();
        for (auto& m : info.memory) { m.FormFactorName(); m.MemoryTypeName(); }
        break;
    }
    case 1: FuzzFilter(r); break;
    case 2: FuzzSettings(data + 1, size - 1); break;
    case 3: FuzzFormat(r); break;
    case 4: FuzzDiagnostics(r); break;
    case 5: FuzzRegexDiff(r); break;
    }
    return 0;
}
