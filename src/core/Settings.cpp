#include "core/Settings.h"
#include "core/Format.h"

#include <cstdlib>
#include <cstdio>

namespace nx {

namespace {
std::string TrimA(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}
// Values are escaped so that newlines and backslashes round-trip safely.
std::string Escape(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    for (char c : v) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o.push_back(c);
    }
    return o;
}
std::string Unescape(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            o.push_back(n == 'n' ? '\n' : n == 'r' ? '\r' : n);
        } else {
            o.push_back(v[i]);
        }
    }
    return o;
}
} // namespace

void Settings::Parse(const std::string& text) {
    values_.clear();
    size_t pos = 0;
    // Skip UTF-8 BOM.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        pos = 3;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = TrimA(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = TrimA(line.substr(0, eq));
        if (k.empty() || k.size() > 128) continue;
        values_[k] = Unescape(TrimA(line.substr(eq + 1)));
    }
    dirty_ = false;
}

std::string Settings::Serialize() const {
    std::string out = "# NEXUS settings - UTF-8, key=value. Safe to edit while NEXUS is closed.\n";
    for (auto& [k, v] : values_) out += k + "=" + Escape(v) + "\n";
    return out;
}

std::wstring Settings::GetString(const std::string& key, const std::wstring& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : FromUtf8(it->second);
}
int64_t Settings::GetInt(const std::string& key, int64_t def) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return def;
    char* end = nullptr;
    long long v = std::strtoll(it->second.c_str(), &end, 0);
    return (end && *end == 0) ? v : def;
}
double Settings::GetDouble(const std::string& key, double def) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    return (end && *end == 0) ? v : def;
}
bool Settings::GetBool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}
std::vector<std::wstring> Settings::GetList(const std::string& key) const {
    std::vector<std::wstring> out;
    std::wstring s = GetString(key);
    size_t p = 0;
    while (p <= s.size() && !s.empty()) {
        size_t bar = s.find(L'|', p);
        if (bar == std::wstring::npos) bar = s.size();
        if (bar > p) out.push_back(s.substr(p, bar - p));
        p = bar + 1;
    }
    return out;
}

void Settings::SetString(const std::string& key, const std::wstring& v) {
    std::string u = ToUtf8(v);
    auto it = values_.find(key);
    if (it != values_.end() && it->second == u) return;
    values_[key] = std::move(u);
    dirty_ = true;
}
void Settings::SetInt(const std::string& key, int64_t v) { SetString(key, std::to_wstring(v)); }
void Settings::SetDouble(const std::string& key, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    SetString(key, FromUtf8(buf));
}
void Settings::SetBool(const std::string& key, bool v) { SetString(key, v ? L"1" : L"0"); }
void Settings::SetList(const std::string& key, const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L'|';
        s += v[i];
    }
    SetString(key, s);
}

} // namespace nx
