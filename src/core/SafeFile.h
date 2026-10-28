#pragma once
// File writes that are safe when NEXUS runs elevated.
//
// As the Task Manager replacement on an administrator account, NEXUS runs elevated but writes into
// folders that any program running as the user can change: the profile (settings, history), %TEMP%
// (dumps) and export locations. Such a program could swap one of those folders for a junction or
// symbolic link, or put a link where NEXUS's file goes, so that an administrator's write lands in a
// protected location instead. SafeWriteGuard prevents that:
//   - Prepare refuses network/device paths and any folder on the path that is a junction or
//     symbolic link (cloud-file placeholders such as OneDrive folders are fine), and keeps each
//     folder open without delete sharing, so none can be renamed or replaced until the guard ends.
//   - Open never truncates an existing file in place (CREATE_ALWAYS / TRUNCATE_EXISTING are refused;
//     write a new file and rename it instead) and refuses files that are links or have several
//     hard links, checked on the opened handle so the check cannot be raced, and verifies that the
//     opened file really is in the locked folder (whatever the file system did with the name).
// When NEXUS is not elevated these are plain operations: the folders belong to the user anyway.
#include "core/Win.h"

#include <string>
#include <vector>

namespace nx {

class SafeWriteGuard {
public:
    SafeWriteGuard();
    SafeWriteGuard(const SafeWriteGuard&) = delete;
    SafeWriteGuard& operator=(const SafeWriteGuard&) = delete;
    ~SafeWriteGuard();

    // Checks and locks the folders leading to `path` (a file path). Returns false with a reason.
    bool Prepare(const std::wstring& path, std::wstring* why = nullptr);
    // CreateFileW for writing within a prepared path; INVALID_HANDLE_VALUE with a reason on refusal.
    HANDLE Open(const std::wstring& path, DWORD access, DWORD disposition, std::wstring* why = nullptr);

private:
    std::vector<HANDLE> dirs_;
    std::wstring finalDir_; // resolved path of the file's (locked) folder
    bool elevated_ = false;
};

// True if `path` is inside the current user's profile folder (after normalization). An elevated
// NEXUS only writes settings-chosen folders (history recording) there, so an edited settings file
// cannot point its writes at system folders.
bool IsInsideUserProfile(const std::wstring& path);

// Restricts an open file to Administrators and SYSTEM (used for process dumps written while
// elevated: a dump holds the target's memory, which other programs must not read).
bool RestrictToAdministrators(HANDLE file);

} // namespace nx
