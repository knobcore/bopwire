#pragma once
#include <string>

namespace mc::crypto {

// Portable, password-encrypted keystore for a short secret (the wallet
// mnemonic). scrypt(password) -> AES-256-GCM, serialized as a small JSON
// document. ONE implementation so the player (via the C API) and both nodes
// produce byte-compatible files. Two uses, same format, differing only in where
// the password comes from:
//   * device-bound local at-rest storage (password = device fingerprint [+PIN]),
//   * passphrase-protected export/import (password = a user passphrase).

// Encrypt `plaintext` under `password`; returns the keystore JSON, or an empty
// string on failure. `addr` is stored as an informational field only (shown
// before decrypt); the GCM tag authenticates only salt-derived key + ciphertext.
std::string keystore_encrypt(const std::string& plaintext,
                             const std::string& password,
                             const std::string& addr = "");

// Decrypt a keystore produced by keystore_encrypt. Returns true + sets `out` on
// success; false on malformed JSON, wrong password, or GCM auth failure.
bool keystore_decrypt(const std::string& keystore_json,
                      const std::string& password,
                      std::string& out);

// Best-effort read of the informational "addr" field without decrypting.
std::string keystore_address(const std::string& keystore_json);

}  // namespace mc::crypto
