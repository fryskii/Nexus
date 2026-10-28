// PE load configuration for MinGW builds (MSVC builds get the same effect from
// /DEPENDENTLOADFLAG:0x800 in CMakeLists.txt).
//
// DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32 makes the Windows loader (Windows 10 1607+)
// resolve NEXUS's statically imported DLLs from System32 only, never from the folder NEXUS.exe was
// started from. Without it, a DLL such as version.dll or dwmapi.dll planted next to NEXUS.exe (for
// example in Downloads) would run inside NEXUS, including when it runs elevated or replaces Task
// Manager. SetDefaultDllDirectories in wWinMain covers later LoadLibrary calls, but runs too late
// for static imports.
#if defined(__MINGW32__) && defined(_WIN64)
#include <cstddef>
#include <cstdint>

namespace {
// Layout of IMAGE_LOAD_CONFIG_DIRECTORY64 up to SEHandlerCount (the MinGW headers predate the
// DependentLoadFlags name for the WORD at offset 0x4E, formerly Reserved1).
struct LoadConfig64 {
    uint32_t Size;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t GlobalFlagsClear;
    uint32_t GlobalFlagsSet;
    uint32_t CriticalSectionDefaultTimeout;
    uint64_t DeCommitFreeBlockThreshold;
    uint64_t DeCommitTotalFreeThreshold;
    uint64_t LockPrefixTable;
    uint64_t MaximumAllocationSize;
    uint64_t VirtualMemoryThreshold;
    uint64_t ProcessAffinityMask;
    uint32_t ProcessHeapFlags;
    uint16_t CSDVersion;
    uint16_t DependentLoadFlags;
    uint64_t EditList;
    uint64_t SecurityCookie;
    uint64_t SEHandlerTable;
    uint64_t SEHandlerCount;
};
static_assert(offsetof(LoadConfig64, DependentLoadFlags) == 0x4E, "DependentLoadFlags must be at offset 0x4E");
static_assert(sizeof(LoadConfig64) == 0x70, "unexpected load config size");
} // namespace

// The linker points the PE load-configuration data directory at this symbol.
extern "C" __attribute__((used)) const LoadConfig64 _load_config_used = {
    sizeof(LoadConfig64), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x0800, // LOAD_LIBRARY_SEARCH_SYSTEM32
    0, 0, 0, 0,
};
#endif
