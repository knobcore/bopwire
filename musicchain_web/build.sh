#!/usr/bin/env bash
# Build the WASM modules for the web player. Wraps emsdk activation +
# emcmake so a fresh clone just runs `bash build.sh` and gets
# wasm/wallet.js + wasm/wallet.wasm dropped into the served directory.
set -e
cd "$(dirname "$0")"

# Locate emsdk. Common spots first, then $EMSDK env var as override.
if [ -z "$EMSDK" ]; then
  for cand in "$HOME/emsdk" "/c/Users/$USER/emsdk" "/c/Users/lain/emsdk" "/opt/emsdk"; do
    if [ -f "$cand/emsdk_env.sh" ]; then EMSDK="$cand"; break; fi
  done
fi
if [ -z "$EMSDK" ] || [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "[build] EMSDK not found. Clone emsdk into ~/emsdk and run:" >&2
  echo "          ./emsdk install latest && ./emsdk activate latest" >&2
  exit 1
fi

# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh"
echo "[build] using $(emcc --version | head -1)"

# Best-effort clean. `rm -rf build` fails on Windows when another shell
# holds build/ as cwd; wiping contents is enough since the empty dir is
# fine for emcmake to reuse.
( shopt -s dotglob nullglob; rm -rf build/* 2>/dev/null || true )
mkdir -p build wasm
cd build
emcmake cmake -DCMAKE_BUILD_TYPE=Release ..
emmake make -j"$(nproc 2>/dev/null || echo 4)"

cd ..
echo "[build] artifacts:"
ls -la wasm/wallet.* 2>/dev/null || echo "  (wallet.* missing — check link errors above)"
