// NEXUS core unit tests. Self-contained (no external framework) so they build with any toolchain
// and run on Windows, Linux (core logic is platform-neutral) or under Wine.
#include "core/DiagnosticsRules.h"
#include "core/Filter.h"
#include "core/Format.h"
#include "core/Regex.h"
#include "core/RingBuffer.h"
#include "core/Settings.h"
#include "core/Smbios.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) ++g_passed;                                                      \
        else { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ_W(a, b)                                                                           \
    do {                                                                                           \
        std::wstring _a = (a), _b = (b);                                                           \
        if (_a == _b) ++g_passed;                                                                  \
        else { ++g_failed; std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,       \
                                       nx::ToUtf8(_a).c_str(), nx::ToUtf8(_b).c_str()); }          \
    } while (0)

using namespace nx;

static void TestFormat() {
    CHECK_EQ_W(FormatBytes(0), L"0 B");
    CHECK_EQ_W(FormatBytes(1536), L"1.5 KB");
    CHECK_EQ_W(FormatBytes(16ULL * 1024 * 1024 * 1024), L"16.0 GB");
    CHECK_EQ_W(FormatMB(10 * 1024 * 1024), L"10.0 MB");
    CHECK_EQ_W(FormatRateBits(1500000), L"1.5 Mbps");
    CHECK_EQ_W(FormatRateBytes(2048), L"2.0 KB/s");
    CHECK_EQ_W(FormatPercent(12.4), L"12%");
    CHECK_EQ_W(FormatPercent(12.46, 1), L"12.5%");
    CHECK_EQ_W(FormatFreqMHz(3400), L"3.40 GHz");
    CHECK_EQ_W(FormatTemp(50, TempUnit::Fahrenheit), L"122 \u00B0F");
    CHECK_EQ_W(FormatUptime(90061), L"1:01:01:01");
    CHECK_EQ_W(FormatCount(1234567), L"1,234,567");
    CHECK_EQ_W(FormatCount(12), L"12");
    // 2024-02-29 12:34:56 as FILETIME ticks
    uint64_t secs1601to1970 = 11644473600ULL;
    uint64_t unix = 1709210096ULL;
    CHECK_EQ_W(FormatFileTime((unix + secs1601to1970) * 10000000ULL), L"2024-02-29 12:34:56");

    std::wstring w = L"h\u00E9llo \u4E16\u754C \U0001F600";
    CHECK(FromUtf8(ToUtf8(w)) == w);
    CHECK(FromUtf8("\xE4\xB8") == std::wstring(1, 0xFFFD)); // truncated sequence must not overrun
    CHECK(ContainsI(L"Microsoft Edge", L"EDGE"));
    CHECK(!ContainsI(L"abc", L"abcd"));

    uint64_t b = 0;
    CHECK(ParseSize(L"100MB", b) && b == 100ULL * 1024 * 1024);
    CHECK(ParseSize(L"1.5 gb", b) && b == 1610612736ULL);
    CHECK(!ParseSize(L"abc", b));
    CHECK(!ParseSize(L"5 zb", b));
}

