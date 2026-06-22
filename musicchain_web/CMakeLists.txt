cmake_minimum_required(VERSION 3.22)
project(musicchain_web C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This CMakeLists must be invoked through emcmake so that CMAKE_C_COMPILER /
# CMAKE_CXX_COMPILER are set to emcc / em++. The build.sh wrapper at the
# project root does that for us; running plain `cmake ..` from here will
# fail loudly during link.
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "musicchain_web must be configured with emcmake — "
                       "run build.sh from the project root.")
endif()

set(MC_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}/../musicchain")
set(WALLY_ROOT  "${MC_ROOT}/deps/libwally-core")
set(KECCAK_SRC  "${MC_ROOT}/src/crypto/keccak256_c.c")

# ---- libwally + bundled secp256k1, both static, WASM target -----------
#
# Force every dependent target to static; emscripten's link step rolls
# the .a archives into the final .wasm module. WALLYCORE_BUILD_ELEMENTS
# stays ON to match the desktop / Android player builds — keeping the
# same module list across all three platforms means the same mnemonic
# derives the same address everywhere with no surprises.
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(WALLYCORE_INSTALL         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_BUILD_ELEMENTS  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH        ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY    ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_GENERATOR   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RANGEPROOF  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SURJECTIONPROOF ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_WHITELIST   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_S2C   ON  CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK           OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS    OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS         OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES            OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                   OFF CACHE BOOL "" FORCE)

add_subdirectory("${WALLY_ROOT}" libwally_build EXCLUDE_FROM_ALL)

# ---- wallet.wasm: BIP39 + BIP32 + secp256k1 + EIP-55 ----------------
#
# The glue layer in native/wallet_glue.c re-exports a small set of
# extern "C" symbols from libwally and our own keccak256_c that the
# web player calls into via Module.cwrap(). Everything else is dead-
# code-eliminated by emcc's --closure pass, so the final .wasm only
# contains what wallet.ts actually uses.
add_executable(wallet
    native/wallet_glue.c
    "${KECCAK_SRC}")
target_include_directories(wallet PRIVATE
    "${MC_ROOT}/include"
    "${MC_ROOT}/src/crypto"
    "${WALLY_ROOT}/include"
    "${WALLY_ROOT}/src/secp256k1/include")
target_link_libraries(wallet PRIVATE wallycore secp256k1)

# Emscripten link flags. -sMODULARIZE wraps the runtime in a factory
# function so multiple wasm modules don't fight over the global
# namespace; -sEXPORT_ES6 emits an ES module the bundler can import.
# Exported runtime methods (cwrap, ccall, getValue, setValue, etc.)
# are needed by the TypeScript wrapper; exported function names are
# the entry points wallet_glue.c defines.
set_target_properties(wallet PROPERTIES
    SUFFIX ".js"
    OUTPUT_NAME "wallet")
target_link_options(wallet PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createWalletModule"
    "-sENVIRONMENT=web,worker"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=16MB"
    "-sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','getValue','setValue','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_mc_web_bip39_generate_12','_mc_web_bip39_validate','_mc_web_wallet_from_mnemonic','_mc_web_wallet_get_address','_mc_web_wallet_get_pubkey','_mc_web_wallet_sign','_mc_web_wallet_free','_mc_web_eip55_checksum_inplace','_mc_web_bytes_to_hex','_mc_web_hex_to_bytes']"
    "--closure=1"
    "-O3")

# Copy the final wallet.js + wallet.wasm next to the web app so the
# dev server can serve them at /wasm/wallet.js. The bundler in src/
# imports via that path.
add_custom_command(TARGET wallet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:wallet>"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/wallet.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:wallet>/wallet.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/wallet.wasm")

