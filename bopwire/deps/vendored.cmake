# =====================================================================
# Vendored third-party dependencies — built from source as STATIC libs.
#
# Everything here compiles straight from deps/<lib>/ so a fresh `git clone`
# produces a hermetic binary: no system shared-library version can ever break
# us ("dependency hell"), and anyone who compiles the project compiles these
# with it. Included from the top-level CMakeLists.
#
# Kept on the SYSTEM on purpose (NOT vendored):
#   * OpenSSL  — trusted crypto; stays on the OS libs so security patches land
#                without a rebuild.
#   * ffmpeg   — decode-only + an enormous external codec tree; the node and
#                player build and run on controlled boxes.
#   * ncurses  — node-only interactive TUI; autotools/terminfo, and present on
#                every Linux.
#
# Vendored + static here: leveldb, miniupnpc, ogg, vorbis(+file), opus,
# opusfile, chromaprint (with chromaprint's bundled kissfft).
# =====================================================================

set(_BW_DEPS ${CMAKE_CURRENT_LIST_DIR})   # absolute path of <src>/deps

# Force every sub-build below to produce a static archive.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Hide the vendored libs' symbols from the final binary's dynamic symbol table
# so they do NOT interpose the SYSTEM ffmpeg's own dynamic ogg/vorbis/opus/
# chromaprint (which it drags in as DT_NEEDED). Without this, ELF global-symbol
# interposition makes libavcodec bind its ov_open/vorbis_synthesis/opus_decode
# calls to OUR static copies — re-introducing the exact version dependency we
# are removing. Our own code still binds these statically at link time; ffmpeg
# keeps using its own .so. (No-op on Windows/PE — each module imports its own.)
# Saved + restored around the vendored targets so our mc_* FFI exports on
# libbopwire stay visible.
set(_BW_SAVE_CVIS   "${CMAKE_C_VISIBILITY_PRESET}")
set(_BW_SAVE_CXXVIS "${CMAKE_CXX_VISIBILITY_PRESET}")
set(_BW_SAVE_INLVIS "${CMAKE_VISIBILITY_INLINES_HIDDEN}")
set(CMAKE_C_VISIBILITY_PRESET       hidden)
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# opus/chromaprint predate CMake 3.3, so their CMP0063 defaults to OLD, which
# IGNORES the visibility presets for STATIC libraries — leaving opus_*/
# chromaprint_* exported. Force NEW so the presets are honored everywhere.
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)

# ---- leveldb (chain DB) — no snappy / crc32c-hw / tcmalloc (hermetic) -------
# Pre-seed the feature probes so leveldb never links a *system* snappy etc.
# (its check_library_exists runs before a vendored copy could exist, and we
# wipe to a fresh DB so compression is moot).
set(LEVELDB_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL          OFF CACHE BOOL "" FORCE)
set(HAVE_SNAPPY   0 CACHE INTERNAL "vendored: no snappy")
set(HAVE_CRC32C   0 CACHE INTERNAL "vendored: no hw crc32c")
set(HAVE_TCMALLOC 0 CACHE INTERNAL "vendored: no tcmalloc")
add_subdirectory(${_BW_DEPS}/leveldb ${CMAKE_BINARY_DIR}/deps/leveldb EXCLUDE_FROM_ALL)
set(LEVELDB_LIBRARIES    leveldb)
set(LEVELDB_INCLUDE_DIRS ${_BW_DEPS}/leveldb/include)
set(LEVELDB_FOUND TRUE)

# ---- miniupnpc (UPnP IGD port mapping) -------------------------------------
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL   ON  CACHE BOOL "" FORCE)
add_subdirectory(${_BW_DEPS}/miniupnpc ${CMAKE_BINARY_DIR}/deps/miniupnpc EXCLUDE_FROM_ALL)
set(MINIUPNPC_LIBRARIES    miniupnpc::miniupnpc)
set(MINIUPNPC_INCLUDE_DIRS ${_BW_DEPS}/miniupnpc/include)
set(MINIUPNPC_FOUND TRUE)

# ---- opus (decode) ---------------------------------------------------------
set(OPUS_BUILD_SHARED_LIBRARY        OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING               OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS              OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${_BW_DEPS}/opus ${CMAKE_BINARY_DIR}/deps/opus EXCLUDE_FROM_ALL)
set(OPUS_LIBRARIES    opus)
set(OPUS_INCLUDE_DIRS ${_BW_DEPS}/opus/include)

# ---- ogg (hand-rolled: 2 sources; config_types.h is vendored) --------------
add_library(ogg STATIC ${_BW_DEPS}/ogg/src/bitwise.c ${_BW_DEPS}/ogg/src/framing.c)
target_include_directories(ogg PUBLIC ${_BW_DEPS}/ogg/include)
set(OGG_LIBRARIES    ogg)
set(OGG_INCLUDE_DIRS ${_BW_DEPS}/ogg/include)

