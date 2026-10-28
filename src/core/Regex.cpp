#include "core/Regex.h"

#include <algorithm>
#include <cwctype>

namespace nx {

namespace {
constexpr size_t kMaxPattern = 1000;  // characters
constexpr int kMaxDepth = 64;         // group nesting
constexpr int kMaxCount = 1000;       // {n,m} bounds
constexpr size_t kMaxProgram = 20000; // NFA instructions after expanding counted repeats

wchar_t Fold(wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))); }
bool IsWord(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_'; }

using Ranges = std::vector<SafeRegex::Range>;

void AddDigits(Ranges& r) { r.push_back({L'0', L'9'}); }
void AddWord(Ranges& r) { r.insert(r.end(), {{L'a', L'z'}, {L'A', L'Z'}, {L'0', L'9'}, {L'_', L'_'}}); }
void AddSpace(Ranges& r) {
    r.insert(r.end(), {{L'\t', L'\r'}, {L' ', L' '}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A},
                       {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}});
}
Ranges Complement(Ranges r) {
    std::sort(r.begin(), r.end(), [](const SafeRegex::Range& a, const SafeRegex::Range& b) { return a.lo < b.lo; });
    Ranges out;
    uint32_t next = 0;
    const uint32_t top = static_cast<uint32_t>(WCHAR_MAX);
    for (auto& x : r) {
        if (static_cast<uint32_t>(x.lo) > next) out.push_back({static_cast<wchar_t>(next), static_cast<wchar_t>(x.lo - 1)});
        next = std::max<uint32_t>(next, static_cast<uint32_t>(x.hi) + 1);
    }
    if (next <= top) out.push_back({static_cast<wchar_t>(next), static_cast<wchar_t>(top)});
    return out;
}
} // namespace

class RegexCompiler {
public:
    RegexCompiler(SafeRegex& re, const std::wstring& p) : re_(re), p_(p) {}

    bool Run(std::wstring& err) {
        int root = ParseAlt(0);
        if (!err_.empty()) { err = err_; return false; }
        if (i_ < p_.size()) { err = L"Unmatched ')'"; return false; }
        Emit(root);
        if (!err_.empty()) { err = err_; return false; }
        Push({SafeRegex::Inst::Match});
        return true;
    }

private:
    struct Node {
        enum Kind { Empty, Char, Any, Class, Bol, Eol, WordB, NotWordB, Cat, Alt, Repeat } kind = Empty;
        wchar_t c = 0;
        int cls = -1;
        bool negated = false;
        int min = 0, max = 0; // Repeat; max < 0 = unbounded
        std::vector<int> kids;
    };

    int Add(Node n) { nodes_.push_back(std::move(n)); return static_cast<int>(nodes_.size() - 1); }
    bool Fail(const wchar_t* msg) { if (err_.empty()) err_ = msg; return false; }
    bool More() const { return i_ < p_.size() && err_.empty(); }
    wchar_t Peek() const { return i_ < p_.size() ? p_[i_] : 0; }

    int ParseAlt(int depth) {
        if (depth > kMaxDepth) { Fail(L"Pattern is nested too deeply"); return Add({}); }
        Node alt;
        alt.kind = Node::Alt;
        alt.kids.push_back(ParseSeq(depth));
        while (More() && Peek() == L'|') {
            ++i_;
            alt.kids.push_back(ParseSeq(depth));
        }
        return alt.kids.size() == 1 ? alt.kids[0] : Add(std::move(alt));
    }

    int ParseSeq(int depth) {
        Node cat;
        cat.kind = Node::Cat;
        while (More() && Peek() != L'|' && Peek() != L')') {
            int atom = ParseAtom(depth);
            int mn, mx;
            if (More() && ParseQuantifier(mn, mx)) {
                if (More() && Peek() == L'?') ++i_; // lazy: same result for a yes/no match
                // A quantifier on a quantifier (a**, a+{2}) is an error in JavaScript, so it is here.
                int dummyMin, dummyMax;
                size_t save = i_;
                if (More() && ParseQuantifier(dummyMin, dummyMax)) { i_ = save; Fail(L"Nothing to repeat"); }
                Node rep;
                rep.kind = Node::Repeat;
                rep.min = mn;
                rep.max = mx;
                rep.kids.push_back(atom);
                atom = Add(std::move(rep));
            }
            cat.kids.push_back(atom);
        }
        return Add(std::move(cat));
    }

