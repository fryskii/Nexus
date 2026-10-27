#pragma once
// Simple, robust key=value settings store (UTF-8 text). Unknown keys are preserved so that
// newer versions don't lose settings when an older build rewrites the file.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nx {

class Settings {
public:
    void Parse(const std::string& utf8Text);
    std::string Serialize() const;

    std::wstring GetString(const std::string& key, const std::wstring& def = L"") const;
    int64_t GetInt(const std::string& key, int64_t def = 0) const;
    double GetDouble(const std::string& key, double def = 0) const;
    bool GetBool(const std::string& key, bool def = false) const;
    std::vector<std::wstring> GetList(const std::string& key) const; // '|' separated

    void SetString(const std::string& key, const std::wstring& v);
    void SetInt(const std::string& key, int64_t v);
    void SetDouble(const std::string& key, double v);
    void SetBool(const std::string& key, bool v);
    void SetList(const std::string& key, const std::vector<std::wstring>& v);
    bool Has(const std::string& key) const { return values_.count(key) != 0; }
    void Remove(const std::string& key) { values_.erase(key); dirty_ = true; }

    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

private:
    std::map<std::string, std::string> values_; // sorted -> stable file output
    bool dirty_ = false;
};

} // namespace nx
