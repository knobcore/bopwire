#!/usr/bin/env bash
#
# Install bopwire full-node build dependencies on a Debian-flavoured
# distro (Debian, Ubuntu, Mint, Pop!_OS, Raspberry Pi OS, …).
#
# Almost everything is VENDORED + built static from deps/ (see
# deps/vendored.cmake) so a clone compiles a hermetic binary: leveldb,
# miniupnpc, ogg, vorbis(+file), opus, opusfile, chromaprint, nlohmann,
# librats, libwally-core, croaring, cpp-httplib, sacad. This script therefore
# installs ONLY the deliberately-system deps:
#   - OpenSSL   (trusted crypto; OS security updates land without a rebuild)
#   - ffmpeg    (decode-only; enormous external codec tree to vendor)
#   - ncurses   (node-only interactive TUI; on every Linux)
#   + the build toolchain.
#
# Usage (run as root or with sudo):
#   sudo bash scripts/install-deps-debian.sh
#
# Optional env vars:
#   APT_OPTS                extra args passed to apt-get (e.g. -y --no-install-recommends)

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "[install] re-running with sudo"
    exec sudo bash "$0" "$@"
fi

APT_OPTS="${APT_OPTS:--y}"

echo "[install] apt-get update"
apt-get update -qq

PKGS=(
    # Build toolchain
    build-essential cmake git pkg-config
    # Cryptography + TLS — kept on the system (trusted; OS security updates)
    libssl-dev
    # FFmpeg suite (covers FLAC / MP3 / Opus / AAC / WAV decode) — kept on the
    # system (decode-only + huge external codec tree to vendor)
    libavcodec-dev libavformat-dev libavutil-dev libswresample-dev
    # TUI — kept on the system (present on every Linux)
    libncurses-dev
    # NB: leveldb, miniupnpc, ogg, vorbis, opus, opusfile, chromaprint and
    #     nlohmann-json are VENDORED + built static (deps/vendored.cmake) —
    #     no -dev packages for them are needed any more.
)

echo "[install] installing: ${PKGS[*]}"
apt-get install $APT_OPTS "${PKGS[@]}"

echo "[install] done. Now build with:"
echo "    bash scripts/build-node-linux.sh"
