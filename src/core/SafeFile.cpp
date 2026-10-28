#include "core/SafeFile.h"

#include <aclapi.h>
#include <shlobj.h>

namespace nx {

namespace {

// Junctions and symbolic links redirect a path somewhere else; other reparse points (OneDrive and
// other cloud placeholders, deduplicated files) do not.
bool IsRedirecting(HANDLE h) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) return true; // unknown: treat as unsafe
    return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag);
}

std::wstring FullPath(const std::wstring& path) {
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
    out.resize(n);
    return out;
}

// Path of an open file or folder as the file system resolved it, without the \\?\ prefix.
std::wstring FinalPath(HANDLE h) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring p(buf, n);
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

void SetWhy(std::wstring* why, const std::wstring& text) {
    if (why) *why = text;
}

} // namespace

SafeWriteGuard::SafeWriteGuard() : elevated_(IsElevated()) {}

SafeWriteGuard::~SafeWriteGuard() {
    for (HANDLE h : dirs_) CloseHandle(h);
}

bool SafeWriteGuard::Prepare(const std::wstring& path, std::wstring* why) {
    if (!elevated_) return true;
    std::wstring full = FullPath(path);
    // Drive-letter paths only: UNC paths (including \\localhost\C$) and device paths are refused.
    if (full.size() < 4 || full[1] != L':' || full[2] != L'\\') {
        SetWhy(why, L"NEXUS is running as administrator and only saves to local folders (not " + path + L").");
        return false;
    }
    size_t last = full.find_last_of(L'\\');
    // Every folder from the drive root down to the file's folder.
    for (size_t end = 2; end != std::wstring::npos && end <= last; end = full.find(L'\\', end + 1)) {
        std::wstring dir = full.substr(0, end == 2 ? 3 : end); // "C:\" for the root
        HANDLE h = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            SetWhy(why, L"Cannot open " + dir + L": " + LastErrorMessage());
            return false;
        }
        dirs_.push_back(h); // held without delete sharing: the folder cannot be renamed or replaced meanwhile
        if (IsRedirecting(h)) {
            SetWhy(why, L"NEXUS is running as administrator and will not save through the link " + dir + L".");
            return false;
        }
        if (end == last) break;
    }
    finalDir_ = dirs_.empty() ? std::wstring() : FinalPath(dirs_.back());
    if (finalDir_.empty()) {
        SetWhy(why, L"Cannot resolve the folder of " + path);
        return false;
    }
    return true;
}

HANDLE SafeWriteGuard::Open(const std::wstring& path, DWORD access, DWORD disposition, std::wstring* why) {
    if (!elevated_) {
        HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) SetWhy(why, LastErrorMessage());
        return h;
    }
    if (disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) {
        // Truncation happens inside CreateFileW, before the handle could be checked.
        SetWhy(why, L"Internal error: unsafe file disposition.");
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SetWhy(why, LastErrorMessage());
        return h;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    std::wstring opened = FinalPath(h);
    size_t slash = opened.find_last_of(L'\\');
    std::wstring openedDir = slash == std::wstring::npos ? std::wstring() : opened.substr(0, slash == 2 ? 3 : slash);
    if (IsRedirecting(h) || !GetFileInformationByHandle(h, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || finalDir_.empty() || _wcsicmp(openedDir.c_str(), finalDir_.c_str()) != 0) {
        CloseHandle(h);
        SetWhy(why, L"NEXUS is running as administrator and will not write to " + path + L" because it is a link.");
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

bool IsInsideUserProfile(const std::wstring& path) {
    PWSTR profile = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) return false;
    std::wstring root = FullPath(profile);
    CoTaskMemFree(profile);
    std::wstring full = FullPath(path); // resolves "..", so C:\Users\me\..\..\Windows does not pass
    if (root.empty() || full.empty()) return false;
    if (root.back() != L'\\') root += L'\\';
    if (full.back() != L'\\') full += L'\\';
    return full.size() >= root.size() && _wcsnicmp(full.c_str(), root.c_str(), root.size()) == 0;
}

bool RestrictToAdministrators(HANDLE file) {
    BYTE adminSid[SECURITY_MAX_SID_SIZE], systemSid[SECURITY_MAX_SID_SIZE];
    DWORD a = sizeof adminSid, s = sizeof systemSid;
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &a) || !CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &s))
        return false;
    EXPLICIT_ACCESSW ea[2] = {};
    PSID sids[2] = {adminSid, systemSid};
    for (int i = 0; i < 2; ++i) {
        ea[i].grfAccessPermissions = FILE_ALL_ACCESS;
        ea[i].grfAccessMode = SET_ACCESS;
        ea[i].grfInheritance = NO_INHERITANCE;
        ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[i].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
    }
    PACL acl = nullptr;
    if (SetEntriesInAclW(2, ea, nullptr, &acl) != ERROR_SUCCESS) return false;
    DWORD r = SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return r == ERROR_SUCCESS;
}

} // namespace nx