static void TestSettings() {
    Settings s;
    s.SetString("ui.theme", L"dark");
    s.SetInt("refresh.ms", 1000);
    s.SetBool("window.topmost", true);
    s.SetDouble("graph.scale", 1.25);
    s.SetString("multi", L"line1\nline2\\x");
    s.SetList("nav.hidden", {L"Users", L"App History"});
    CHECK(s.Dirty());
    std::string text = s.Serialize();
    Settings t;
    t.Parse(text);
    CHECK_EQ_W(t.GetString("ui.theme"), L"dark");
    CHECK(t.GetInt("refresh.ms") == 1000);
    CHECK(t.GetBool("window.topmost"));
    CHECK(t.GetDouble("graph.scale") == 1.25);
    CHECK_EQ_W(t.GetString("multi"), L"line1\nline2\\x");
    auto l = t.GetList("nav.hidden");
    CHECK(l.size() == 2 && l[1] == L"App History");
    CHECK(t.GetInt("missing", 7) == 7);
    CHECK(!t.Dirty());
    Settings bad;
    bad.Parse("\xEF\xBB\xBF# comment\n[section]\nnoequals\n=novalue\nk = v \r\nnum=12abc\n");
    CHECK_EQ_W(bad.GetString("k"), L"v");
    CHECK(bad.GetInt("num", -1) == -1);
    // Leading/trailing spaces and tabs survive a save and reload (the parser trims unescaped edges).
    Settings sp;
    sp.SetString("search", L"  two spaces\t");
    sp.SetString("tab", L"\tx\t");
    sp.SetString("one", L" ");
    Settings sp2;
    sp2.Parse(sp.Serialize());
    CHECK_EQ_W(sp2.GetString("search"), L"  two spaces\t");
    CHECK_EQ_W(sp2.GetString("tab"), L"\tx\t");
    CHECK_EQ_W(sp2.GetString("one"), L" ");
    CHECK(sp2.Serialize() == sp.Serialize());
}

static void TestRingBuffer() {
    RingBuffer<int> r(3);
    CHECK(r.Empty());
    for (int i = 1; i <= 5; ++i) r.Push(i);
    CHECK(r.Size() == 3 && r.At(0) == 3 && r.Last() == 5 && r.Max() == 5);
    r.Resize(5);
    CHECK(r.Size() == 3 && r.At(0) == 3 && r.Last() == 5);
    r.Push(6);
    CHECK(r.Size() == 4 && r.Last() == 6 && r.At(0) == 3);
    r.Resize(2);
    CHECK(r.Size() == 2 && r.At(0) == 5 && r.Last() == 6);
    std::vector<int> tail;
    r.Tail(10, tail);
    CHECK(tail.size() == 2 && tail[0] == 5);
}


static void TestRegex() {
    auto re = [](const wchar_t* p, const std::wstring& t) {
        SafeRegex r;
        return r.Compile(p) && r.Search(t);
    };
    auto bad = [](const wchar_t* p) { SafeRegex r; return !r.Compile(p); };
    CHECK(re(L"chr(o|a)me", L"Chrome.exe"));
    CHECK(re(L"^svc", L"svchost.exe") && !re(L"^host", L"svchost.exe"));
    CHECK(re(L"exe$", L"a.exe") && !re(L"exe$", L"a.exe "));
    CHECK(re(L"a.c", L"abc") && !re(L"a.c", L"a\nc"));
    CHECK(re(L"[a-c]+x", L"zzABCx") && !re(L"[^a-c]x", L"cx"));
    CHECK(re(L"\\d{3}-\\d{2,}", L"tel 555-1234") && !re(L"^\\d{3}$", L"5555"));
    CHECK(re(L"\\bhost\\b", L"svc host x") && !re(L"\\bhost\\b", L"svchost"));
    CHECK(re(L"\\Bhost", L"svchost"));
    CHECK(re(L"\\s\\S", L"a b") && re(L"[\\s\\d]x", L"1x") && !re(L"[\\D]", L"123"));
    CHECK(re(L"\\.exe", L"a.exe") && !re(L"\\.exe", L"aXexe"));
    CHECK(re(L"\\x41\\u0042", L"ab"));
    CHECK(re(L"(?:ab)+c", L"ababc") && re(L"a{0}b", L"b") && re(L"x(a|)y", L"xy"));
    CHECK(re(L"a{2,3}?b", L"aab") && re(L"a{,", L"a{,") && !re(L"[]a]", L"]"));
    CHECK(re(L"(a*)*b", L"aaab") && re(L"(|a)+$", L"aa"));
    CHECK(bad(L"(a") && bad(L"a)") && bad(L"[a") && bad(L"a**") && bad(L"*a") && bad(L"(?=a)") && bad(L"(a)\\1") &&
          bad(L"[z-a]") && bad(L"a{5,2}") && bad(L"a{1001}") && bad(L"\\") && bad(L"\\xZ1"));
    CHECK(bad(std::wstring(1001, L'a').c_str()));
    CHECK(bad(L"((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((a))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))"));
    CHECK(bad(L"(a{1000}){1000}")); // program size limit
    // Patterns that make backtracking engines (std::regex) run for minutes finish immediately.
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!re(L"(a+)+$", std::wstring(5000, L'a') + L"!"));
    CHECK(!re(L"(a|aa)*b", std::wstring(5000, L'a')));
    CHECK(!re(L".*x.*y.*z", std::wstring(32767, L'q')));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

