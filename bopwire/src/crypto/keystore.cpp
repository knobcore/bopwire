#include "keystore.h"

#include "../audio/fingerprint.h"  // mc::audio::base64_encode / base64_decode

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mc::crypto {

namespace {
using json = nlohmann::json;

// scrypt cost params (RFC 7914). N=2^14 keeps derivation ~50-100ms, plenty for
// a wallet keystore, and well under the 64 MB maxmem below (128*N*r ≈ 16 MB).
constexpr uint64_t kScryptN  = 16384;
constexpr uint64_t kScryptR  = 8;
constexpr uint64_t kScryptP  = 1;
constexpr size_t   kKeyLen   = 32;
constexpr size_t   kSaltLen  = 16;
constexpr size_t   kNonceLen = 12;  // AES-GCM default IV length
constexpr size_t   kTagLen   = 16;
constexpr uint64_t kMaxMem   = 64ull * 1024 * 1024;

bool scrypt_kdf(const std::string& password, const uint8_t* salt, size_t salt_len,
                uint64_t N, uint64_t r, uint64_t p, uint8_t* out_key, size_t key_len) {
    // OpenSSL 3.x scrypt — no extra dependency.
    return EVP_PBE_scrypt(password.data(), password.size(), salt, salt_len,
                          N, r, p, kMaxMem, out_key, key_len) == 1;
}

std::string b64(const std::vector<uint8_t>& v) {
    return mc::audio::base64_encode(v.data(), v.size());
}
}  // namespace

std::string keystore_encrypt(const std::string& plaintext, const std::string& password,
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

bool keystore_decrypt(const std::string& keystore_json, const std::string& password,
                      std::string& out) {
    json j = json::parse(keystore_json, nullptr, false);
    if (!j.is_object()) return false;
    if (j.value("cipher", std::string()) != "aes-256-gcm") return false;
    if (j.value("kdf", std::string()) != "scrypt") return false;

    const auto salt  = mc::audio::base64_decode(j.value("salt", std::string()));
    const auto nonce = mc::audio::base64_decode(j.value("nonce", std::string()));
    const auto ct    = mc::audio::base64_decode(j.value("ct", std::string()));
    const auto tag   = mc::audio::base64_decode(j.value("tag", std::string()));
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
        // Auth failure (wrong password / tampering) => returns != 1.
        if (EVP_DecryptFinal_ex(ctx, pt.data() + outl, &finl) != 1) break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    std::memset(key, 0, sizeof(key));
    if (!ok) return false;

    out.assign(reinterpret_cast<const char*>(pt.data()), pt.size());
    return true;
}

std::string keystore_address(const std::string& keystore_json) {
    json j = json::parse(keystore_json, nullptr, false);
    if (!j.is_object()) return {};
    return j.value("addr", std::string());
}

std::string password_policy_error(const std::string& pw) {
    if (pw.size() < 12) return "password must be at least 12 characters";
    bool has_upper = false, has_special = false;
    for (unsigned char c : pw) {
        if (c >= 'A' && c <= 'Z') has_upper = true;
        else if (!std::isalnum(c) && !std::isspace(c)) has_special = true;
    }
    if (!has_upper) return "add at least one uppercase letter";
    if (!has_special) return "add at least one special character";
    return "";
}

}  // namespace mc::crypto
