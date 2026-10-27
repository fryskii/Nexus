#include "core/Format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace nx {

namespace {
std::wstring Printf(const wchar_t* fmt, double v) {
    wchar_t buf[64];
    std::swprintf(buf, 64, fmt, v);
    return buf;
}
} // namespace

std::wstring FormatBytes(uint64_t bytes, int decimals) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0) std::swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    else std::swprintf(buf, 64, L"%.*f %ls", decimals, v, units[u]);
    return buf;
}

std::wstring FormatMB(uint64_t bytes) {
    return Printf(L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::wstring FormatRateBytes(double bps) {
    if (bps < 0) bps = 0;
    if (bps < 1024.0) return Printf(L"%.0f B/s", bps);
    if (bps < 1024.0 * 1024.0) return Printf(L"%.1f KB/s", bps / 1024.0);
    if (bps < 1024.0 * 1024.0 * 1024.0) return Printf(L"%.1f MB/s", bps / (1024.0 * 1024.0));
    return Printf(L"%.2f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
}

std::wstring FormatRateBits(double bits) {
    if (bits < 0) bits = 0;
    if (bits < 1000.0) return Printf(L"%.0f bps", bits);
    if (bits < 1e6) return Printf(L"%.1f Kbps", bits / 1e3);
    if (bits < 1e9) return Printf(L"%.1f Mbps", bits / 1e6);
    return Printf(L"%.2f Gbps", bits / 1e9);
}

std::wstring FormatNetRate(double bytesPerSec, RateUnit unit) {
    return unit == RateUnit::Bits ? FormatRateBits(bytesPerSec * 8.0) : FormatRateBytes(bytesPerSec);
}

std::wstring FormatPercent(double pct, int decimals) {
    if (!std::isfinite(pct)) pct = 0;
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%.*f%%", decimals, pct);
    return buf;
}

std::wstring FormatFreqMHz(double mhz) {
    if (mhz <= 0) return L"—";
    if (mhz >= 1000.0) return Printf(L"%.2f GHz", mhz / 1000.0);
    return Printf(L"%.0f MHz", mhz);
}

std::wstring FormatTemp(double c, TempUnit unit) {
    if (!std::isfinite(c)) return L"—";
    if (unit == TempUnit::Fahrenheit) return Printf(L"%.0f °F", c * 9.0 / 5.0 + 32.0);
    return Printf(L"%.0f °C", c);
}

std::wstring FormatUptime(uint64_t s) {
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%llu:%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 86400), static_cast<unsigned long long>((s / 3600) % 24),
                  static_cast<unsigned long long>((s / 60) % 60), static_cast<unsigned long long>(s % 60));
    return buf;
}

std::wstring FormatDuration100ns(uint64_t t) {
    uint64_t s = t / 10000000ULL;
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60), static_cast<unsigned long long>(s % 60));
    return buf;
}

std::wstring FormatCount(uint64_t n) {
    std::wstring digits = std::to_wstring(n);
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3);
    int c = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(L',');
        out.push_back(*it);
        ++c;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring FormatHex(uint64_t v, int width) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"0x%0*llX", width, static_cast<unsigned long long>(v));
    return buf;
}

std::wstring FormatLatencyMs(double ms) {
    if (ms < 0) ms = 0;
    if (ms < 10) return Printf(L"%.1f ms", ms);
    return Printf(L"%.0f ms", ms);
}

std::wstring FormatFileTime(uint64_t ft, bool) {
    // Convert 100ns ticks since 1601-01-01 (already local) to a civil date/time.
    uint64_t secs = ft / 10000000ULL;
    int64_t days = static_cast<int64_t>(secs / 86400ULL);
    uint64_t rem = secs % 86400ULL;
    // Days since 1601-01-01 -> civil date (algorithm from Howard Hinnant, shifted epoch).
    int64_t z = days - 134774 + 719468; // 134774 days between 1601-01-01 and 1970-01-01
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%04lld-%02u-%02u %02llu:%02llu:%02llu", static_cast<long long>(y), m, d,
                  static_cast<unsigned long long>(rem / 3600), static_cast<unsigned long long>((rem / 60) % 60),
                  static_cast<unsigned long long>(rem % 60));
    return buf;
}

std::string ToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t lo = static_cast<uint32_t>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (extra && i + extra >= s.size()) { out.push_back(0xFFFD); break; } // truncated sequence
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += extra + 1;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ContainsI(const std::wstring& h, const std::wstring& n) {
    if (n.empty()) return true;
    if (n.size() > h.size()) return false;
    auto it = std::search(h.begin(), h.end(), n.begin(), n.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != h.end();
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool ParseSize(const std::wstring& text, uint64_t& out) {
    std::wstring t = ToLower(Trim(text));
    if (t.empty()) return false;
    size_t i = 0;
    double v = 0;
    bool any = false, dot = false;
    double frac = 0.1;
    for (; i < t.size(); ++i) {
        wchar_t c = t[i];
        if (c >= L'0' && c <= L'9') {
            any = true;
            if (!dot) v = v * 10 + (c - L'0');
            else { v += (c - L'0') * frac; frac /= 10; }
        } else if (c == L'.' && !dot) dot = true;
        else break;
    }
    if (!any) return false;
    std::wstring unit = Trim(t.substr(i));
    double mul = 1;
    if (unit.empty() || unit == L"b") mul = 1;
    else if (unit == L"k" || unit == L"kb") mul = 1024.0;
    else if (unit == L"m" || unit == L"mb") mul = 1024.0 * 1024.0;
    else if (unit == L"g" || unit == L"gb") mul = 1024.0 * 1024.0 * 1024.0;
    else if (unit == L"t" || unit == L"tb") mul = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else return false;
    out = static_cast<uint64_t>(v * mul + 0.5);
    return true;
}

} // namespace nx