# =====================================================================
# chain.wasm — TransferTx / UsernameTx preimages, SHA-256 hashing,
# EIP-55 checksum encode + verify. Built from a deliberately-trimmed
# slice of the musicchain core:
#
#   * src/core/transaction.cpp   — TransferTx / UsernameTx / other tx
#                                   types' sign_message / serialize.
#                                   verify_signature paths still
#                                   reference crypto::verify_ecdsa etc.
#                                   but get dead-stripped by emcc
#                                   because no web export calls into
#                                   them.
#   * native/chain_shim.cpp      — reimplementations of mc::write_*le,
#                                   mc::crypto::sha256, address derive,
#                                   EIP-55, ECDSA verify. Built on
#                                   libwally (sha256) + libsecp256k1
#                                   (verify / recover) + vendored
#                                   keccak256_c so we don't drag in
#                                   OpenSSL for the four functions
#                                   transaction.cpp actually needs.
#   * native/chain_glue.cpp      — extern "C" mc_web_chain_* entry
#                                   points the TypeScript layer calls
#                                   via Module.cwrap().
#   * src/crypto/keccak256_c.c   — same one wallet.wasm pulls in.
#
# Deliberately excluded — these either pull in OpenSSL, leveldb, the
# chain rules engine, or platform syscalls that don't link under emcc:
#
#   * src/core/block.cpp        — BlockHeader::hash() calls into
#                                  hash.cpp which is OpenSSL.
#                                  Helpers (write_u*le etc.) are
#                                  re-supplied in chain_shim.cpp.
#   * src/core/chain.cpp        — leveldb, threading, tx rules engine.
#   * src/core/merkle.cpp       — only Block::compute_merkle_root, not
#                                  needed by any web verb.
#   * src/crypto/hash.cpp       — OpenSSL SHA-256; chain_shim uses
#                                  libwally instead.
#   * src/crypto/keys.cpp       — OpenSSL EC_KEY + Argon2/PBKDF2.
#                                  Web wallet flow already lives in
#                                  wallet.wasm via libwally BIP32.
#   * src/crypto/signature.cpp  — OpenSSL ECDSA; chain_shim ships a
#                                  libsecp256k1 reimplementation.
#   * src/crypto/bip39.cpp      — already exposed via wallet.wasm.
#   * src/crypto/ecies.cpp      — OpenSSL; no web verb uses it.
#   * src/crypto/keccak256.cpp  — C++ wrapper over keccak256_c. We
#                                  use the plain-C mc_keccak256
#                                  symbol directly.
#   * src/audio/*               — browser decodes via MediaSource.
#   * src/network/*, src/p2p/*, src/consensus/* — don't link under
#                                  emcc and unused by the web side.
#   * src/capi/capi.cpp         — pulls audio + block transitively;
#                                  the web has its own smaller verb
#                                  surface in chain_glue.cpp.
add_executable(chain
    native/chain_glue.cpp
    native/chain_shim.cpp
    "${MC_ROOT}/src/core/transaction.cpp"
    "${KECCAK_SRC}")
target_include_directories(chain PRIVATE
    "${MC_ROOT}/include"
    "${MC_ROOT}/src"
    "${MC_ROOT}/src/crypto"
    "${MC_ROOT}/src/core"
    "${WALLY_ROOT}/include"
    "${WALLY_ROOT}/src/secp256k1/include")
target_link_libraries(chain PRIVATE wallycore secp256k1)

set_target_properties(chain PROPERTIES
    SUFFIX ".js"
    OUTPUT_NAME "chain")
target_link_options(chain PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createChainModule"
    "-sENVIRONMENT=web,worker"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=16MB"
    "-sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','getValue','setValue','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_mc_web_chain_transfer_sign_message','_mc_web_chain_username_sign_message','_mc_web_chain_to_checksum_hex','_mc_web_chain_parse_address','_mc_web_chain_sha256_hex','_mc_web_chain_validate_hex_hash']"
    "--closure=1"
    "-O3")

add_custom_command(TARGET chain POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:chain>"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/chain.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:chain>/chain.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/chain.wasm")

# =====================================================================
# chromaprint.wasm: AcoustID fingerprint generator for browser uploads.
#
# The home node's `fingerprint.submit` endpoint expects a base64-encoded
# Chromaprint v2 fingerprint (same format Android player + Windows
# tracker already produce). To match those, we vendor chromaprint
# 1.5.1 + its bundled KissFFT directly — same source tree the Android
# arm64 build at chromaprint-android-arm64/ uses — and link them into
# a small WASM module the web player can drive after decoding a local
# audio file with the browser's WebAudio API.
#
# We compile sources inline instead of `add_subdirectory(chromaprint)`
# because the upstream CMakeLists does `find_package(FFmpeg/FFTW3/...)`,
# builds tests, and installs pkgconfig files — none of which we want
# in a WASM build. The list of sources mirrors what the upstream
# src/CMakeLists.txt picks when USE_KISSFFT=1 and no FFmpeg.
# =====================================================================
set(CP_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}/../chromaprint-1.5.1")
set(CP_SRC      "${CP_ROOT}/src")
set(CP_KISSFFT  "${CP_SRC}/3rdparty/kissfft")

