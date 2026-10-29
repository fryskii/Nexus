#pragma once
// Crash reports. When NEXUS crashes, a small minidump and a text summary are saved in
// %LOCALAPPDATA%\NEXUS\Crashes (the newest 10 are kept) so that a bug report can include them. They
// are written like process dumps: through SafeWriteGuard and, when NEXUS runs elevated, readable by
// administrators only, because a dump holds part of NEXUS's memory.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nx::crash {

// Installs the handlers for unhandled exceptions, std::terminate and C runtime errors. Call once at
// startup. An empty folder means %LOCALAPPDATA%\NEXUS\Crashes.
void Install(const std::wstring& folder = L"");
const std::wstring& Folder();
// Writes a report for `ep` on a thread of its own; returns the summary's path (empty on failure).
std::wstring WriteReport(EXCEPTION_POINTERS* ep);
// Summaries written after `afterFileTime` (UTC FILETIME), newest first; `newest` receives the latest
// write time (or `afterFileTime` when there is none).
std::vector<std::wstring> ReportsAfter(uint64_t afterFileTime, uint64_t* newest = nullptr);

} // namespace nx::crash
