#!/usr/bin/env bash
#
# Build musicchain-node (and mini-node) on Linux via vcpkg + CMake.
#
# Required env vars (no defaults — script aborts if unset):
#   VCPKG_ROOT           Path to vcpkg checkout
#
# Optional env vars (defaults shown):
#   CMAKE=cmake          cmake binary
#   BUILD_DIR=build-linux out-of-tree build dir
#   OUTPUT_DIR=$BUILD_DIR/Release
#   CLEAN=0              "1" wipes BUILD_DIR first
#   JOBS=$(nproc)        parallel build jobs
#
# Usage from musicchain root:
#   VCPKG_ROOT=/opt/vcpkg ./scripts/build-node-linux.sh

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT not set — point at your vcpkg checkout}"
CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
OUTPUT_DIR="${OUTPUT_DIR:-$BUILD_DIR/Release}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Strip any Windows PATH entries WSL inherits. Without this, cmake's
# find_package() will discover Windows mingw / msys64 toolchain configs
# (e.g. /mnt/c/msys64/mingw64/lib/cmake/CURL) and try to use Win32-only
# headers in our Linux build — which fails with "Only Win32 target is
# supported!" deep inside <sys/cdefs.h>. We also clear CMAKE_PREFIX_PATH
# and add CMAKE_IGNORE_PATH so vcpkg + the system Linux toolchain are
# the only sources of headers and libs.
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH
unset CMAKE_PREFIX_PATH

if [ "$CLEAN" = "1" ]; then
    echo "[clean] removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "[configure] $CMAKE -S . -B $BUILD_DIR"
    "$CMAKE" -S . -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_IGNORE_PATH=/mnt/c/msys64
fi

echo "[build] $CMAKE --build $BUILD_DIR -j$JOBS"
"$CMAKE" --build "$BUILD_DIR" --config Release -j"$JOBS"

mkdir -p "$OUTPUT_DIR"
for f in musicchain-node musicchain-mini-node libmusicchain.so; do
    src="$BUILD_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$OUTPUT_DIR/$f"
done
# librats shared lib
rats_so="$BUILD_DIR/deps/librats/lib/libmc_rats.so"
[ -f "$rats_so" ] && cp -f "$rats_so" "$OUTPUT_DIR/libmc_rats.so"

# Default config files — same shipping policy as the Windows build.
for cfg in full-node.config.json mini-node.config.json; do
    [ -f "config/$cfg" ] && cp -f "config/$cfg" "$OUTPUT_DIR/$cfg"
done

echo "[done] artifacts in $OUTPUT_DIR"
