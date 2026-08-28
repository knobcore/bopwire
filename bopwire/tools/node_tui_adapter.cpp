// Public full-node UI adapter.
//
// bopwire-node renders the SHARED monitor screen (tools/monitor_tui.cpp) —
// F1 status (wallet address, balance, escrow, chain height, songs, peers)
// and F2 logs — PLUS the password-protected WALLET page (F3) that the same
// module now offers: unlock, address, balance, send, receive.
//
// This file satisfies the run_tui() contract declared in node_tui.h by
// building a MonitorState from the live Chain/Database/RatsApi and handing it
// to the chain-free run_monitor_tui(). All the chain-aware work (reading the
// balance, checking the wallet password against the keystore, building and
// signing a TransferTx) lives HERE, behind std::function, so the monitor
// module itself stays dependency-light for the mini node and the other
// programs that render the same screen.
//
// There is deliberately NO moderator, founder or bootstrap surface: those
// live in the private bopwire-bootstrap / bopwire-modclient binaries, built
// from tools/admin/ and never from this file.
//
// Security notes:
//   * The wallet password is never stored. Unlock decrypts the on-disk
//     keystore (mc::node_wallet_keystore_path()) and the buffer is zeroed
//     immediately; the page re-locks itself after an idle period.
//   * Sending uses the SAME canonical path as the rats `wallet.transfer`
//     verb: a sender-signed TransferTx handed to RatsApi::ingest_tx, which
//     mempools it with a pt: submit_ms stamp and floods it to every peer.
//     The node never moves funds outside a signed, flooded transaction.
//   * Nothing secret is ever written to std::cout or the F2 log ring.

#include "node_tui.h"
#include "monitor_tui.h"
#include "node_wallet.h"   // node_wallet_keystore_path()

#include "../src/core/chain.h"
#include "../src/storage/database.h"
#include "../src/store/swarm.h"
#include "../src/network/manager.h"
#include "../src/api/rats_api.h"
#include "../src/core/transaction.h"
#include "../src/crypto/bip39.h"
#include "../src/crypto/hash.h"   // escrow_address_for
#include "../src/crypto/keys.h"
#include "../src/crypto/keystore.h"
#include "../src/crypto/signature.h"
#include "../src/tokens/ledger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace mc::ui {
namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Chain-aware half of the F3 wallet page. Owned by run_tui for the lifetime
// of the TUI; the MonitorState callbacks capture a pointer to it.
class WalletBackend {
public:
    WalletBackend(mc::api::RatsApi& api, mc::Database& db,
                  const mc::crypto::KeyPair& kp, std::string keystore)
        : api_(api), db_(db), kp_(kp), keystore_(std::move(keystore)) {}

    bool has_keystore() const { return !keystore_.empty(); }

    // Verify the operator's password against the keystore this node was
    // unlocked from, and confirm it really is the running identity — an
    // unrelated keystore must not unlock somebody else's balance view.
    bool unlock(const std::string& password) {
        if (keystore_.empty()) return false;
        std::string mnemonic;
        if (!mc::crypto::keystore_decrypt(keystore_, password, mnemonic)) return false;
        auto kp = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        return kp && kp->address == kp_.address;
    }