static void TestFilter() {
    std::wstring name = L"chrome.exe", pub = L"Google LLC", path = L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
                 cmd = L"chrome.exe --type=renderer", user = L"alice", desc = L"Google Chrome";
    FilterSubject s;
    s.pid = 1234; s.ppid = 100; s.name = &name; s.publisher = &pub; s.path = &path; s.commandLine = &cmd; s.user = &user;
    s.description = &desc; s.cpu = 12.5; s.memory = 300ULL * 1024 * 1024; s.threads = 30;

    auto match = [&](const wchar_t* q) {
        ProcessFilter f;
        f.Compile(q);
        return f.Matches(s);
    };
    CHECK(match(L""));
    CHECK(match(L"chrome"));
    CHECK(match(L"CHROME"));
    CHECK(match(L"1234"));
    CHECK(match(L"pid:1234"));
    CHECK(!match(L"pid:123"));
    CHECK(match(L"ppid:100"));
    CHECK(match(L"pub:google"));
    CHECK(!match(L"pub:microsoft"));
    CHECK(match(L"cpu>10"));
    CHECK(!match(L"cpu>20"));
    CHECK(match(L"cpu<20 mem>200MB"));
    CHECK(match(L"mem>200"));
    CHECK(!match(L"mem>1GB"));
    CHECK(match(L"/chr(o|a)me/"));
    CHECK(match(L"re:type=render"));
    CHECK(match(L"!firefox"));
    CHECK(!match(L"!chrome"));
    CHECK(match(L"cmd:renderer user:ALI"));
    // Out-of-range numbers never match and never overflow.
    CHECK(!match(L"pid:1e30") && !match(L"pid:-1") && !match(L"ppid:nan"));
    CHECK(match(L"mem>-5") && !match(L"mem>1e300") && match(L"mem<99999999999999999999TB"));
    {
        ProcessFilter f;
        std::wstring err;
        CHECK(!f.Compile(L"/(a/", &err) && !err.empty());
    }
    CHECK(match(L"\"google chrome\""));
    CHECK(match(L"path:\\google\\"));
    ProcessFilter bad;
    std::wstring err;
    CHECK(!bad.Compile(L"/([/", &err));
    CHECK(!err.empty());
}

