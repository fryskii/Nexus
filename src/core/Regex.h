#pragma once
// Small case-insensitive regular-expression matcher for the search box. It compiles to a Thompson
// NFA and simulates it breadth-first, so matching is O(pattern x text) with no recursion over the
// text: a hostile pattern such as (a+)+$ or a 32K-character command line cannot freeze or crash the UI
// the way a backtracking engine (std::regex) can.
//
// Supported (ECMAScript subset): literals, ., [...] and [^...] with ranges, \d \D \w \W \s \S \b \B,
// \t \n \r \f \v \xHH \uHHHH and escaped punctuation, ^ $, ( ) (?: ), |, * + ? {n} {n,} {n,m}
// (lazy suffixes are accepted; they do not change whether a match exists).
// Not supported, rejected with an error: backreferences and lookaround.
#include <cstdint>
#include <string>
#include <vector>

namespace nx {

class SafeRegex {
public:
    bool Compile(const std::wstring& pattern, std::wstring* error = nullptr);
    bool Search(const std::wstring& text) const; // true if the pattern matches anywhere in text
    bool Valid() const { return !prog_.empty(); }

    struct Range { wchar_t lo, hi; };
    struct Inst {
        enum Op : uint8_t { Char, Any, Class, Split, Jmp, Match, Bol, Eol, WordB, NotWordB } op;
        bool negated = false;  // Class
        wchar_t c = 0;         // Char (already lower-cased)
        int x = 0, y = 0;      // Split / Jmp targets
        int cls = -1;          // index into classes_
    };

private:
    std::vector<Inst> prog_;
    std::vector<std::vector<Range>> classes_;
    friend class RegexCompiler;
};

} // namespace nx
