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

} // namespace mc