    // Parse, sanity-check, sign and flood a transfer. Returns
    // {ok, message-for-the-screen}.
    std::pair<bool, std::string> send(const std::string& to_str,
                                      const std::string& amount_str) {
        mc::Address to{};
        if (!mc::crypto::parse_address_checksummed(to_str, to))
            return {false, "send: not a valid address"};
        uint64_t amount = 0;
        if (!mc::Ledger::parse_balance(amount_str, amount) || amount == 0)
            return {false, "send: bad amount"};
        const uint64_t bal = db_.get_balance(kp_.address);
        if (amount > bal)
            return {false, "send: amount exceeds balance (" +
                           mc::Ledger::format_balance(bal) + " mc)"};

        // Nonce: the chain's next-expected value, floored by what this
        // session already spent, so two sends issued before the first one
        // mines don't collide on the same nonce.
        uint64_t nonce = db_.get_nonce(kp_.address);
        if (nonce < nonce_floor_) nonce = nonce_floor_;

        mc::TransferTx tx{};
        tx.from_address = kp_.address;
        tx.to_address   = to;
        tx.amount       = amount;
        tx.nonce        = nonce;
        tx.from_pubkey  = kp_.public_key;
        {
            const auto msg = tx.sign_message();
            const auto h   = mc::crypto::sha256(msg.data(), msg.size());
            tx.signature   = mc::crypto::sign_ecdsa(h, kp_.private_key);
        }
        if (!tx.verify_signature())
            return {false, "send: internal sign/verify mismatch"};

        nlohmann::json txe = {
            {"tx",        mc::crypto::to_hex(tx.serialize())},
            {"submit_ms", now_ms()},
        };
        if (!api_.ingest_tx(txe.dump(), /*broadcast_if_new=*/true))
            return {false, "send: rejected by the mempool (duplicate or invalid)"};

        nonce_floor_ = nonce + 1;
        return {true, "sent " + mc::Ledger::format_balance(amount) + " mc - tx " +
                      mc::crypto::to_hex(tx.tx_hash()).substr(0, 12) + "... queued"};
    }

private:
    mc::api::RatsApi&          api_;
    mc::Database&              db_;
    const mc::crypto::KeyPair& kp_;
    const std::string          keystore_;
    uint64_t                   nonce_floor_ = 0;
};

}  // namespace

void start_log_capture() { monitor_start_log_capture(); }
void stop_log_capture()  { monitor_stop_log_capture(); }

void run_tui(mc::api::HttpServer&    /*http*/,
             mc::api::RatsApi&       api,
             mc::Chain&              chain,
             mc::Database&           db,
             mc::store::SwarmIndex&  swarm,
             mc::net::NetworkManager& net,
             mc::CandidateManager&   /*candidates*/,
             const mc::crypto::KeyPair& node_keypair,
             const std::string&      data_dir,
             std::atomic<bool>&      should_quit) {
    MonitorState st;
    st.title = "bopwire-node";
    // No seed export/import chips: founder-wallet portability is an admin-only
    // concern, and this node's own wallet travels as the keystore file (or via
    // `bopwire-node create-wallet --seed`), not from this screen.
    st.seed_path = "";

    st.wallet_address = [&node_keypair]() {
        return mc::crypto::to_checksum_hex(node_keypair.address);
    };
    st.balance = [&db, &node_keypair]() {
        return mc::Ledger::format_balance(db.get_balance(node_keypair.address)) + " mc";
    };
    st.escrow = [&db, &node_keypair]() {
        const Address esc = mc::crypto::escrow_address_for(node_keypair.address);
        return mc::Ledger::format_balance(db.get_balance(esc)) + " mc";
    };
    st.chain_height = [&chain]() { return std::to_string(chain.tip().height); };
    st.songs        = [&swarm]() { return std::to_string(swarm.song_count()); };
    st.peers        = [&net]()   { return std::to_string(net.peer_count()); };

    // ---- F3 wallet page -------------------------------------------------
    // The keystore this process was unlocked from — node_wallet.cpp records
    // it, so --wallet-file / --seed are honoured without threading the path
    // through run_tui()'s (shared) signature. Fall back to the default.
    std::string ks_path = mc::node_wallet_keystore_path();
    if (ks_path.empty()) ks_path = data_dir + "/node-wallet.json";
    static std::unique_ptr<WalletBackend> wallet;
    wallet = std::make_unique<WalletBackend>(api, db, node_keypair,
                                             read_file(ks_path));
    if (wallet->has_keystore()) {
        // Only offer the page when there is a keystore to check the password
        // against — an unguarded send screen would be worse than none.
        WalletBackend* w = wallet.get();
        st.wallet_unlock = [w](const std::string& pw) { return w->unlock(pw); };
        st.wallet_send   = [w](const std::string& to, const std::string& amount) {
            return w->send(to, amount);
        };
    } else {
        std::cerr << "[wallet] no keystore at " << ks_path
                  << " - F3 wallet page disabled\n";
    }

    monitor_start_log_capture();
    run_monitor_tui(st, should_quit);
    // Hand stdout/stderr back so the shutdown log reaches the console.
    monitor_stop_log_capture();
}

} // namespace mc::ui
