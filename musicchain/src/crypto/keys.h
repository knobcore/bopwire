#pragma once
#include "../core/block.h"
#include <string>
#include <vector>

namespace mc::crypto {

// secp256k1 key pair
struct KeyPair {
    std::vector<uint8_t> private_key;  // 32 bytes
    PubKey33             public_key;   // 33 bytes compressed
    Address              address;      // last 20 bytes of SHA256(uncompressed_pubkey)
};

// Generate a new random keypair
KeyPair generate_keypair();

// Derive keypair from 32-byte seed
KeyPair keypair_from_seed(const uint8_t* seed, size_t len);

// Derive a deterministic 32-byte seed from a passphrase using
// PBKDF2-HMAC-SHA512. Domain-separated by the salt string so a
// passphrase reused elsewhere (encrypted key file etc.) doesn't
// produce the same key. Used by the founder bootstrap so the founder's
// private key is "the passphrase in your head" with no on-disk
// material.
std::vector<uint8_t> derive_seed_pbkdf2_sha512(const std::string& passphrase,
                                               const std::string& salt,
                                               uint32_t iterations);

// Derive keypair from hex private key string
bool keypair_from_hex(const std::string& priv_hex, KeyPair& out);

// Derive Address from compressed public key
Address address_from_pubkey(const PubKey33& pubkey);

// ---- Encrypted key file (Argon2id + AES-256-GCM) -------------------

struct EncryptedKey {
    std::vector<uint8_t> salt;        // 32 bytes for Argon2id
    std::vector<uint8_t> nonce;       // 12 bytes for AES-GCM
    std::vector<uint8_t> ciphertext;  // encrypted private key + tag
};

EncryptedKey encrypt_key(const std::vector<uint8_t>& priv_key,
                         const std::string& password);

bool decrypt_key(const EncryptedKey& ek,
                 const std::string& password,
                 std::vector<uint8_t>& priv_key_out);

// Save / load encrypted key to file
bool save_encrypted_key(const std::string& path, const EncryptedKey& ek);
bool load_encrypted_key(const std::string& path, EncryptedKey& ek);

// Node keypair helpers (unencrypted binary files for node use)
bool save_keypair(const std::string& key_path, const std::string& pub_path,
                  const KeyPair& kp);
bool load_keypair(const std::string& key_path, const std::string& pub_path,
                  KeyPair& kp);

// Generate or load node keypair from directory
KeyPair load_or_generate_node_keypair(const std::string& keys_dir);

} // namespace mc::crypto
