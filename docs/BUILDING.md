# Building NEXUS

NEXUS is a CMake project with no third-party dependencies. You need only the Windows SDK, which ships with Visual Studio or MinGW-w64.

## Windows, MSVC (primary toolchain)

Requirements: Visual Studio 2022 with *Desktop development with C++* (MSVC v143 and a Windows 11 SDK), and CMake 3.21 or later (bundled with VS).
Install NSIS 3 if you also want the installer.

```powershell
# From "Developer PowerShell for VS 2022"
.\scripts\build.ps1                # configure, build Release, run unit tests
.\scripts\build.ps1 -Package       # also create dist\*.zip and the installer
```

Manual equivalent:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build\Release\NEXUS.exe`. The static CRT (`/MT`) is used, so it runs without the VC++ redistributable.

## Windows, MinGW-w64 (no Visual Studio needed)

With a native MinGW-w64 toolchain that includes CMake and Ninja, for example WinLibs
(`winget install BrechtSanders.WinLibs.POSIX.UCRT`), from PowerShell with its `bin` folder on `PATH`:

```powershell
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
.\build-mingw\nexus_tests.exe
.\build-mingw\NEXUS.exe
```

The linker warns ".rsrc merge failure: multiple non-default manifests" because the toolchain adds a
default manifest; NEXUS's own manifest is the first one and is the one Windows uses. Close NEXUS
before rebuilding, or linking fails with "Permission denied".

## Linux, cross-compiling with MinGW-w64

This path was used to produce and test the binaries in this repository.

```bash
sudo apt install cmake ninja-build g++-mingw-w64-x86-64-posix nsis zip
./scripts/package.sh       # builds build-win/NEXUS.exe, dist/*.zip and dist/*-setup-x64.exe
```

Options:
- `-DNEXUS_STRIP=OFF` keeps symbols, which is useful with `x86_64-w64-mingw32-addr2line`.
- `-DNEXUS_BUILD_TESTS=OFF` skips the unit tests.

## Unit tests

`nexus_tests` covers the platform-neutral core: formatting, the settings store, the SMBIOS parser (including fuzzed truncation and corruption), the filter
language, the ring buffer and the diagnostics rule engine. It builds and runs on any host:

```bash
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build-host && ./build-host/nexus_tests
```

The Windows-specific layer is checked by running the application. `NEXUS.exe --bench <out.json> [iterations]` measures collection cost without a UI.

## Reproducibility

- Version: `project(NEXUS VERSION ...)` in `CMakeLists.txt` and the file version in `res/nexus.rc`. An optional label is
  added with `-DNEXUS_VERSION_LABEL=alpha.2` (or `NEXUS_VERSION_LABEL=alpha.2 ./scripts/package.sh`) and gives 0.9.0-alpha.2
  in the app and the file names. In Windows PowerShell, quote the whole argument (`"-DNEXUS_VERSION_LABEL=alpha.2"`);
  unquoted, the part after the dot is split off. CI takes the label from a release tag (v0.9.0-alpha.2) and uses
  `dev.<commit>` for other pushes.
- `scripts/package.sh` writes `dist/SHA256SUMS.txt`.
- No network access is needed at build time.
