#pragma once
//
// Node identity as a PASSWORD-PROTECTED PORTABLE wallet. A full node or mini
// node's operating identity is a BIP39 mnemonic saved as an encrypted keystore
// (scrypt + AES-256-GCM) at <data_dir>/node-wallet.json (or an operator path).
// This replaces the old auto-generated node.key / plaintext mini-node.seed.
//
// Resolution:
//   * keystore exists  -> decrypt with `password`; if that fails and we're
//     interactive, prompt for the password on the terminal.
//   * keystore absent + interactive -> run a first-config TUI wizard
//     (create a new 12-word wallet or import one, set a password, show the
//     words to back up), save the encrypted keystore, and use it.
//   * keystore absent + headless -> return nullopt; the caller errors and
//     tells the operator to run once interactively (to create the wallet) or
//     pass --wallet-file/--wallet-password.
//
#include "../src/crypto/keys.h"   // mc::crypto::KeyPair

#include <optional>
#include <string>

namespace mc {

std::optional<mc::crypto::KeyPair> load_or_setup_node_identity(
    const std::string& data_dir,
    const std::string& wallet_file,   // explicit keystore path, or "" for default
    const std::string& password,      // from --wallet-password / config / env, or ""
    bool               interactive,   // true when a TUI/terminal is available
    const char*        role_label);   // "full node" | "mini-node" (shown in the wizard)

// Keystore path that the LAST load_or_setup_node_identity() call resolved
// (the operator's --wallet-file, else <data_dir>/node-wallet.json). Empty
// before the first call. The node's wallet TUI uses it to re-check the
// operator's password at unlock time without being told the path twice.
const std::string& node_wallet_keystore_path();

// Result of create_node_wallet(). `mnemonic` is the ONLY copy of the seed
// phrase that ever leaves this function — show it to the operator once and
// never write it to a log.
struct CreatedWallet {
    std::string mnemonic;   // 12 BIP39 words
    std::string address;    // EIP-55 checksummed 0x… address
    std::string path;       // keystore file that was written
};

// Non-interactive wallet creation (`bopwire-node create-wallet`).
//
// Generates a fresh 12-word BIP39 wallet, encrypts the mnemonic under
// `password` with the SAME keystore format load_or_setup_node_identity()
// consumes (scrypt + AES-256-GCM JSON), and writes it to `out_path` with
// owner-only permissions. Refuses to clobber an existing file unless
// `overwrite`. Returns nullopt and sets `err` on failure.
//
// The resulting file is fully portable: it carries nothing node-local, is
// only ever read (never rewritten) at load time, and therefore the same
// keystore + password pair can be deployed to any number of full or mini
// nodes simultaneously.
std::optional<CreatedWallet> create_node_wallet(const std::string& out_path,
                                                const std::string& password,
                                                bool               overwrite,
                                                std::string&       err);

// Same as create_node_wallet(), but for an ALREADY-KNOWN mnemonic — the
// `--seed "<twelve words>"` path. Validates the phrase, encrypts it under
// `password` and writes the identical keystore format, so a wallet can be
// moved to a new node from the command line alone. Returns nullopt + `err`
// on an invalid phrase, a weak password, or an unwritable path.
std::optional<CreatedWallet> write_node_wallet(const std::string& out_path,
                                               const std::string& mnemonic,
                                               const std::string& password,
                                               bool               overwrite,
                                               std::string&       err);

// Write `contents` to `path` with owner-read/write-only permissions
// (0600). Used for the keystore and the optional --seed-out file so a
// secret never lands on disk world-readable. Returns false on any error.
bool write_secret_file(const std::string& path, const std::string& contents);

} // namespace mc
