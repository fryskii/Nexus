#pragma once
// Platform-neutral value formatting helpers. All functions are allocation-light and
// deterministic so they can be unit-tested on any host.
#include <cstdint>
#include <string>

namespace nx {

enum class TempUnit { Celsius, Fahrenheit };
enum class RateUnit { Bits, Bytes };

std::wstring FormatBytes(uint64_t bytes, int decimals = 1);          // 1.5 GB (binary multiples, Task Manager style)
std::wstring FormatMB(uint64_t bytes);                                // "123.4 MB" fixed-unit for process columns
std::wstring FormatRateBytes(double bytesPerSec);                     // "1.2 MB/s"
std::wstring FormatRateBits(double bitsPerSec);                       // "12.3 Mbps" (decimal multiples)
std::wstring FormatNetRate(double bytesPerSec, RateUnit unit);
std::wstring FormatPercent(double pct, int decimals = 0);             // "12%"
std::wstring FormatFreqMHz(double mhz);                               // "3.40 GHz"
std::wstring FormatTemp(double celsius, TempUnit unit);               // "54 °C"
std::wstring FormatUptime(uint64_t seconds);                          // "3:04:05:06"
std::wstring FormatDuration100ns(uint64_t t100ns);                    // "1:02:03"
std::wstring FormatCount(uint64_t n);                                 // "12,345"
std::wstring FormatHex(uint64_t v, int width = 0);
std::wstring FormatLatencyMs(double ms);                              // "0.4 ms"
std::wstring FormatFileTime(uint64_t fileTime100ns, bool utcToLocalHandled = true);

// UTF-8 <-> UTF-16/32 (wchar_t) conversion, independent of platform APIs.
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

std::wstring ToLower(std::wstring s);
bool ContainsI(const std::wstring& haystack, const std::wstring& needle);
std::wstring Trim(const std::wstring& s);

// Parses sizes such as "100MB", "1.5 GB", "512k" -> bytes. Returns false on malformed input.
bool ParseSize(const std::wstring& text, uint64_t& out);
// Quotes one command-line argument so CommandLineToArgvW / the CRT parse it back unchanged
// (embedded quotes and trailing backslashes included). Used for elevated helper arguments.
std::wstring QuoteArg(const std::wstring& arg);

} // namespace nx
