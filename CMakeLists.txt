cmake_minimum_required(VERSION 3.21)
project(NEXUS VERSION 0.9.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEXUS_BUILD_TESTS "Build unit tests" ON)
option(NEXUS_STRIP "Strip symbols from release binaries (MinGW)" ON)

# Static C runtime everywhere (no VC++ redistributable needed; all targets must match to link).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Sources are UTF-8 (°, —, …): without this MSVC reads them in the local ANSI code page.
if(MSVC)
  add_compile_options(/utf-8)
endif()

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
  src/core/Regex.cpp
)
target_include_directories(nexus_core PUBLIC src)
target_compile_definitions(nexus_core PUBLIC NEXUS_VERSION_STR="${PROJECT_VERSION}")

if(WIN32)
  set(NEXUS_SOURCES
    src/main.cpp
    src/app/LoadConfig.cpp
    src/app/App.cpp
    src/app/Chrome.cpp
    src/app/Commands.cpp
    src/app/Stress.cpp
    src/app/Tray.cpp
    src/app/MiniMonitor.cpp
    src/core/SafeFile.cpp
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
  set_source_files_properties(res/nexus.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/Inter-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/Inter-SemiBold.ttf;${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/InterDisplay-SemiBold.ttf;${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/Inter-OFL.txt")
  set_target_properties(nexus PROPERTIES OUTPUT_NAME "NEXUS")
  target_include_directories(nexus PRIVATE src)
  target_compile_definitions(nexus PRIVATE ${NEXUS_DEFS})
  # DLLs that are not on Windows' KnownDLLs list are delay-loaded: they are loaded on first use with
  # LoadLibrary, which only searches System32 (SetDefaultDllDirectories in wWinMain). Linked
  # normally, Windows would look for them next to NEXUS.exe first, so a planted DLL (for example
  # version.dll in Downloads) would run inside NEXUS. The functions used from each are listed in
  # cmake/delayload/<dll>.def; a new function from these DLLs is a link error until it is added there.
  set(NEXUS_DELAYLOAD crypt32 dwrite iphlpapi powrprof uxtheme version wintrust wtsapi32 d2d1 dbghelp dwmapi dxgi pdh wevtapi secur32)
  target_link_libraries(nexus PRIVATE nexus_core
    comctl32 shlwapi shell32 ole32 oleaut32 uuid ws2_32 psapi advapi32 setupapi comdlg32 ntdll)
  if(MSVC)
    target_link_libraries(nexus PRIVATE ${NEXUS_DELAYLOAD} delayimp)
    foreach(dll ${NEXUS_DELAYLOAD})
      target_link_options(nexus PRIVATE /DELAYLOAD:${dll}.dll)
    endforeach()
  else()
    find_program(NEXUS_DLLTOOL NAMES x86_64-w64-mingw32-dlltool dlltool REQUIRED)
    set(delay_libs)
    foreach(dll ${NEXUS_DELAYLOAD})
      set(out "${CMAKE_CURRENT_BINARY_DIR}/delayload/lib${dll}_delay.a")
      add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/delayload"
        COMMAND "${NEXUS_DLLTOOL}" --input-def "${CMAKE_CURRENT_SOURCE_DIR}/cmake/delayload/${dll}.def"
                --output-delaylib "${out}" --dllname ${dll}.dll
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/delayload/${dll}.def" VERBATIM)
      list(APPEND delay_libs "${out}")
    endforeach()
    add_custom_target(nexus_delayload DEPENDS ${delay_libs})
    add_dependencies(nexus nexus_delayload)
    target_link_libraries(nexus PRIVATE ${delay_libs})
  endif()

  if(MSVC)
    target_compile_options(nexus PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    # Resolve static imports from System32 only (no DLL planting next to NEXUS.exe); see LoadConfig.cpp.
    target_link_options(nexus PRIVATE /DEPENDENTLOADFLAG:0x800)
    target_link_options(nexus PRIVATE /MANIFEST:NO)
    target_compile_definitions(nexus PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(nexus PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
      -Wno-cast-function-type -municode)
    target_link_options(nexus PRIVATE -municode -static -static-libgcc -static-libstdc++ $<$<BOOL:${NEXUS_STRIP}>:-s>)
    # GNU ld < 2.42 does not set the PE load-config directory, so point it at _load_config_used
    # (DependentLoadFlags = System32 only; see src/app/LoadConfig.cpp). Required: without it,
    # DLLs planted next to NEXUS.exe would be loaded.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(TARGET nexus POST_BUILD
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pe_load_config.py" "$<TARGET_FILE:nexus>"
      VERBATIM)
  endif()
endif()

if(NEXUS_BUILD_TESTS)
  enable_testing()
  add_executable(nexus_tests tests/tests.cpp)
  target_link_libraries(nexus_tests PRIVATE nexus_core)
  if(WIN32)
    target_link_libraries(nexus_tests PRIVATE shell32)
  endif()
  if(WIN32 AND NOT MSVC)
    target_link_options(nexus_tests PRIVATE -static)
  endif()
  add_test(NAME nexus_tests COMMAND nexus_tests)
  if(WIN32)
    # Elevated-write protections, tested against real junction / hard-link / symlink attacks (run elevated).
    add_executable(nexus_security_tests tests/security_tests.cpp src/core/SafeFile.cpp src/core/Win.cpp)
    target_include_directories(nexus_security_tests PRIVATE src)
    target_compile_definitions(nexus_security_tests PRIVATE ${NEXUS_DEFS})
    target_link_libraries(nexus_security_tests PRIVATE nexus_core advapi32 shell32 shlwapi ole32 version)
    if(NOT MSVC)
      target_link_options(nexus_security_tests PRIVATE -static)
    endif()
    # Process-churn load generator for stress runs (see docs/STRESS_TESTING.md).
    add_executable(nexus_churn tests/stress/churn.cpp)
    target_compile_definitions(nexus_churn PRIVATE UNICODE _UNICODE)
    if(NOT MSVC)
      target_compile_options(nexus_churn PRIVATE -municode)
      target_link_options(nexus_churn PRIVATE -municode -static)
    endif()
  endif()
endif()