static std::vector<uint8_t> BuildSmbios() {
    std::vector<uint8_t> t;
    auto add = [&](std::vector<uint8_t> formatted, std::vector<std::string> strings) {
        formatted[1] = static_cast<uint8_t>(formatted.size());
        t.insert(t.end(), formatted.begin(), formatted.end());
        if (strings.empty()) { t.push_back(0); t.push_back(0); return; }
        for (auto& s : strings) { t.insert(t.end(), s.begin(), s.end()); t.push_back(0); }
        t.push_back(0);
    };
    // Type 0 BIOS
    std::vector<uint8_t> bios(0x18, 0);
    bios[0] = 0; bios[4] = 1; bios[5] = 2; bios[8] = 3;
    add(bios, {"American Megatrends", "1.2.3  ", "01/02/2024"});
    // Type 16 array: use=3, max capacity 64GB (in KB), 4 slots
    std::vector<uint8_t> arr(0x17, 0);
    arr[0] = 16; arr[5] = 3;
    uint32_t kb = 64u * 1024 * 1024;
    std::memcpy(&arr[7], &kb, 4);
    arr[0x0D] = 4;
    add(arr, {});
    // Type 17 device: 16GB DDR5 DIMM 5600/5200
    std::vector<uint8_t> dev(0x5C, 0);
    dev[0] = 17;
    uint16_t sz = 16384; std::memcpy(&dev[0x0C], &sz, 2);
    dev[0x0E] = 0x09; dev[0x10] = 1; dev[0x11] = 2; dev[0x12] = 0x22;
    uint16_t sp = 5600; std::memcpy(&dev[0x15], &sp, 2);
    dev[0x17] = 3; dev[0x18] = 4; dev[0x1A] = 5; dev[0x1B] = 2;
    uint16_t cs = 5200; std::memcpy(&dev[0x20], &cs, 2);
    add(dev, {"DIMM_A1", "BANK 0", "Kingston", "SN123", "KF556C40-16"});
    // Empty slot
    std::vector<uint8_t> empty(0x28, 0);
    empty[0] = 17; empty[0x10] = 1;
    add(empty, {"DIMM_A2"});
    // End of table
    std::vector<uint8_t> eot(4, 0);
    eot[0] = 127;
    add(eot, {});

    std::vector<uint8_t> raw(8, 0);
    raw[1] = 3; raw[2] = 6;
    uint32_t len = static_cast<uint32_t>(t.size());
    std::memcpy(&raw[4], &len, 4);
    raw.insert(raw.end(), t.begin(), t.end());
    return raw;
}

static void TestSmbios() {
    auto raw = BuildSmbios();
    SmbiosInfo i = ParseSmbios(raw.data(), raw.size());
    CHECK(i.valid);
    CHECK(i.majorVersion == 3 && i.minorVersion == 6);
    CHECK_EQ_W(i.biosVendor, L"American Megatrends");
    CHECK_EQ_W(i.biosVersion, L"1.2.3");
    CHECK(i.memorySlots == 4);
    CHECK(i.maxMemoryCapacityBytes == 64ULL * 1024 * 1024 * 1024);
    CHECK(i.memory.size() == 2);
    CHECK(i.SlotsUsed() == 1);
    if (i.memory.size() == 2) {
        auto& m = i.memory[0];
        CHECK(m.sizeBytes == 16ULL * 1024 * 1024 * 1024);
        CHECK_EQ_W(m.MemoryTypeName(), L"DDR5");
        CHECK_EQ_W(m.FormFactorName(), L"DIMM");
        CHECK(m.speedMTs == 5600 && m.configuredSpeedMTs == 5200);
        CHECK_EQ_W(m.manufacturer, L"Kingston");
        CHECK_EQ_W(m.partNumber, L"KF556C40-16");
        CHECK(m.rank == 2);
        CHECK_EQ_W(i.memory[1].deviceLocator, L"DIMM_A2");
        CHECK(i.memory[1].sizeBytes == 0);
    }
    // Fuzz: random truncations / corruptions must never crash.
    for (size_t cut = 0; cut < raw.size(); ++cut) (void)ParseSmbios(raw.data(), cut);
    auto bad = raw;
    for (size_t k = 8; k < bad.size(); k += 7) {
        bad[k] ^= 0xA5;
        (void)ParseSmbios(bad.data(), bad.size());
    }
    CHECK(!ParseSmbios(nullptr, 0).valid);
}

