#!/usr/bin/env bash
#
# Install bopwire full-node build dependencies on an Arch-flavoured
# distro (Arch Linux, Manjaro, EndeavourOS, …).
#
# Same dep set as install-deps-debian.sh; only the package names differ.
# Almost everything is VENDORED + built static from deps/ (leveldb, miniupnpc,
# ogg, vorbis, opus, opusfile, chromaprint, nlohmann, librats, libwally,
# croaring, cpp-httplib, sacad). pacman only needs the deliberately-system
# deps: openssl, ffmpeg, ncurses + the toolchain.
#
# Usage (run as root or with sudo):
#   sudo bash scripts/install-deps-arch.sh
#
# Optional env vars:
#   PACMAN_OPTS             extra args (e.g. --noconfirm)

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "[install] re-running with sudo"
    exec sudo bash "$0" "$@"
fi

PACMAN_OPTS="${PACMAN_OPTS:---noconfirm --needed}"

PKGS=(
    # Build toolchain
    base-devel cmake git pkgconf
    # Cryptography + TLS — kept on the system (trusted; OS security updates)
    openssl
    # FFmpeg suite — kept on the system (decode-only; huge to vendor). Arch
    # ships a single ffmpeg package with libav{codec,format,util} + libswresample.
    ffmpeg
    # TUI — kept on the system (present on every Linux)
    ncurses
    # NB: leveldb, miniupnpc, ogg, vorbis, opus, opusfile, chromaprint and
    #     nlohmann-json are VENDORED + built static (deps/vendored.cmake) —
    #     no system packages for them are needed any more.
)

echo "[install] pacman -S ${PKGS[*]}"
pacman -S $PACMAN_OPTS "${PKGS[@]}"

echo "[install] done. Now build with:"
echo "    bash scripts/build-node-linux.sh"
