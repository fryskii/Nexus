#pragma once
// Process search/filter language used by the Processes and Details views.
//
//   chrome            plain text: case-insensitive match on name, description, publisher, PID
//   pid:1234          exact PID          ppid:4      exact parent PID
//   name:svc path:\windows\  pub:microsoft  user:SYSTEM  cmd:--type=gpu   (substring)
//   /reg(ex)?/        ECMAScript regex over name, path and command line (also re:pattern)
//   cpu>5  cpu<1      CPU percent        mem>200MB   private/working memory
//   gpu>10            GPU percent        io>1MB      I/O rate (bytes/s)
//   threads>100  handles>5000
//   !term             negates any term   (all terms are ANDed)
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx {

struct FilterSubject {
    uint32_t pid = 0, ppid = 0;
    const std::wstring* name = nullptr;
    const std::wstring* description = nullptr;
    const std::wstring* publisher = nullptr;
    const std::wstring* path = nullptr;
    const std::wstring* user = nullptr;
    const std::wstring* commandLine = nullptr;
    double cpu = 0, gpu = 0, ioRate = 0;
    uint64_t memory = 0;
    uint32_t threads = 0, handles = 0;
};

class ProcessFilter {
public:
    ProcessFilter();
    ~ProcessFilter();
    ProcessFilter(ProcessFilter&&) noexcept;
    ProcessFilter& operator=(ProcessFilter&&) noexcept;

    // Returns false and fills error when a term is malformed (e.g. bad regex); the filter
    // still contains the terms that did compile.
    bool Compile(const std::wstring& query, std::wstring* error = nullptr);
    bool Empty() const;
    bool Matches(const FilterSubject& s) const;

private:
    struct Term;
    std::vector<std::unique_ptr<Term>> terms_;
};

} // namespace nx
