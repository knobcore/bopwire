/**
 * keystore_android.cpp
 *
 * Android implementation of mc_keystore_encrypt / mc_keystore_decrypt
 * (declared in bopwire/include/bopwire.h).
 *
 * This is a byte-for-byte reimplementation of the algorithm in
 * bopwire/src/crypto/keystore.cpp (the desktop / node source of truth —
 * NOT modified, NOT included here). The Android CMake target is a
 * deliberately lean wallet-only build that never linked capi.cpp or
 * OpenSSL, so `mc_keystore_encrypt` never actually existed in the
 * Android .so despite keystore_bindings.dart looking it up — that's the
 * "vault encryption failed" bug this file fixes.
 *
 * Same primitives, same parameters, same JSON shape as keystore.cpp:
 *   - scrypt via EVP_PBE_scrypt: N=16384, r=8, p=1, dklen=32, salt=16
 *     random bytes, maxmem=64MB.
 *   - AES-256-GCM: nonce=12 random bytes, tag=16 bytes.
 *   - JSON: {"v":1,"kdf":"scrypt","n":...,"r":...,"p":...,"dklen":...,
 *            "salt":"<b64>","cipher":"aes-256-gcm","nonce":"<b64>",
 *            "ct":"<b64>","tag":"<b64>","addr":"<optional>"}
 *   - base64: standard alphabet, no newlines (OpenSSL BIO, same as desktop).
 *
 * Linked against a prebuilt static libcrypto.a (OpenSSL 3.1.8, from the
 * KDAB android_openssl project) so the actual crypto calls
 * (EVP_PBE_scrypt / EVP_aes_256_gcm / RAND_bytes) are the exact same
 * audited OpenSSL implementation the desktop build uses — this is not a
 * reimplementation of the crypto itself, only a second call site.
 */

#include "bopwire.h"

#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;

constexpr uint64_t kScryptN  = 16384;
constexpr uint64_t kScryptR  = 8;
constexpr uint64_t kScryptP  = 1;
constexpr size_t   kKeyLen   = 32;
constexpr size_t   kSaltLen  = 16;
constexpr size_t   kNonceLen = 12;
constexpr size_t   kTagLen   = 16;
constexpr uint64_t kMaxMem   = 64ull * 1024 * 1024;

bool scrypt_kdf(const std::string& password, const uint8_t* salt, size_t salt_len,
                uint64_t N, uint64_t r, uint64_t p, uint8_t* out_key, size_t key_len) {
    return EVP_PBE_scrypt(password.data(), password.size(), salt, salt_len,
                          N, r, p, kMaxMem, out_key, key_len) == 1;
}

std::string b64_encode(const uint8_t* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, data, static_cast<int>(len));
    (void)BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

std::vector<uint8_t> b64_decode(const std::string& s) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new_mem_buf(s.data(), static_cast<int>(s.size()));
    mem = BIO_push(b64, mem);
    std::vector<uint8_t> out(s.size());
    int n = BIO_read(mem, out.data(), static_cast<int>(out.size()));
    if (n < 0) n = 0;
    out.resize(static_cast<size_t>(n));
    BIO_free_all(mem);
    return out;
}

std::string b64(const std::vector<uint8_t>& v) { return b64_encode(v.data(), v.size()); }

// Allocate a malloc'd copy of a std::string for return across the C ABI
// (caller frees with mc_free, which is plain free()).
char* dup_c_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

std::string keystore_encrypt_impl(const std::string& plaintext, const std::string& password,
                                  const std::string& addr) {
    std::vector<uint8_t> salt(kSaltLen), nonce(kNonceLen);
    if (RAND_bytes(salt.data(), static_cast<int>(kSaltLen)) != 1) return {};
    if (RAND_bytes(nonce.data(), static_cast<int>(kNonceLen)) != 1) return {};

    uint8_t key[kKeyLen];
    if (!scrypt_kdf(password, salt.data(), kSaltLen, kScryptN, kScryptR, kScryptP, key,
                    kKeyLen)) {
        return {};
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { std::memset(key, 0, sizeof(key)); return {}; }
    std::vector<uint8_t> ct(plaintext.size());
    std::vector<uint8_t> tag(kTagLen);
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce.data()) != 1) break;
        int outl = 0;
        if (EVP_EncryptUpdate(ctx, ct.data(), &outl,
                              reinterpret_cast<const uint8_t*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) != 1) break;
        int finl = 0;
        if (EVP_EncryptFinal_ex(ctx, ct.data() + outl, &finl) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen),
                                tag.data()) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    std::memset(key, 0, sizeof(key));
    if (!ok) return {};

    json j = {
        {"v", 1},          {"kdf", "scrypt"},   {"n", kScryptN},
        {"r", kScryptR},   {"p", kScryptP},     {"dklen", kKeyLen},
        {"salt", b64(salt)}, {"cipher", "aes-256-gcm"},
        {"nonce", b64(nonce)}, {"ct", b64(ct)}, {"tag", b64(tag)},
    };
    if (!addr.empty()) j["addr"] = addr;
    return j.dump();
}

bool keystore_decrypt_impl(const std::string& keystore_json, const std::string& password,
                           std::string& out) {
    json j = json::parse(keystore_json, nullptr, false);
    if (!j.is_object()) return false;
    if (j.value("cipher", std::string()) != "aes-256-gcm") return false;
    if (j.value("kdf", std::string()) != "scrypt") return false;

    const auto salt  = b64_decode(j.value("salt", std::string()));
    const auto nonce = b64_decode(j.value("nonce", std::string()));
    const auto ct    = b64_decode(j.value("ct", std::string()));
    const auto tag   = b64_decode(j.value("tag", std::string()));
    if (salt.empty() || nonce.size() != kNonceLen || tag.size() != kTagLen) return false;

    const uint64_t N     = j.value("n", kScryptN);
    const uint64_t r     = j.value("r", kScryptR);
    const uint64_t p     = j.value("p", kScryptP);
    const size_t   dklen = j.value("dklen", kKeyLen);
    if (dklen != kKeyLen) return false;

    uint8_t key[kKeyLen];
    if (!scrypt_kdf(password, salt.data(), salt.size(), N, r, p, key, kKeyLen)) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { std::memset(key, 0, sizeof(key)); return false; }
    std::vector<uint8_t> pt(ct.size());
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce.data()) != 1) break;
        int outl = 0;
        if (EVP_DecryptUpdate(ctx, pt.data(), &outl, ct.data(),
                              static_cast<int>(ct.size())) != 1) break;
        uint8_t tag_copy[kTagLen];
        std::memcpy(tag_copy, tag.data(), kTagLen);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                                tag_copy) != 1) break;
        int finl = 0;
        if (EVP_DecryptFinal_ex(ctx, pt.data() + outl, &finl) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    std::memset(key, 0, sizeof(key));
    if (!ok) return false;

    out.assign(reinterpret_cast<const char*>(pt.data()), pt.size());
    return true;
}

}  // namespace

extern "C" {

char* mc_keystore_encrypt(const char* plaintext, const char* password) {
    if (!plaintext || !password) return nullptr;
    std::string result = keystore_encrypt_impl(plaintext, password, std::string());
    if (result.empty()) return nullptr;
    return dup_c_string(result);
}

char* mc_keystore_decrypt(const char* keystore_json, const char* password) {
    if (!keystore_json || !password) return nullptr;
    std::string out;
    if (!keystore_decrypt_impl(keystore_json, password, out)) return nullptr;
    return dup_c_string(out);
}

}  // extern "C"