    bool ParseQuantifier(int& mn, int& mx) {
        wchar_t c = Peek();
        if (c == L'*') { ++i_; mn = 0; mx = -1; return true; }
        if (c == L'+') { ++i_; mn = 1; mx = -1; return true; }
        if (c == L'?') { ++i_; mn = 0; mx = 1; return true; }
        if (c != L'{') return false;
        // {n}, {n,}, {n,m}; anything else is a literal '{' (as in browsers).
        size_t j = i_ + 1;
        auto num = [&](int& out) {
            size_t s = j;
            long v = 0;
            while (j < p_.size() && p_[j] >= L'0' && p_[j] <= L'9') { v = std::min<long>(v * 10 + (p_[j] - L'0'), 1000000); ++j; }
            out = static_cast<int>(v);
            return j > s;
        };
        int a = 0, b = 0;
        if (!num(a)) return false;
        b = a;
        if (j < p_.size() && p_[j] == L',') {
            ++j;
            if (!num(b)) b = -1;
        }
        if (j >= p_.size() || p_[j] != L'}') return false;
        if (a > kMaxCount || b > kMaxCount) return Fail(L"Repeat count is too large (max 1000)");
        if (b >= 0 && b < a) return Fail(L"Repeat range is out of order");
        i_ = j + 1;
        mn = a;
        mx = b;
        return true;
    }

    int ParseAtom(int depth) {
        wchar_t c = p_[i_++];
        Node n;
        switch (c) {
        case L'(': {
            if (Peek() == L'?') {
                if (i_ + 1 < p_.size() && p_[i_ + 1] == L':') i_ += 2;
                else { Fail(L"Lookaround and named groups are not supported"); return Add({}); }
            }
            int inner = ParseAlt(depth + 1);
            if (Peek() != L')') { Fail(L"Missing ')'"); return inner; }
            ++i_;
            return inner;
        }
        case L'[': return ParseClass();
        case L'.': n.kind = Node::Any; return Add(n);
        case L'^': n.kind = Node::Bol; return Add(n);
        case L'$': n.kind = Node::Eol; return Add(n);
        case L'*': case L'+': case L'?': Fail(L"Nothing to repeat"); return Add({});
        case L'\\': return ParseEscape(false, nullptr);
        default: n.kind = Node::Char; n.c = Fold(c); return Add(n);
        }
    }

    // Outside a class returns a node; inside a class appends to `into` and returns -1.
    int ParseEscape(bool inClass, Ranges* into) {
        if (i_ >= p_.size()) { Fail(L"Pattern ends with '\\'"); return Add({}); }
        wchar_t e = p_[i_++];
        Ranges set;
        bool isSet = true, neg = false;
        switch (e) {
        case L'd': AddDigits(set); break;
        case L'D': AddDigits(set); neg = true; break;
        case L'w': AddWord(set); break;
        case L'W': AddWord(set); neg = true; break;
        case L's': AddSpace(set); break;
        case L'S': AddSpace(set); neg = true; break;
        default: isSet = false;
        }
        if (isSet) {
            if (inClass) {
                Ranges r = neg ? Complement(set) : set;
                into->insert(into->end(), r.begin(), r.end());
                return -1;
            }
            Node n;
            n.kind = Node::Class;
            n.cls = static_cast<int>(re_.classes_.size());
            n.negated = neg;
            re_.classes_.push_back(std::move(set));
            return Add(n);
        }
        if (!inClass && (e == L'b' || e == L'B')) {
            Node n;
            n.kind = e == L'b' ? Node::WordB : Node::NotWordB;
            return Add(n);
        }
        wchar_t lit = 0;
        auto hex = [&](int digits) {
            unsigned v = 0;
            for (int k = 0; k < digits; ++k) {
                if (i_ >= p_.size() || !std::iswxdigit(p_[i_])) { Fail(L"Invalid \\x or \\u escape"); return wchar_t(0); }
                wchar_t h = p_[i_++];
                v = v * 16 + (h <= L'9' ? h - L'0' : (h | 0x20) - L'a' + 10);
            }
            return static_cast<wchar_t>(v);
        };
        switch (e) {
        case L't': lit = L'\t'; break;
        case L'n': lit = L'\n'; break;
        case L'r': lit = L'\r'; break;
        case L'f': lit = L'\f'; break;
        case L'v': lit = L'\v'; break;
        case L'b': lit = L'\b'; break; // only reachable inside a class
        case L'0': lit = 0; break;
        case L'x': lit = hex(2); break;
        case L'u': lit = hex(4); break;
        default:
            if (e >= L'1' && e <= L'9') { Fail(L"Backreferences are not supported"); return Add({}); }
            lit = e; // identity escape: \. \/ \( \\ ...
        }
        if (inClass) { into->push_back({lit, lit}); return -1; }
        Node n;
        n.kind = Node::Char;
        n.c = Fold(lit);
        return Add(n);
    }