static void TestDiagnostics() {
    double slope, r2;
    LinearFit({1, 2, 3, 4, 5}, slope, r2);
    CHECK(slope > 0.99 && slope < 1.01 && r2 > 0.99);
    CHECK(Mean({1, 2, 3}) == 2.0);
    CHECK(FractionAtLeast({1, 5, 10, 20}, 10) == 0.5);
    {
        // Degenerate sampling intervals must not overflow sample-count arithmetic.
        DiagInput in;
        in.totalCpu.assign(100, 99.f);
        for (double iv : {0.0, -1.0, 1e-300, 1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            in.intervalSec = iv;
            EvaluateDiagnostics(in);
        }
        CHECK(true);
    }

    DiagInput in;
    in.intervalSec = 1;
    in.logicalCpus = 8;
    DiagInput::Proc hog;
    hog.pid = 42; hog.name = L"hog.exe";
    hog.cpu.assign(120, 13.0f); // > 1 core of 8
    DiagInput::Proc leak;
    leak.pid = 43; leak.name = L"leak.exe";
    for (int i = 0; i < 600; ++i) leak.privateMB.push_back(200.0f + i * 0.5f); // +300 MB over 10 min
    leak.cpu.assign(10, 0.f);
    DiagInput::Proc calm;
    calm.pid = 44; calm.name = L"calm.exe";
    calm.cpu.assign(120, 0.5f);
    for (int i = 0; i < 600; ++i) calm.privateMB.push_back(100.0f + (i % 7));
    in.procs = {hog, leak, calm};
    in.totalCpu.assign(60, 30.f);
    in.volumes.push_back({L"C:", 2ULL * 1024 * 1024 * 1024, 500ULL * 1024 * 1024 * 1024});
    in.adapters.push_back({L"Ethernet", 50, 0, 10000});
    in.adapters.push_back({L"Wi-Fi", 0, 0, 100000});
    in.storageHealth.push_back({L"NVMe0", 0x04, 12, 100, 10, 40});
    DiagInput::Disk d;
    d.name = L"Disk 0";
    d.latencyMs.assign(60, 150.f);
    d.activePct.assign(60, 90.f);
    in.disks.push_back(d);

    auto alerts = EvaluateDiagnostics(in);
    auto has = [&](uint32_t pid, const wchar_t* cat) {
        for (auto& a : alerts) if (a.pid == pid && a.category == cat) return true;
        return false;
    };
    auto hasTitle = [&](const wchar_t* part) {
        for (auto& a : alerts) if (ContainsI(a.title, part)) return true;
        return false;
    };
    CHECK(has(42, L"CPU"));
    CHECK(has(43, L"Memory"));
    CHECK(!has(44, L"CPU"));
    CHECK(!has(44, L"Memory"));
    CHECK(hasTitle(L"Low free space"));
    CHECK(hasTitle(L"Ethernet"));
    CHECK(!hasTitle(L"Wi-Fi"));
    CHECK(hasTitle(L"NVMe0"));
    CHECK(hasTitle(L"Disk 0"));
    CHECK(!hasTitle(L"saturated"));
    for (auto& a : alerts) CHECK(!a.evidence.empty() && !a.reasoning.empty());
    // Sorted most severe first.
    for (size_t i = 1; i < alerts.size(); ++i) CHECK(alerts[i - 1].severity >= alerts[i].severity);

    DiagInput sat;
    sat.totalCpu.assign(60, 99.f);
    sat.commitPct.assign(60, 98.f);
    sat.availPct.assign(60, 3.f);
    sat.hardFaults.assign(60, 3000.f);
    auto a2 = EvaluateDiagnostics(sat);
    bool satSeen = false, memCrit = false;
    for (auto& a : a2) {
        satSeen |= a.title.find(L"saturated") != std::wstring::npos;
        memCrit |= a.category == L"Memory" && a.severity == DiagAlert::Critical;
    }
    CHECK(satSeen);
    CHECK(memCrit);
    CHECK(EvaluateDiagnostics(DiagInput{}).empty());
}

int main() {
    TestFormat();
    TestSettings();
    TestRingBuffer();
    TestFilter();
    TestRegex();
    TestSmbios();
    TestDiagnostics();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
