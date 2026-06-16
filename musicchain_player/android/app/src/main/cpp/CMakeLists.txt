cmake_minimum_required(VERSION 3.22)
project(musicchain_android C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# secp256k1 source dir (extracted beside android/ folder)
set(SECP256K1_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../secp256k1-0.5.1")

# Build secp256k1 as a static library with minimal config
add_library(secp256k1 STATIC
    ${SECP256K1_DIR}/src/secp256k1.c
    ${SECP256K1_DIR}/src/precomputed_ecmult.c
    ${SECP256K1_DIR}/src/precomputed_ecmult_gen.c
)
target_include_directories(secp256k1 PUBLIC
    ${SECP256K1_DIR}/include
    ${SECP256K1_DIR}/src
    ${SECP256K1_DIR}
)
target_compile_definitions(secp256k1 PRIVATE
    SECP256K1_BUILD
    ENABLE_MODULE_ECDH=1
    ENABLE_MODULE_RECOVERY=0
    USE_SCALAR_8x32
    USE_FIELD_10x26
    USE_NUM_NONE
    USE_FIELD_INV_BUILTIN
    USE_SCALAR_INV_BUILTIN
    ECMULT_WINDOW_SIZE=15
    ECMULT_GEN_PREC_BITS=4
)

# Build the musicchain shared library
add_library(musicchain SHARED musicchain_android.c)
target_include_directories(musicchain PRIVATE
    ${SECP256K1_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../musicchain/include
)
target_link_libraries(musicchain PRIVATE secp256k1 log)
target_compile_definitions(musicchain PRIVATE MUSICCHAIN_BUILD=1)

# ---- librats shared library (loaded via Dart FFI at runtime) ----
#
# Path: from android/app/src/main/cpp/  →  ../../../../../../musicchain/deps/librats
set(LIBRATS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../musicchain/deps/librats")

set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       ON  CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY ON  CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE "1.0.0" CACHE STRING "" FORCE)

add_subdirectory("${LIBRATS_DIR}" librats_build)

# ---- chromaprint JNI bridge (chromaprint_jni.cpp) -------------------
#
# Lets Kotlin call chromaprint_new/start/feed/finish/get_fingerprint
# directly without bouncing PCM through MethodChannel + Dart FFI.
# Drops one full PCM round-trip + a Dart-side memcpy per fingerprinted
# song, so an album scan finishes ~2-3× faster.
#
# Header lives in the original chromaprint-1.5.1 source tree the user
# unpacked alongside the chromaprint-android-arm64 prebuilt. We link
# against the prebuilt libchromaprint.so that already ships in
# jniLibs/arm64-v8a/.
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
        "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libchromaprint.so"
)
target_link_libraries(chromaprint_jni PRIVATE chromaprint_prebuilt log)
