#!/usr/bin/env bash
# Cross-builds NEXUS for Windows x64 with MinGW-w64 and produces the portable ZIP and the installer.
# Requirements (Debian/Ubuntu): cmake ninja-build g++-mingw-w64-x86-64-posix nsis zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(NEXUS VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release -DNEXUS_BUILD_TESTS=ON
cmake --build "$BUILD"

rm -rf "$DIST"
mkdir -p "$DIST/NEXUS-$VERSION-portable-x64"
P="$DIST/NEXUS-$VERSION-portable-x64"
cp "$BUILD/NEXUS.exe" "$P/"
cp "$ROOT/LICENSE" "$P/LICENSE.txt"
cp "$ROOT/docs/USER_GUIDE.md" "$P/README.txt"
# The marker file makes NEXUS keep its settings next to the executable.
printf 'This file makes NEXUS store its settings in this folder (portable mode). Delete it to use %%APPDATA%%\\NEXUS.\r\n' > "$P/portable"
(cd "$DIST" && zip -qr "NEXUS-$VERSION-portable-x64.zip" "NEXUS-$VERSION-portable-x64")

makensis -V2 -DVERSION="$VERSION" -DSRCDIR="$BUILD" -DOUTFILE="$DIST/NEXUS-$VERSION-setup-x64.exe" "$ROOT/installer/nexus.nsi"
(cd "$DIST" && sha256sum NEXUS-*.zip NEXUS-*-setup-x64.exe > SHA256SUMS.txt)
echo "Artifacts in $DIST:"
ls -la "$DIST"
