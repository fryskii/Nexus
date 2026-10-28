// Windows-only tests for the elevated-write protections (SafeFile) and the related path checks.
// They stage the redirection attacks for real (junctions, hard links, symbolic links) in a scratch
// folder and check that writes through them are refused and the "protected" file is untouched.
// Run elevated: the protections only apply when NEXUS is elevated (they are no-ops otherwise).
#include "core/Format.h"
#include "core/SafeFile.h"
#include "core/Win.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winioctl.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace nx;

static int g_failed = 0, g_passed = 0, g_skipped = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) ++g_passed;                                                      \
        else { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

std::wstring g_root;

std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    std::string s(1 << 16, '\0');
    DWORD n = 0;
    ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
    CloseHandle(h);
    s.resize(n);
    return s;
}

void WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr);
    CloseHandle(h);
}

// Directory junction (mount point) from `link` to `target`, created with FSCTL_SET_REPARSE_POINT.
bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring sub = L"\\??\\" + target;
    std::vector<BYTE> buf(8 + 8 + (sub.size() + 1 + 1) * sizeof(wchar_t));
    auto* p = buf.data();
    *reinterpret_cast<DWORD*>(p) = IO_REPARSE_TAG_MOUNT_POINT;
    *reinterpret_cast<WORD*>(p + 4) = static_cast<WORD>(buf.size() - 8);
    WORD* hdr = reinterpret_cast<WORD*>(p + 8);
    hdr[0] = 0;                                                          // SubstituteNameOffset
    hdr[1] = static_cast<WORD>(sub.size() * sizeof(wchar_t));            // SubstituteNameLength
    hdr[2] = static_cast<WORD>((sub.size() + 1) * sizeof(wchar_t));      // PrintNameOffset
    hdr[3] = 0;                                                          // PrintNameLength
    memcpy(p + 16, sub.c_str(), (sub.size() + 1) * sizeof(wchar_t));
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, p, static_cast<DWORD>(buf.size()), nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

std::wstring Fresh(const wchar_t* name) {
    std::wstring d = g_root + L"\\" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void TestPlainWrite() {
    std::wstring dir = Fresh(L"plain");
    std::wstring f = dir + L"\\new.txt";
    DeleteFileW(f.c_str());
    SafeWriteGuard g;
    CHECK(g.Prepare(f));
    HANDLE h = g.Open(f, GENERIC_WRITE, CREATE_NEW);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    // Appending to an ordinary existing file is fine.
    HANDLE a = g.Open(f, FILE_APPEND_DATA, OPEN_ALWAYS);
    CHECK(a != INVALID_HANDLE_VALUE);
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    // In-place truncation is never allowed when elevated.
    CHECK(g.Open(f, GENERIC_WRITE, CREATE_ALWAYS) == INVALID_HANDLE_VALUE);
}

void TestJunctionInPath(const std::wstring& protectedDir, const std::wstring& protectedFile) {
    std::wstring link = g_root + L"\\junction";
    RemoveDirectoryW(link.c_str());
    if (!MakeJunction(link, protectedDir)) { ++g_skipped; std::printf("skip: junctions not supported here\n"); return; }
    std::wstring through = link + L"\\victim.txt";
    SafeWriteGuard g;
    std::wstring why;
    CHECK(!g.Prepare(through, &why)); // refused: a folder on the path is a junction
    CHECK(ReadAll(protectedFile) == "original");
    RemoveDirectoryW(link.c_str());
}

void TestHardLink(const std::wstring& protectedFile) {
    std::wstring dir = Fresh(L"hardlink");
    std::wstring link = dir + L"\\history.csv";
    DeleteFileW(link.c_str());
    if (!CreateHardLinkW(link.c_str(), protectedFile.c_str(), nullptr)) { ++g_skipped; std::printf("skip: hard links not supported here\n"); return; }
    SafeWriteGuard g;
    CHECK(g.Prepare(link));
    HANDLE h = g.Open(link, FILE_APPEND_DATA, OPEN_ALWAYS); // the recorder's append pattern
    CHECK(h == INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(ReadAll(protectedFile) == "original");
    DeleteFileW(link.c_str());
}

void TestSymlinkFile(const std::wstring& protectedFile) {
    std::wstring dir = Fresh(L"symlink");
    std::wstring link = dir + L"\\dump.DMP";
    DeleteFileW(link.c_str());
    DWORD attr = CreateSymbolicLinkW(link.c_str(), protectedFile.c_str(), 0) ? GetFileAttributesW(link.c_str()) : INVALID_FILE_ATTRIBUTES;
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // Needs Developer Mode or elevation on Windows; Wine reports success without creating a link.
        ++g_skipped;
        std::printf("skip: symbolic links not supported here\n");
        DeleteFileW(link.c_str());
        return;
    }
    SafeWriteGuard g;
    CHECK(g.Prepare(link));
    CHECK(g.Open(link, GENERIC_WRITE, CREATE_NEW) == INVALID_HANDLE_VALUE);    // name taken: no write
    CHECK(g.Open(link, FILE_APPEND_DATA, OPEN_ALWAYS) == INVALID_HANDLE_VALUE); // opened as the link itself: refused
    CHECK(ReadAll(protectedFile) == "original");
    DeleteFileW(link.c_str());
}

void TestPaths() {
    SafeWriteGuard g;
    CHECK(!g.Prepare(L"\\\\localhost\\C$\\Windows\\x.txt"));
    CHECK(!g.Prepare(L"\\\\?\\GLOBALROOT\\Device\\x"));
    PWSTR p = nullptr;
    SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p);
    std::wstring profile = p ? p : L"";
    CoTaskMemFree(p);
    CHECK(IsInsideUserProfile(profile + L"\\AppData\\Local\\NEXUS\\History"));
    CHECK(!IsInsideUserProfile(profile + L"\\..\\..\\Windows\\System32"));
    CHECK(!IsInsideUserProfile(profile + L"x\\evil")); // sibling folder sharing the prefix
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    CHECK(!IsInsideUserProfile(win));
}

