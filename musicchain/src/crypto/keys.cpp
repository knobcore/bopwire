#include "keys.h"
#include "hash.h"
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <cstring>

// Note: Argon2 is typically provided by libsodium.
// If libsodium is not available, a PBKDF2 fallback is used.
#if __has_include(<sodium.h>)
#  include <sodium.h>
#  define HAS_LIBSODIUM 1
#else
#  define HAS_LIBSODIUM 0
#endif

namespace mc::crypto {

namespace {

// Derive 32-byte key from EC private key BN
std::vector<uint8_t> bn_to_privkey(const BIGNUM* bn) {
    std::vector<uint8_t> out(32, 0);
    int n = BN_num_bytes(bn);
    if (n > 32) throw std::runtime_error("private key too large");
    BN_bn2bin(bn, out.data() + (32 - n));
    return out;
}

EC_KEY* make_ec_key() {
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) throw std::runtime_error("EC_KEY_new_by_curve_name failed");
    return key;
}

KeyPair fill_from_ec_key(EC_KEY* key) {
    KeyPair kp;
    // Private key
    const BIGNUM* priv_bn = EC_KEY_get0_private_key(key);
    kp.private_key = bn_to_privkey(priv_bn);

    // Compressed public key (33 bytes)
    const EC_GROUP* group = EC_KEY_get0_group(key);
    const EC_POINT* pub   = EC_KEY_get0_public_key(key);
    size_t pub_len = EC_POINT_point2oct(group, pub, POINT_CONVERSION_COMPRESSED,
                                        kp.public_key.data(), 33, nullptr);
    if (pub_len != 33) throw std::runtime_error("pubkey compression failed");

    // Address: last 20 bytes of SHA256(uncompressed pubkey)
    std::vector<uint8_t> uncompressed(65);
    EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED,
                       uncompressed.data(), 65, nullptr);
    auto h = sha256(uncompressed.data(), uncompressed.size());
    std::copy(h.end() - 20, h.end(), kp.address.begin());

    return kp;
}

} // anonymous namespace

KeyPair generate_keypair() {
    EC_KEY* key = make_ec_key();
    if (EC_KEY_generate_key(key) != 1) {
        EC_KEY_free(key);
        throw std::runtime_error("EC_KEY_generate_key failed");
    }
    auto kp = fill_from_ec_key(key);
    EC_KEY_free(key);
    return kp;
}

KeyPair keypair_from_seed(const uint8_t* seed, size_t len) {
    auto hash = sha256(seed, len);
    EC_KEY*   key    = make_ec_key();
    BIGNUM*   bn     = BN_bin2bn(hash.data(), 32, nullptr);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* pub = EC_POINT_new(group);
    EC_KEY_set_private_key(key, bn);
    EC_POINT_mul(group, pub, bn, nullptr, nullptr, nullptr);
    EC_KEY_set_public_key(key, pub);
    auto kp = fill_from_ec_key(key);
    EC_POINT_free(pub);
    BN_free(bn);
    EC_KEY_free(key);
    return kp;
}

bool keypair_from_hex(const std::string& priv_hex, KeyPair& out) {
    auto bytes = from_hex(priv_hex);
    if (bytes.size() != 32) return false;
    try {
        out = keypair_from_seed(bytes.data(), 32);
    } catch (...) { return false; }
    return true;
}

std::vector<uint8_t> derive_seed_pbkdf2_sha512(const std::string& passphrase,
                                               const std::string& salt,
                                               uint32_t iterations) {
    std::vector<uint8_t> out(32, 0);
    PKCS5_PBKDF2_HMAC(passphrase.data(),
                      static_cast<int>(passphrase.size()),
                      reinterpret_cast<const unsigned char*>(salt.data()),
                      static_cast<int>(salt.size()),
                      static_cast<int>(iterations),
                      EVP_sha512(),
                      32,
                      out.data());
    return out;
}

Address address_from_pubkey(const PubKey33& pubkey) {
    EC_KEY*   key   = make_ec_key();
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* pub   = EC_POINT_new(group);
    EC_POINT_oct2point(group, pub, pubkey.data(), 33, nullptr);
    std::vector<uint8_t> uncompressed(65);
    EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED,
                       uncompressed.data(), 65, nullptr);
    EC_POINT_free(pub);
    EC_KEY_free(key);
    auto h = sha256(uncompressed.data(), 65);
    Address addr;
    std::copy(h.end() - 20, h.end(), addr.begin());
    return addr;
}

// ---- Key encryption -------------------------------------------------