    int ParseClass() {
        Node n;
        n.kind = Node::Class;
        if (Peek() == L'^') { n.negated = true; ++i_; }
        Ranges set;
        bool closed = false;
        while (i_ < p_.size() && err_.empty()) {
            wchar_t c = p_[i_];
            if (c == L']') { ++i_; closed = true; break; }
            wchar_t lo;
            ++i_;
            if (c == L'\\') {
                size_t before = set.size();
                ParseEscape(true, &set);
                if (!err_.empty()) break;
                if (set.size() != before + 1 || set.back().lo != set.back().hi) continue; // \d etc: not a range start
                lo = set.back().lo;
                set.pop_back();
            } else {
                lo = c;
            }
            wchar_t hi = lo;
            if (i_ + 1 < p_.size() && p_[i_] == L'-' && p_[i_ + 1] != L']') {
                ++i_;
                wchar_t h = p_[i_++];
                if (h == L'\\') {
                    size_t before = set.size();
                    ParseEscape(true, &set);
                    if (!err_.empty()) break;
                    if (set.size() != before + 1 || set.back().lo != set.back().hi) { Fail(L"Invalid range in character class"); break; }
                    h = set.back().lo;
                    set.pop_back();
                }
                hi = h;
                if (hi < lo) { Fail(L"Range out of order in character class"); break; }
            }
            set.push_back({lo, hi});
        }
        if (!closed) Fail(L"Missing ']'");
        n.cls = static_cast<int>(re_.classes_.size());
        re_.classes_.push_back(std::move(set));
        return Add(n);
    }

    int Push(SafeRegex::Inst in) {
        if (re_.prog_.size() >= kMaxProgram) { Fail(L"Pattern is too complex"); return static_cast<int>(re_.prog_.size()); }
        re_.prog_.push_back(in);
        return static_cast<int>(re_.prog_.size() - 1);
    }

    void Emit(int id) {
        if (!err_.empty()) return;
        const Node& n = nodes_[id];
        using I = SafeRegex::Inst;
        switch (n.kind) {
        case Node::Empty: break;
        case Node::Char: { I in{I::Char}; in.c = n.c; Push(in); break; }
        case Node::Any: Push({I::Any}); break;
        case Node::Class: { I in{I::Class}; in.cls = n.cls; in.negated = n.negated; Push(in); break; }
        case Node::Bol: Push({I::Bol}); break;
        case Node::Eol: Push({I::Eol}); break;
        case Node::WordB: Push({I::WordB}); break;
        case Node::NotWordB: Push({I::NotWordB}); break;
        case Node::Cat:
            for (int k : n.kids) Emit(k);
            break;
        case Node::Alt: {
            // split L1, next; L1: a; jmp end; next: split L2, ... ; last alternative falls through.
            std::vector<int> jumps;
            for (size_t k = 0; k < n.kids.size(); ++k) {
                if (!err_.empty()) return;
                if (k + 1 < n.kids.size()) {
                    int split = Push({I::Split});
                    if (!err_.empty()) return;
                    re_.prog_[split].x = split + 1;
                    Emit(n.kids[k]);
                    jumps.push_back(Push({I::Jmp}));
                    if (!err_.empty()) return;
                    re_.prog_[split].y = static_cast<int>(re_.prog_.size());
                } else {
                    Emit(n.kids[k]);
                }
            }
            if (!err_.empty()) return;
            for (int j : jumps) re_.prog_[j].x = static_cast<int>(re_.prog_.size());
            break;
        }
        case Node::Repeat: {
            int kid = n.kids[0];
            for (int k = 0; k < n.min && err_.empty(); ++k) Emit(kid);
            if (n.max < 0) {
                int split = Push({I::Split});
                if (!err_.empty()) return;
                re_.prog_[split].x = split + 1;
                Emit(kid);
                int jmp = Push({I::Jmp});
                if (!err_.empty()) return;
                re_.prog_[jmp].x = split;
                re_.prog_[split].y = static_cast<int>(re_.prog_.size());
            } else {
                std::vector<int> splits;
                for (int k = n.min; k < n.max && err_.empty(); ++k) {
                    int split = Push({I::Split});
                    if (!err_.empty()) return;
                    re_.prog_[split].x = split + 1;
                    splits.push_back(split);
                    Emit(kid);
                }
                if (!err_.empty()) return;
                for (int s : splits) re_.prog_[s].y = static_cast<int>(re_.prog_.size());
            }
            break;
        }
        }
    }

