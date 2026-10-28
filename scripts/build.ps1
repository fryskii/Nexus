# Builds NEXUS with MSVC (Visual Studio 2022, "Desktop development with C++") and runs the unit tests.
# Usage (from a "Developer PowerShell for VS 2022"):  .\scripts\build.ps1 [-Config Release] [-Package]
param([string]$Config = "Release", [switch]$Package)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 -DNEXUS_BUILD_TESTS=ON
cmake --build $build --config $Config --parallel
ctest --test-dir $build -C $Config --output-on-failure

if ($Package) {
    $version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(NEXUS VERSION ([0-9.]+)').Matches[0].Groups[1].Value
    $dist = Join-Path $root "dist"
    $portable = Join-Path $dist "NEXUS-$version-portable-x64"
    New-Item -ItemType Directory -Force -Path $portable | Out-Null
    Copy-Item (Join-Path $build "$Config\NEXUS.exe") $portable
    Copy-Item (Join-Path $root "LICENSE") (Join-Path $portable "LICENSE.txt")
    Copy-Item (Join-Path $root "res\fonts\Inter-OFL.txt") $portable
    Copy-Item (Join-Path $root "docs\USER_GUIDE.md") (Join-Path $portable "README.txt")
    Set-Content -Path (Join-Path $portable "portable") -Value "This file makes NEXUS store its settings in this folder (portable mode)."
    Compress-Archive -Force -Path $portable -DestinationPath "$portable.zip"
    $makensis = Get-Command makensis -ErrorAction SilentlyContinue
    if ($makensis) {
        & makensis "-DVERSION=$version" "-DSRCDIR=$build\$Config" "-DOUTFILE=$dist\NEXUS-$version-setup-x64.exe" (Join-Path $root "installer\nexus.nsi")
    } else {
        Write-Warning "NSIS (makensis) not found; skipping installer. Install it from https://nsis.sourceforge.io/"
    }
    Get-ChildItem $dist
}
