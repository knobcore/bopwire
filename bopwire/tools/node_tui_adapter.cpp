// Public full-node UI adapter.
//
// bopwire-node ships a READ-ONLY monitor — no wallet/founder/moderator
// tooling (all of that lives in the private bopwire-admin binary). This
// file satisfies the run_tui() contract declared in node_tui.h by building
// a MonitorState from the live Chain/Database/etc and handing it to the
// shared, chain-free run_monitor_tui() (monitor_tui.cpp), the same monitor
// the mini-node renders.
//
// The private admin binary provides its OWN run_tui() (tools/admin/) and is
// NOT built from this file — so the public repo contains no moderation UI.

#include "node_tui.h"
#include "monitor_tui.h"

#include "../src/core/chain.h"
#include "../src/storage/database.h"
#include "../src/store/swarm.h"
#include "../src/network/manager.h"
#include "../src/crypto/hash.h"   // escrow_address_for
#include "../src/crypto/keys.h"
#include "../src/tokens/ledger.h"

#include <string>

namespace mc::ui {

void start_log_capture() { monitor_start_log_capture(); }
void stop_log_capture()  { monitor_stop_log_capture(); }

void run_tui(mc::api::HttpServer&    /*http*/,
             mc::api::RatsApi&       /*api*/,
             mc::Chain&              chain,
             mc::Database&           db,
             mc::store::SwarmIndex&  swarm,
             mc::net::NetworkManager& net,
             mc::CandidateManager&   /*candidates*/,
             const mc::crypto::KeyPair& node_keypair,
             const std::string&      /*data_dir*/,
             std::atomic<bool>&      should_quit) {
    MonitorState st;
    st.title = "bopwire-node";
    // No export/import: the full node's identity (node.key) is a raw key,
    // not a portable mnemonic. Founder-wallet portability is admin-only.
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

    monitor_start_log_capture();
    run_monitor_tui(st, should_quit);
}

} // namespace mc::ui
