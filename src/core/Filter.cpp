#include "core/Filter.h"
#include "core/Format.h"

#include <cwchar>
#include <regex>

namespace nx {

struct ProcessFilter::Term {
    enum Kind { Text, Pid, PPid, Name, Path, Pub, User, Cmd, Regex, Num } kind = Text;
    enum Field { Cpu, Mem, Gpu, Io, Threads, Handles } field = Cpu;
    bool negate = false;
    bool greater = true;
    double value = 0;
    std::wstring text;
    std::wregex re;
};

ProcessFilter::ProcessFilter() = default;
ProcessFilter::~ProcessFilter() = default;
ProcessFilter::ProcessFilter(ProcessFilter&&) noexcept = default;
ProcessFilter& ProcessFilter::operator=(ProcessFilter&&) noexcept = default;

bool ProcessFilter::Empty() const { return terms_.empty(); }

namespace {
std::vector<std::wstring> Tokenize(const std::wstring& q) {
    // Whitespace separated; double quotes group; /regex/ may contain spaces.
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuote = false, inRegex = false;
    for (size_t i = 0; i < q.size(); ++i) {
        wchar_t c = q[i];
        if (!inQuote && c == L'/' && (cur.empty() || cur == L"!")) { inRegex = true; cur.push_back(c); continue; }
        if (inRegex) {
            cur.push_back(c);
            if (c == L'/' && cur.size() > 1 && q[i - 1] != L'\\') inRegex = false;
            continue;
        }
        if (c == L'"') { inQuote = !inQuote; continue; }
        if (!inQuote && (c == L' ' || c == L'\t')) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
} // namespace

bool ProcessFilter::Compile(const std::wstring& query, std::wstring* error) {
    terms_.clear();
    bool ok = true;
    for (std::wstring tok : Tokenize(query)) {
        auto t = std::make_unique<Term>();
        if (tok.size() > 1 && tok[0] == L'!') { t->negate = true; tok.erase(0, 1); }
        std::wstring lower = ToLower(tok);

        auto prefixed = [&](const wchar_t* p, Term::Kind k) {
            size_t n = std::wcslen(p);
            if (lower.compare(0, n, p) == 0 && tok.size() > n) { t->kind = k; t->text = ToLower(tok.substr(n)); return true; }
            return false;
        };

        std::wstring pattern;
        if (tok.size() >= 2 && tok.front() == L'/' && tok.back() == L'/') pattern = tok.substr(1, tok.size() - 2);
        else if (lower.compare(0, 3, L"re:") == 0) pattern = tok.substr(3);

        if (!pattern.empty()) {
            try {
                t->re = std::wregex(pattern, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
                t->kind = Term::Regex;
            } catch (const std::regex_error&) {
                ok = false;
                if (error) *error = L"Invalid regular expression: " + pattern;
                continue;
            }
        } else if (prefixed(L"pid:", Term::Pid) || prefixed(L"ppid:", Term::PPid)) {
            t->value = std::wcstod(t->text.c_str(), nullptr);
        } else if (prefixed(L"name:", Term::Name) || prefixed(L"path:", Term::Path) || prefixed(L"pub:", Term::Pub) ||
                   prefixed(L"user:", Term::User) || prefixed(L"cmd:", Term::Cmd)) {
            // text already set
        } else {
            // Numeric comparisons: field(>|<)value
            size_t op = lower.find_first_of(L"<>");
            static const struct { const wchar_t* n; Term::Field f; bool size; } fields[] = {
                {L"cpu", Term::Cpu, false}, {L"mem", Term::Mem, true},          {L"gpu", Term::Gpu, false},
                {L"io", Term::Io, true},    {L"threads", Term::Threads, false}, {L"handles", Term::Handles, false},
            };
            bool numeric = false;
            if (op != std::wstring::npos && op > 0) {
                std::wstring fname = lower.substr(0, op);
                std::wstring val = lower.substr(op + 1);
                for (auto& f : fields) {
                    if (fname != f.n) continue;
                    t->kind = Term::Num;
                    t->field = f.f;
                    t->greater = lower[op] == L'>';
                    if (f.size) {
                        uint64_t b = 0;
                        if (!ParseSize(val, b)) {
                            // Bare numbers for mem/io are interpreted as MB, matching the column unit.
                            b = static_cast<uint64_t>(std::wcstod(val.c_str(), nullptr) * 1024.0 * 1024.0);
                        }
                        t->value = static_cast<double>(b);
                    } else {
                        t->value = std::wcstod(val.c_str(), nullptr);
                    }
                    numeric = true;
                    break;
                }
            }
            if (!numeric) { t->kind = Term::Text; t->text = lower; }
        }
        terms_.push_back(std::move(t));
    }
    return ok;
}

bool ProcessFilter::Matches(const FilterSubject& s) const {
    auto has = [](const std::wstring* f, const std::wstring& needle) { return f && ContainsI(*f, needle); };
    for (auto& tp : terms_) {
        const Term& t = *tp;
        bool m = false;
        switch (t.kind) {
        case Term::Text:
            m = has(s.name, t.text) || has(s.description, t.text) || has(s.publisher, t.text) ||
                std::to_wstring(s.pid) == t.text;
            break;
        case Term::Pid: m = s.pid == static_cast<uint32_t>(t.value); break;
        case Term::PPid: m = s.ppid == static_cast<uint32_t>(t.value); break;
        case Term::Name: m = has(s.name, t.text); break;
        case Term::Path: m = has(s.path, t.text); break;
        case Term::Pub: m = has(s.publisher, t.text); break;
        case Term::User: m = has(s.user, t.text); break;
        case Term::Cmd: m = has(s.commandLine, t.text); break;
        case Term::Regex:
            m = (s.name && std::regex_search(*s.name, t.re)) || (s.path && std::regex_search(*s.path, t.re)) ||
                (s.commandLine && std::regex_search(*s.commandLine, t.re));
            break;
        case Term::Num: {
            double v = 0;
            switch (t.field) {
            case Term::Cpu: v = s.cpu; break;
            case Term::Mem: v = static_cast<double>(s.memory); break;
            case Term::Gpu: v = s.gpu; break;
            case Term::Io: v = s.ioRate; break;
            case Term::Threads: v = s.threads; break;
            case Term::Handles: v = s.handles; break;
            }
            m = t.greater ? v > t.value : v < t.value;
            break;
        }
        }
        if (m == t.negate) return false;
    }
    return true;
}

} // namespace nx