static void derive_key_from_password(const std::string& password,
                                     const uint8_t* salt, size_t salt_len,
                                     uint8_t* out_key, size_t key_len) {
#if HAS_LIBSODIUM
    if (crypto_pwhash(out_key, key_len,
                      password.data(), password.size(),
                      salt,
                      crypto_pwhash_OPSLIMIT_INTERACTIVE,
                      crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("Argon2id key derivation failed");
    }
#else
    // PBKDF2 fallback
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                      salt, static_cast<int>(salt_len),
                      100000, EVP_sha256(),
                      static_cast<int>(key_len), out_key);
#endif
}

EncryptedKey encrypt_key(const std::vector<uint8_t>& priv_key,
                         const std::string& password) {
    EncryptedKey ek;
    ek.salt.resize(32);
    ek.nonce.resize(12);
    RAND_bytes(ek.salt.data(), 32);
    RAND_bytes(ek.nonce.data(), 12);

    uint8_t enc_key[32];
    derive_key_from_password(password, ek.salt.data(), 32, enc_key, 32);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, enc_key, ek.nonce.data());

    ek.ciphertext.resize(priv_key.size() + 16);
    int outl = 0;
    EVP_EncryptUpdate(ctx, ek.ciphertext.data(), &outl,
                      priv_key.data(), static_cast<int>(priv_key.size()));
    int final_len = 0;
    EVP_EncryptFinal_ex(ctx, ek.ciphertext.data() + outl, &final_len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                        ek.ciphertext.data() + priv_key.size());
    EVP_CIPHER_CTX_free(ctx);
    return ek;
}

bool decrypt_key(const EncryptedKey& ek, const std::string& password,
                 std::vector<uint8_t>& priv_key_out) {
    if (ek.ciphertext.size() < 16) return false;
    uint8_t enc_key[32];
    derive_key_from_password(password, ek.salt.data(), ek.salt.size(), enc_key, 32);

    size_t plain_len = ek.ciphertext.size() - 16;
    priv_key_out.resize(plain_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, enc_key, ek.nonce.data());
    int outl = 0;
    EVP_DecryptUpdate(ctx, priv_key_out.data(), &outl,
                      ek.ciphertext.data(), static_cast<int>(plain_len));
    // Set tag
    uint8_t tag[16];
    std::memcpy(tag, ek.ciphertext.data() + plain_len, 16);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    int final_len = 0;
    int ret = EVP_DecryptFinal_ex(ctx, priv_key_out.data() + outl, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    if (ret != 1) { priv_key_out.clear(); return false; }
    return true;
}

bool save_encrypted_key(const std::string& path, const EncryptedKey& ek) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t sl = static_cast<uint32_t>(ek.salt.size());
    uint32_t nl = static_cast<uint32_t>(ek.nonce.size());
    uint32_t cl = static_cast<uint32_t>(ek.ciphertext.size());
    f.write(reinterpret_cast<const char*>(&sl), 4);
    f.write(reinterpret_cast<const char*>(ek.salt.data()), sl);
    f.write(reinterpret_cast<const char*>(&nl), 4);
    f.write(reinterpret_cast<const char*>(ek.nonce.data()), nl);
    f.write(reinterpret_cast<const char*>(&cl), 4);
    f.write(reinterpret_cast<const char*>(ek.ciphertext.data()), cl);
    return f.good();
}

bool load_encrypted_key(const std::string& path, EncryptedKey& ek) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    auto read_vec = [&](std::vector<uint8_t>& v) {
        uint32_t len = 0;
        f.read(reinterpret_cast<char*>(&len), 4);
        v.resize(len);
        f.read(reinterpret_cast<char*>(v.data()), len);
    };
    read_vec(ek.salt);
    read_vec(ek.nonce);
    read_vec(ek.ciphertext);
    return f.good();
}

bool save_keypair(const std::string& key_path, const std::string& pub_path,
                  const KeyPair& kp) {
    {
        std::ofstream f(key_path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(kp.private_key.data()), 32);
    }
    {
        std::ofstream f(pub_path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(kp.public_key.data()), 33);
    }
    return true;
}

bool load_keypair(const std::string& key_path, const std::string& pub_path,
                  KeyPair& kp) {
    {
        std::ifstream f(key_path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> priv(32);
        f.read(reinterpret_cast<char*>(priv.data()), 32);
        if (!f) return false;
        kp = keypair_from_seed(priv.data(), 32);
    }
    (void)pub_path; // pubkey re-derived
    return true;
}

KeyPair load_or_generate_node_keypair(const std::string& keys_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(keys_dir);
    std::string key_path = keys_dir + "/node.key";
    std::string pub_path = keys_dir + "/node.pub";
    if (fs::exists(key_path)) {
        KeyPair kp;
        if (load_keypair(key_path, pub_path, kp)) return kp;
    }
    auto kp = generate_keypair();
    save_keypair(key_path, pub_path, kp);
    return kp;
}

} // namespace mc::crypto