# ---- vorbis + vorbisfile (hand-rolled; decode source set from libvorbis) ---
add_library(vorbis STATIC
  ${_BW_DEPS}/vorbis/lib/mdct.c      ${_BW_DEPS}/vorbis/lib/smallft.c
  ${_BW_DEPS}/vorbis/lib/block.c     ${_BW_DEPS}/vorbis/lib/envelope.c
  ${_BW_DEPS}/vorbis/lib/window.c    ${_BW_DEPS}/vorbis/lib/lsp.c
  ${_BW_DEPS}/vorbis/lib/lpc.c       ${_BW_DEPS}/vorbis/lib/analysis.c
  ${_BW_DEPS}/vorbis/lib/synthesis.c ${_BW_DEPS}/vorbis/lib/psy.c
  ${_BW_DEPS}/vorbis/lib/info.c      ${_BW_DEPS}/vorbis/lib/floor1.c
  ${_BW_DEPS}/vorbis/lib/floor0.c    ${_BW_DEPS}/vorbis/lib/res0.c
  ${_BW_DEPS}/vorbis/lib/mapping0.c  ${_BW_DEPS}/vorbis/lib/registry.c
  ${_BW_DEPS}/vorbis/lib/codebook.c  ${_BW_DEPS}/vorbis/lib/sharedbook.c
  ${_BW_DEPS}/vorbis/lib/lookup.c    ${_BW_DEPS}/vorbis/lib/bitrate.c)
target_include_directories(vorbis
  PUBLIC  ${_BW_DEPS}/vorbis/include
  PRIVATE ${_BW_DEPS}/vorbis/lib)
target_link_libraries(vorbis PUBLIC ogg)

add_library(vorbisfile STATIC ${_BW_DEPS}/vorbis/lib/vorbisfile.c)
target_include_directories(vorbisfile PUBLIC ${_BW_DEPS}/vorbis/include)
target_link_libraries(vorbisfile PUBLIC vorbis ogg)
set(VORBIS_LIBRARIES        vorbis)
set(VORBISFILE_LIBRARIES    vorbisfile)
set(VORBIS_INCLUDE_DIRS     ${_BW_DEPS}/vorbis/include)
set(VORBISFILE_INCLUDE_DIRS ${_BW_DEPS}/vorbis/include)

# ---- opusfile (hand-rolled; HTTP disabled → no sockets / TLS) --------------
add_library(opusfile STATIC
  ${_BW_DEPS}/opusfile/src/info.c     ${_BW_DEPS}/opusfile/src/internal.c
  ${_BW_DEPS}/opusfile/src/opusfile.c ${_BW_DEPS}/opusfile/src/stream.c)
target_include_directories(opusfile
  PUBLIC  ${_BW_DEPS}/opusfile/include
  PRIVATE ${_BW_DEPS}/opusfile/src ${_BW_DEPS}/opus/include)
target_link_libraries(opusfile PUBLIC opus ogg)
set(OPUSFILE_LIBRARIES    opusfile)
set(OPUSFILE_INCLUDE_DIRS ${_BW_DEPS}/opusfile/include)

# ---- chromaprint (static; uses its bundled kissfft, no ffmpeg/fftw) --------
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFT_LIB kissfft CACHE STRING "" FORCE)
# chromaprint's FindKissFFT looks under CMAKE_SOURCE_DIR by default (= our top
# tree under add_subdirectory) — point it at the bundled copy explicitly.
set(KISSFFT_ROOT ${_BW_DEPS}/chromaprint/src/3rdparty/kissfft CACHE PATH "" FORCE)
add_subdirectory(${_BW_DEPS}/chromaprint ${CMAKE_BINARY_DIR}/deps/chromaprint EXCLUDE_FROM_ALL)
set(CHROMAPRINT_LIBRARIES    chromaprint)
set(CHROMAPRINT_INCLUDE_DIRS ${_BW_DEPS}/chromaprint/src)
set(CHROMAPRINT_FOUND TRUE)

# Restore the parent's visibility so libbopwire keeps exporting its mc_* FFI
# symbols (the Flutter player loads them) and the node/mini binaries are normal.
# NB: an empty saved value means the var was UNSET — restore it by unsetting,
# not by set("") which is an illegal value for the *_VISIBILITY_PRESET property.
if(_BW_SAVE_CVIS STREQUAL "")
  unset(CMAKE_C_VISIBILITY_PRESET)
else()
  set(CMAKE_C_VISIBILITY_PRESET "${_BW_SAVE_CVIS}")
endif()
if(_BW_SAVE_CXXVIS STREQUAL "")
  unset(CMAKE_CXX_VISIBILITY_PRESET)
else()
  set(CMAKE_CXX_VISIBILITY_PRESET "${_BW_SAVE_CXXVIS}")
endif()
if(_BW_SAVE_INLVIS STREQUAL "")
  unset(CMAKE_VISIBILITY_INLINES_HIDDEN)
else()
  set(CMAKE_VISIBILITY_INLINES_HIDDEN "${_BW_SAVE_INLVIS}")
endif()