    SafeRegex& re_;
    const std::wstring& p_;
    size_t i_ = 0;
    std::vector<Node> nodes_;
    std::wstring err_;
};

bool SafeRegex::Compile(const std::wstring& pattern, std::wstring* error) {
    prog_.clear();
    classes_.clear();
    std::wstring err;
    if (pattern.size() > kMaxPattern) err = L"Pattern is too long (max 1000 characters)";
    else if (!RegexCompiler(*this, pattern).Run(err)) {}
    if (!err.empty()) {
        prog_.clear();
        classes_.clear();
        if (error) *error = err;
        return false;
    }
    return true;
}

bool SafeRegex::Search(const std::wstring& text) const {
    if (prog_.empty()) return false;
    const size_t n = prog_.size(), len = text.size();
    std::vector<int> cur, next, stack;
    std::vector<uint32_t> mark(n, 0);
    uint32_t gen = 0;
    cur.reserve(n);
    next.reserve(n);

    auto inClass = [&](const Inst& in, wchar_t c) {
        const auto& set = classes_[in.cls];
        wchar_t lc = Fold(c), uc = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
        bool hit = false;
        for (auto& r : set)
            if ((c >= r.lo && c <= r.hi) || (lc >= r.lo && lc <= r.hi) || (uc >= r.lo && uc <= r.hi)) { hit = true; break; }
        return hit != in.negated;
    };
    // Follows epsilon edges from pc at text position pos; returns true on reaching Match.
    auto add = [&](std::vector<int>& list, int pc0, size_t pos) {
        stack.clear();
        stack.push_back(pc0);
        while (!stack.empty()) {
            int pc = stack.back();
            stack.pop_back();
            if (mark[pc] == gen) continue;
            mark[pc] = gen;
            const Inst& in = prog_[pc];
            switch (in.op) {
            case Inst::Match: return true;
            case Inst::Jmp: stack.push_back(in.x); break;
            case Inst::Split: stack.push_back(in.y); stack.push_back(in.x); break;
            case Inst::Bol: if (pos == 0) stack.push_back(pc + 1); break;
            case Inst::Eol: if (pos == len) stack.push_back(pc + 1); break;
            case Inst::WordB: case Inst::NotWordB: {
                bool before = pos > 0 && IsWord(text[pos - 1]);
                bool after = pos < len && IsWord(text[pos]);
                if ((before != after) == (in.op == Inst::WordB)) stack.push_back(pc + 1);
                break;
            }
            default: list.push_back(pc); break;
            }
        }
        return false;
    };

    ++gen;
    if (add(cur, 0, 0)) return true;
    for (size_t pos = 0; pos < len; ++pos) {
        wchar_t c = text[pos], lc = Fold(c);
        ++gen;
        next.clear();
        for (int pc : cur) {
            const Inst& in = prog_[pc];
            bool ok = in.op == Inst::Any ? (c != L'\n' && c != L'\r' && c != 0x2028 && c != 0x2029) : in.op == Inst::Char ? lc == in.c : in.op == Inst::Class && inClass(in, c);
            if (ok && add(next, pc + 1, pos + 1)) return true;
        }
        if (add(next, 0, pos + 1)) return true; // unanchored: a match may start at any position
        std::swap(cur, next);
    }
    return false;
}

} // namespace nx
