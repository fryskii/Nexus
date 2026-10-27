cmake_minimum_required(VERSION 3.21)
project(NEXUS VERSION 0.9.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEXUS_BUILD_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(NEXUS_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000B
    NEXUS_VERSION_STR="${PROJECT_VERSION}")

# ---- Core library: platform-neutral logic (unit-testable on any host) ----
add_library(nexus_core STATIC
  src/core/Format.cpp
  src/core/Settings.cpp
  src/core/Smbios.cpp
  src/core/DiagnosticsRules.cpp
  src/core/Filter.cpp
)
target_include_directories(nexus_core PUBLIC src)
target_compile_definitions(nexus_core PUBLIC NEXUS_VERSION_STR="${PROJECT_VERSION}")

if(WIN32)
  set(NEXUS_SOURCES
    src/main.cpp
    src/app/App.cpp
    src/app/Commands.cpp
    src/app/Tray.cpp
    src/app/MiniMonitor.cpp
    src/core/Win.cpp
    src/data/Pdh.cpp
    src/data/Sampler.cpp
    src/data/Processes.cpp
    src/data/ProcessMeta.cpp
    src/data/ProcessDetails.cpp
    src/data/ProcessActions.cpp
    src/data/CpuInfo.cpp
    src/data/MemoryInfo.cpp
    src/data/DiskInfo.cpp
    src/data/NetInfo.cpp
    src/data/GpuInfo.cpp
    src/data/Services.cpp
    src/data/Startup.cpp
    src/data/Users.cpp
    src/data/SystemInfo.cpp
    src/data/EventLog.cpp
    src/data/Connections.cpp
    src/data/NetTools.cpp
    src/data/History.cpp
    src/data/Worker.cpp
    src/ui/Gfx.cpp
    src/ui/Theme.cpp
    src/ui/Table.cpp
    src/ui/Graph.cpp
    src/ui/Widgets.cpp
    src/ui/IconCache.cpp
    src/ui/Accessibility.cpp
    src/ui/pages/OverviewPage.cpp
    src/ui/pages/ProcessesPage.cpp
    src/ui/pages/PerformancePage.cpp
    src/ui/pages/AppHistoryPage.cpp
    src/ui/pages/StartupPage.cpp
    src/ui/pages/UsersPage.cpp
    src/ui/pages/DetailsPage.cpp
    src/ui/pages/ServicesPage.cpp
    src/ui/pages/HardwarePage.cpp
    src/ui/pages/NetworkPage.cpp
    src/ui/pages/StoragePage.cpp
    src/ui/pages/DiagnosticsPage.cpp
    src/ui/pages/SettingsPage.cpp
    src/ui/ProcessWindow.cpp
    res/nexus.rc
  )
  add_executable(nexus WIN32 ${NEXUS_SOURCES})
  set_target_properties(nexus PROPERTIES OUTPUT_NAME "NEXUS")
  target_include_directories(nexus PRIVATE src)
  target_compile_definitions(nexus PRIVATE ${NEXUS_DEFS})
  target_link_libraries(nexus PRIVATE nexus_core
    d2d1 dwrite windowscodecs dwmapi uxtheme comctl32 shlwapi shell32 ole32 oleaut32 uuid
    pdh iphlpapi ws2_32 wtsapi32 wintrust crypt32 dbghelp psapi version advapi32
    wevtapi setupapi dxgi powrprof userenv comdlg32 ntdll)

  if(MSVC)
    target_compile_options(nexus PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    target_link_options(nexus PRIVATE /MANIFEST:NO)
    set_property(TARGET nexus nexus_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(nexus PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(nexus PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
      -Wno-cast-function-type -municode)
    target_link_options(nexus PRIVATE -municode -static -static-libgcc -static-libstdc++ -s)
  endif()
endif()

if(NEXUS_BUILD_TESTS)
  enable_testing()
  add_executable(nexus_tests tests/tests.cpp)
  target_link_libraries(nexus_tests PRIVATE nexus_core)
  if(WIN32 AND NOT MSVC)
    target_link_options(nexus_tests PRIVATE -static)
  endif()
  add_test(NAME nexus_tests COMMAND nexus_tests)
endif()