// Whether this file system keeps Windows permissions at all (Wine on some Linux file systems
// does not): sets a protected DACL with an independent method and reads it back.
bool FileSystemStoresAcls(const std::wstring& dir) {
    std::wstring f = dir + L"\\control.txt";
    DeleteFileW(f.c_str());
    WriteAll(f, "x");
    PSECURITY_DESCRIPTOR want = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;BA)(A;;FA;;;SY)", SDDL_REVISION_1, &want, nullptr)) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    GetSecurityDescriptorDacl(want, &present, &acl, &defaulted);
    SetNamedSecurityInfoW(const_cast<LPWSTR>(f.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(want);
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL got = nullptr;
    bool ok = GetNamedSecurityInfoW(f.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &got, nullptr, &sd) == ERROR_SUCCESS && got &&
              got->AceCount == 2;
    LocalFree(sd);
    DeleteFileW(f.c_str());
    return ok;
}

void TestDumpAcl() {
    if (!FileSystemStoresAcls(Fresh(L"dump"))) {
        ++g_skipped;
        std::printf("skip: this file system does not store Windows permissions\n");
        return;
    }
    std::wstring f = Fresh(L"dump") + L"\\test.DMP";
    DeleteFileW(f.c_str());
    SafeWriteGuard g;
    CHECK(g.Prepare(f));
    HANDLE h = g.Open(f, GENERIC_WRITE | WRITE_DAC, CREATE_NEW);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    CHECK(RestrictToAdministrators(h));
    CloseHandle(h);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(GetNamedSecurityInfoW(f.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0;
    GetSecurityDescriptorControl(sd, &ctl, &rev);
    CHECK(ctl & SE_DACL_PROTECTED); // no inherited access from the folder
    bool onlyAdmins = dacl && dacl->AceCount == 2;
    for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
        ACCESS_ALLOWED_ACE* ace = nullptr;
        GetAce(dacl, i, reinterpret_cast<void**>(&ace));
        PSID sid = reinterpret_cast<PSID>(&ace->SidStart);
        if (!IsWellKnownSid(sid, WinBuiltinAdministratorsSid) && !IsWellKnownSid(sid, WinLocalSystemSid)) onlyAdmins = false;
    }
    CHECK(onlyAdmins);
    LocalFree(sd);
    DeleteFileW(f.c_str());
}

} // namespace

int main() {
    if (!IsElevated()) {
        std::printf("These tests must run elevated (the protections only apply then).\n");
        return 2;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"nexus-security-tests";
    CreateDirectoryW(g_root.c_str(), nullptr);
    // Stand-in for a protected system file.
    std::wstring protectedDir = Fresh(L"protected");
    std::wstring protectedFile = protectedDir + L"\\victim.txt";
    WriteAll(protectedFile, "original");

    TestPlainWrite();
    TestJunctionInPath(protectedDir, protectedFile);
    TestHardLink(protectedFile);
    TestSymlinkFile(protectedFile);
    TestPaths();
    TestDumpAcl();
    std::printf("%d passed, %d failed, %d skipped\n", g_passed, g_failed, g_skipped);
    return g_failed ? 1 : 0;
}
