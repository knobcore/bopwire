cmake_minimum_required(VERSION 3.22)
project(bopwire_android C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- bopwire root --------------------------------------------------
# From android/app/src/main/cpp/ → bopwire root is 7 dirs up.
set(MC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../bopwire")

# ---- libwally-core (static, vendored) --------------------------------
#
# libwally bundles its own secp256k1-zkp. Building both libwally's
# secp256k1 and a standalone secp256k1 here creates a target-name clash
# (both register a `secp256k1` target), so we let libwally own the
# secp256k1 build and link bopwire against libwally's copy.
#
# These flags match the desktop / home-node build (bopwire/CMakeLists.txt
# lines 290+): static-only, no tests, the modules wally needs and nothing
# else. Keeping them in sync means the same mnemonic derives the same
# secp256k1 key on every platform.
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(WALLYCORE_INSTALL         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_BUILD_ELEMENTS  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH        ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY    ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT    OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_GENERATOR   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RANGEPROOF  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SURJECTIONPROOF ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_WHITELIST   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG       OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_ADAPTOR OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_S2C   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_BPPP        OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK           OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS    OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS         OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES            OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                   OFF CACHE BOOL "" FORCE)

add_subdirectory("${MC_ROOT}/deps/libwally-core" libwally_build EXCLUDE_FROM_ALL)

# ---- Prebuilt OpenSSL (static libcrypto only) for the Android keystore ----
#
# mc_keystore_encrypt/decrypt (bopwire/include/bopwire.h) need scrypt +
# AES-256-GCM. The desktop/node build gets these from vcpkg OpenSSL via
# bopwire/src/crypto/keystore.cpp (untouched, off-limits here) — this
# Android target never linked that file or OpenSSL at all, which is why
# mc_keystore_encrypt did not exist in the .so (NativeLibrary lookup
# silently failed, surfacing as "vault encryption failed" in the app).
#
# We reuse the prebuilt OpenSSL 3.1.8 static libcrypto.a already vendored
# at repo root under openssl-android/ (KDAB's android_openssl project,
# used upstream for Qt-on-Android; not part of the removed msquic/quictls
# tree). Static linking means no extra .so to ship in jniLibs. libssl is
# NOT linked — only libcrypto (EVP/scrypt/AES-GCM/RAND), no TLS needed.
set(MC_OPENSSL_ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../openssl-android/android_openssl-master/ssl_3")
add_library(openssl_crypto_static STATIC IMPORTED)
set_target_properties(openssl_crypto_static PROPERTIES
    IMPORTED_LOCATION "${MC_OPENSSL_ANDROID_ROOT}/${ANDROID_ABI}/libcrypto.a")

# ---- Build the bopwire shared library -----------------------------
add_library(bopwire SHARED
    bopwire_android.c
    keystore_android.cpp
    "${MC_ROOT}/src/crypto/keccak256_c.c")
target_include_directories(bopwire PRIVATE
    "${MC_ROOT}/include"
    "${MC_ROOT}/src/crypto"
    "${MC_ROOT}/deps/libwally-core/include"
    "${MC_ROOT}/deps/libwally-core/src/secp256k1/include"
    "${MC_ROOT}/deps/nlohmann"
    "${MC_OPENSSL_ANDROID_ROOT}/include")
target_link_libraries(bopwire PRIVATE wallycore secp256k1 log openssl_crypto_static)
target_compile_definitions(bopwire PRIVATE BOPWIRE_BUILD=1)
# libcrypto.a pulls in thousands of OpenSSL-internal symbols (X509_*, ASN1_*,
# PKCS7_*, ...) that bopwire never calls. Without --exclude-libs those all
# become global dynamic symbols in libbopwire.so — pure bloat, and needless
# attack surface / potential clash risk if another native lib in-process ever
# links its own OpenSSL. Only the bopwire.h C API (mc_*, already covered by
# BOPWIRE_API's default visibility) needs to be exported.
target_link_options(bopwire PRIVATE "-Wl,--exclude-libs,libcrypto.a")

# ---- librats shared library (loaded via Dart FFI at runtime) ---------
set(LIBRATS_DIR "${MC_ROOT}/deps/librats")
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       ON  CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY ON  CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE "1.0.0" CACHE STRING "" FORCE)
add_subdirectory("${LIBRATS_DIR}" librats_build)

# The librats CMake target is named `rats`, whose default output is
# `librats.so`. But the Dart FFI loads `libmc_rats.so` (see rats_client.dart
# _loadLibrary). Without this rename the build produced librats.so while the app
# loaded a STALE prebuilt jniLibs/libmc_rats.so — so none of the freshly-built
# transport ever reached the device. Force the output name so the compiled
# librats IS what gets loaded. (The bopwire root CMakeLists does the same for
# the desktop/node builds; the player builds librats directly so must repeat it.)
set_target_properties(rats PROPERTIES OUTPUT_NAME mc_rats)

# ---- chromaprint JNI bridge ------------------------------------------
#
# Lets Kotlin call chromaprint_new/start/feed/finish/get_fingerprint
# directly without bouncing PCM through MethodChannel + Dart FFI.
# Drops one full PCM round-trip + a Dart-side memcpy per fingerprinted
# song, so an album scan finishes ~2-3× faster.
set(CHROMAPRINT_HEADER_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../chromaprint-1.5.1/src/include")

add_library(chromaprint_jni SHARED chromaprint_jni.cpp)
target_include_directories(chromaprint_jni PRIVATE ${CHROMAPRINT_HEADER_DIR})

# Import the prebuilt chromaprint .so as a SHARED IMPORTED target so
# the linker can resolve chromaprint_* symbols and the runtime loader
# picks the version in jniLibs.
add_library(chromaprint_prebuilt SHARED IMPORTED)
set_target_properties(chromaprint_prebuilt PROPERTIES
    IMPORTED_LOCATION
        "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libchromaprint.so")
target_link_libraries(chromaprint_jni PRIVATE chromaprint_prebuilt log)
