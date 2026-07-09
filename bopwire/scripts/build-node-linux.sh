#!/usr/bin/env bash
#
# Build bopwire-node + mini-node + libbopwire.so on Linux using
# VENDORED deps built static (NO vcpkg).
#
# Run install-deps-debian.sh or install-deps-arch.sh first — but that now
# installs ONLY the deliberately-system deps (OpenSSL, ffmpeg, ncurses +
# toolchain). Everything else (leveldb, miniupnpc, ogg, vorbis, opus,
# opusfile, chromaprint, nlohmann, librats, libwally, croaring, cpp-httplib,
# sacad) is compiled from deps/ and linked static — see deps/vendored.cmake —
# so the resulting binary is hermetic.
#
# Optional env vars (defaults shown):
#   CMAKE=cmake
#   BUILD_DIR=build-linux
#   OUTPUT_DIR=$BUILD_DIR/Release
#   CLEAN=0                "1" wipes BUILD_DIR first
#   JOBS=$(nproc)

set -euo pipefail

CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
OUTPUT_DIR="${OUTPUT_DIR:-$BUILD_DIR/Release}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Strip /mnt/c PATH entries that WSL inherits from Windows — otherwise
# cmake's find_package can pick up Windows mingw toolchain configs
# (e.g. /mnt/c/msys64/mingw64/lib/cmake/CURL) which then drag Win32-only
# sys/cdefs.h into a Linux build. Harmless on a real Linux host where
# /mnt/c doesn't exist; necessary in WSL.
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH
unset CMAKE_PREFIX_PATH

# Rust/cargo is REQUIRED here: the node's real album-art scraper (vendored sacad)
# is a cargo-built static lib. Put ~/.cargo on PATH so cmake's find_program(cargo)
# succeeds. Combined with -DMC_REQUIRE_ART=ON below, a shell without cargo now
# FAILS the configure loudly instead of silently producing a node with no covers
# (which is exactly how the live node once shipped art-less).
if [ -f "$HOME/.cargo/env" ]; then . "$HOME/.cargo/env"; fi

if [ "$CLEAN" = "1" ]; then
    echo "[clean] removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Always (re)configure with album art REQUIRED. Re-running configure on an
# existing build dir is cheap and idempotent, and it guarantees that a stale
# cache once generated without cargo (art silently OFF) is corrected here — an
# incremental `cmake --build` alone would keep shipping an art-less node.
echo "[configure] $CMAKE -S . -B $BUILD_DIR (album art required)"
"$CMAKE" -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMC_REQUIRE_ART=ON \
    -DCMAKE_IGNORE_PATH=/mnt/c/msys64

echo "[build] $CMAKE --build $BUILD_DIR -j$JOBS"
"$CMAKE" --build "$BUILD_DIR" --config Release -j"$JOBS"

mkdir -p "$OUTPUT_DIR"
for f in bopwire-node bopwire-mini-node libbopwire.so; do
    src="$BUILD_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$OUTPUT_DIR/$f"
done
# librats shared lib
rats_so="$BUILD_DIR/deps/librats/lib/libmc_rats.so"
[ -f "$rats_so" ] && cp -f "$rats_so" "$OUTPUT_DIR/libmc_rats.so"

# Default config files shipped alongside binaries.
for cfg in full-node.config.json mini-node.config.json; do
    [ -f "config/$cfg" ] && cp -f "config/$cfg" "$OUTPUT_DIR/$cfg"
done

echo "[done] artifacts in $OUTPUT_DIR"