# Generated config.h for chromaprint. The upstream build configures
# this via configure_file() inside its CMakeLists; we hand-roll it
# because the host CMake's check_symbol_exists() can't run during a
# cross-compile through emcc cleanly. The values match what the
# Android arm64 build produced (chromaprint-android-arm64/config.h).
set(CP_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/chromaprint_config")
file(MAKE_DIRECTORY "${CP_CONFIG_DIR}")
file(WRITE "${CP_CONFIG_DIR}/config.h"
"#ifndef CHROMAPRINT_WEB_CONFIG_H_\n"
"#define CHROMAPRINT_WEB_CONFIG_H_\n"
"#define HAVE_ROUND 1\n"
"#define HAVE_LRINTF 1\n"
"#define USE_KISSFFT 1\n"
"#endif\n")

add_executable(chromaprint
    native/chromaprint_glue.c
    # Public C ABI
    "${CP_SRC}/chromaprint.cpp"
    # Core chromaprint engine
    "${CP_SRC}/audio_processor.cpp"
    "${CP_SRC}/chroma.cpp"
    "${CP_SRC}/chroma_resampler.cpp"
    "${CP_SRC}/chroma_filter.cpp"
    "${CP_SRC}/spectrum.cpp"
    "${CP_SRC}/fft.cpp"
    "${CP_SRC}/fingerprinter.cpp"
    "${CP_SRC}/image_builder.cpp"
    "${CP_SRC}/simhash.cpp"
    "${CP_SRC}/silence_remover.cpp"
    "${CP_SRC}/fingerprint_calculator.cpp"
    "${CP_SRC}/fingerprint_compressor.cpp"
    "${CP_SRC}/fingerprint_decompressor.cpp"
    "${CP_SRC}/fingerprinter_configuration.cpp"
    "${CP_SRC}/fingerprint_matcher.cpp"
    "${CP_SRC}/utils/base64.cpp"
    "${CP_SRC}/avresample/resample2.c"
    # KissFFT backend
    "${CP_SRC}/fft_lib_kissfft.cpp"
    "${CP_KISSFFT}/kiss_fft.c"
    "${CP_KISSFFT}/tools/kiss_fftr.c")

target_include_directories(chromaprint PRIVATE
    "${CP_CONFIG_DIR}"
    "${CP_SRC}"
    "${CP_KISSFFT}"
    "${CP_KISSFFT}/tools")

target_compile_definitions(chromaprint PRIVATE
    HAVE_CONFIG_H
    CHROMAPRINT_NODLL
    _USE_MATH_DEFINES
    __STDC_LIMIT_MACROS
    __STDC_CONSTANT_MACROS)

set_target_properties(chromaprint PROPERTIES
    SUFFIX ".js"
    OUTPUT_NAME "chromaprint")

target_link_options(chromaprint PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createChromaprintModule"
    "-sENVIRONMENT=web,worker"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=16MB"
    "-sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','getValue','setValue','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8','HEAP16']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_mc_web_chromaprint_new','_mc_web_chromaprint_start','_mc_web_chromaprint_feed','_mc_web_chromaprint_finish','_mc_web_chromaprint_get_compressed','_mc_web_chromaprint_free']"
    "-O3")

add_custom_command(TARGET chromaprint POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:chromaprint>"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/chromaprint.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:chromaprint>/chromaprint.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/wasm/chromaprint.wasm")

# =====================================================================
# rats.wasm — REMOVED.
#
# See memory entry `project-musicchain-mini-node-router`. The browser
# player no longer speaks librats; it dials its configured mini-node
# over plain WebSocket via NodeClient (src/main.ts -> src/node_client.ts)
# and lets the mini-node fan verbs out to the rest of the mesh on the
# server side. The librats-WASM target below pulled in:
#
#   * deps/librats/src/*.cpp     — io_loop, management_loop, gossipsub,
#                                   noise, stun/turn/ice, etc.
#   * native/librats_stubs/*.cpp — local no-op shims for io_poller /
#                                   dht / mdns / upnp / natpmp /
#                                   librats_portmap / file_transfer
#                                   (everything browser-forbidden).
#   * -lwebsocket.js, -sASYNCIFY, -sUSE_PTHREADS link options to make
#                                   the BSD socket layer survive in a
#                                   browser tab.
#
# All of that has been deleted along with src/rats_client.ts and
# src/mini_node_pool.ts. Re-introducing a browser-side librats requires
# walking the memory entry first — the mini-node router was added
# precisely so the browser doesn't have to.
# =====================================================================
# (rats add_executable / target_link_options / post_build copy intentionally
# left commented out below for reference only — DO NOT uncomment.)
#
# set(LIBRATS_ROOT "${MC_ROOT}/deps/librats")
# set(LIBRATS_SRC  "${LIBRATS_ROOT}/src")
#
# set(RATS_VERSION_DIR "${CMAKE_CURRENT_BINARY_DIR}/librats_version")
# file(MAKE_DIRECTORY "${RATS_VERSION_DIR}")
# file(WRITE "${RATS_VERSION_DIR}/version.h"
# "#pragma once\n"
# "#include \"rats_export.h\"\n"
# "#define LIBRATS_VERSION_MAJOR 1\n"
# "#define LIBRATS_VERSION_MINOR 0\n"
# "#define LIBRATS_VERSION_PATCH 0\n"
# "#define LIBRATS_VERSION_BUILD 0\n"
# "#define LIBRATS_VERSION_STRING \"1.0.0.0\"\n"
# "#define LIBRATS_GIT_DESCRIBE \"wasm\"\n"
# "#define LIBRATS_ASCII_HEADER \"librats wasm\\n\"\n"
# "namespace librats { namespace version {\n"
# "  const int MAJOR = LIBRATS_VERSION_MAJOR;\n"
# "  const int MINOR = LIBRATS_VERSION_MINOR;\n"
# "  const int PATCH = LIBRATS_VERSION_PATCH;\n"
# "  const int BUILD = LIBRATS_VERSION_BUILD;\n"
# "  const char* const STRING = LIBRATS_VERSION_STRING;\n"
# "  const char* const GIT_DESCRIBE = LIBRATS_GIT_DESCRIBE;\n"
# "  const char* const ASCII_HEADER = LIBRATS_ASCII_HEADER;\n"
# "  RATS_API void rats_print_version_info();\n"
# "  RATS_API void rats_print_header();\n"
# "}}\n"
# )
#
# set(LIBRATS_WASM_SOURCES
#     "${LIBRATS_SRC}/socket.cpp"
#     "${LIBRATS_SRC}/network_utils.cpp"
#     "${LIBRATS_SRC}/bencode.cpp"
#     "${LIBRATS_SRC}/krpc.cpp"
#     "${LIBRATS_SRC}/librats.cpp"
#     "${LIBRATS_SRC}/librats_reconnection.cpp"
#     "${LIBRATS_SRC}/librats_logging.cpp"
#     "${LIBRATS_SRC}/librats_encryption.cpp"
#     "${LIBRATS_SRC}/librats_file_transfer.cpp"
#     "${LIBRATS_SRC}/librats_gossipsub.cpp"
#     "${LIBRATS_SRC}/librats_persistence.cpp"
#     "${LIBRATS_SRC}/librats_statistic.cpp"
#     "${LIBRATS_SRC}/sha1.cpp"
#     "${LIBRATS_SRC}/os.cpp"
#     "${LIBRATS_SRC}/fs.cpp"
#     "${LIBRATS_SRC}/logger.cpp"
#     "${LIBRATS_SRC}/threadmanager.cpp"
#     "${LIBRATS_SRC}/gossipsub.cpp"
#     "${LIBRATS_SRC}/version.cpp"
#     "${LIBRATS_SRC}/receive_buffer.cpp"
#     "${LIBRATS_SRC}/chained_send_buffer.cpp"
#     "${LIBRATS_SRC}/crypto/curve25519.c"
#     "${LIBRATS_SRC}/crypto/chacha.c"
#     "${LIBRATS_SRC}/crypto/poly1305.c"
#     "${LIBRATS_SRC}/crypto/chachapoly.c"
#     "${LIBRATS_SRC}/crypto/sha256.c"
#     "${LIBRATS_SRC}/crypto/sha512.c"
#     "${LIBRATS_SRC}/crypto/hkdf.c"
#     "${LIBRATS_SRC}/crypto/blake2s.c"
#     "${LIBRATS_SRC}/crypto/blake2b.c"
#     "${LIBRATS_SRC}/noise.cpp"
#     "${LIBRATS_SRC}/stun.cpp"
#     "${LIBRATS_SRC}/turn.cpp"
#     "${LIBRATS_SRC}/ice.cpp"
#     "${LIBRATS_SRC}/librats_ice.cpp"
#     "${LIBRATS_SRC}/librats_c.cpp"
#     native/librats_stubs/io_poller_stub.cpp
#     native/librats_stubs/dht_stub.cpp
#     native/librats_stubs/librats_discovery_stub.cpp
#     native/librats_stubs/mdns_stub.cpp
#     native/librats_stubs/librats_mdns_stub.cpp
#     native/librats_stubs/upnp_stub.cpp
#     native/librats_stubs/natpmp_stub.cpp
#     native/librats_stubs/librats_portmap_stub.cpp
#     native/librats_stubs/file_transfer_stub.cpp
# )
#
# add_executable(rats ${LIBRATS_WASM_SOURCES})
# target_include_directories(rats PRIVATE
#     "${LIBRATS_SRC}"
#     "${LIBRATS_SRC}/crypto"
#     "${RATS_VERSION_DIR}"
# )
# target_compile_definitions(rats PRIVATE RATS_EXPORT_DLL)
# target_compile_options(rats PRIVATE
#     "-include" "sys/select.h"
#     "-fexceptions"
#     "-pthread"
# )
# set_target_properties(rats PROPERTIES
#     SUFFIX ".js"
#     OUTPUT_NAME "rats")
# target_link_options(rats PRIVATE
#     "-sMODULARIZE=1"
#     "-sEXPORT_ES6=1"
#     "-sEXPORT_NAME=createRatsModule"
#     "-sENVIRONMENT=web,worker,node"
#     "-sALLOW_MEMORY_GROWTH=1"
#     "-sINITIAL_MEMORY=16MB"
#     "-lwebsocket.js"
#     "-sASYNCIFY=1"
#     "-sALLOW_TABLE_GROWTH=1"
#     "-pthread"
#     "-sUSE_PTHREADS=1"
#     "-sPTHREAD_POOL_SIZE=8"
#     "-sPTHREAD_POOL_SIZE_STRICT=0"
#     "-sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','getValue','setValue','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8','addFunction','removeFunction']"
#     "-sEXPORTED_FUNCTIONS=['_malloc','_free','_rats_create','_rats_destroy','_rats_start','_rats_stop','_rats_connect','_rats_send_message','_rats_send_binary','_rats_broadcast_string','_rats_broadcast_binary','_rats_on_message','_rats_set_string_callback','_rats_set_binary_callback','_rats_set_json_callback','_rats_set_connection_callback','_rats_set_disconnect_callback','_rats_get_our_peer_id','_rats_get_validated_peer_ids','_rats_get_peer_ids','_rats_get_peer_count','_rats_get_peer_info_json','_rats_string_free','_rats_set_logging_enabled','_rats_set_console_logging_enabled','_rats_set_log_level','_rats_get_version_string','_rats_get_abi','_rats_get_listen_port','_rats_set_max_peers','_rats_get_max_peers','_rats_set_encryption_enabled','_rats_is_encryption_enabled','_rats_disconnect_peer_by_id','_rats_set_data_directory','_rats_get_data_directory','_rats_load_configuration','_rats_save_configuration']"
#     "-O3"
#     "-sASSERTIONS=1"
#     "-fexceptions")
#
# add_custom_command(TARGET rats POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different
#             "$<TARGET_FILE:rats>"
#             "${CMAKE_CURRENT_SOURCE_DIR}/wasm/rats.js"
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different
#             "$<TARGET_FILE_DIR:rats>/rats.wasm"
#             "${CMAKE_CURRENT_SOURCE_DIR}/wasm/rats.wasm")
