#pragma once

#include "../core/chain.h"
#include "../core/block.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mc {

namespace net { class RatsLink; }

// SyncManager — fetches blocks from peers on startup, validates them
// independently of the source, and connects the longest valid chain that
// satisfies hardcoded_checkpoints() to our local chain.
//
// Anti-eclipse policy:
//   1. Require min_independent_peers tips before adopting any peer
//      chain (default 2). Cuts the cost of an eclipse attack from
//      "one malicious peer" to "all peers in the configured minimum."
//   2. Reject any pulled block whose (height, hash) contradicts a
//      hardcoded checkpoint. Even if every peer is malicious they
//      can't forge a chain that matches our baked-in audit points.
//   3. Run the same five-step block validation rebuild_derived_state
//      uses (block.validate + prev_hash + confirmation quorum +
//      chromaprint uniqueness + apply_transactions). A bad block
//      stops the sync at that height; nothing past it is connected.
//
// SyncManager runs a single best-effort sync pass on start(). For an
// always-on background sync loop (re-poll peers every N minutes), add
// a periodic_loop_ thread in a follow-up.
class SyncManager {
public:
    SyncManager(Chain& chain, net::RatsLink& rats,
                uint32_t min_independent_peers = 2);
    ~SyncManager();

    // Runs the sync pass in a background thread. Safe to call once.
    void start();
    void stop();

    // Called by node_main from its MC_REPLY_TYPE handler so SyncManager
    // can correlate inbound replies back to the request that's waiting
    // on them. Passes the raw reply JSON string (envelope shape:
    //   { req_id, status, body }).
    void on_rpc_reply(const std::string& peer_id,
                      const std::string& reply_json);

    // node_main hands us the mini-node's librats peer_id once the rats
    // link handshakes (and the wallet field from the routes.get
    // response so credit attribution still works). Without it
    // SyncManager has no idea which connected librats peer is a relay
    // vs a full node, and run_pass falls back to "query every peer
    // directly" — which silently fails because the mini-node has no
    // chain to answer chain.tip with.
    void set_mini_node_peer_id(const std::string& peer_id);

private:
    void run_pass();

    // Round-trip a single typed RPC. Returns the parsed `body` field on
    // success, nullopt on timeout / "not_found" / malformed reply.
    // body_json may be empty for verbs that take no parameters.
    std::optional<std::string>
    rpc(const std::string& peer_id,
        const std::string& verb,
        const std::string& body_json,
        uint32_t           timeout_ms = 5000);

    // High-level wrappers — the three verbs SyncManager talks to.
    struct PeerTip {
        uint32_t height;
        Hash256  hash;
        uint64_t timestamp_ms;
    };
    std::optional<PeerTip>
    peer_chain_tip(const std::string& peer_id);
    std::vector<Hash256>
    peer_list_block_hashes(const std::string& peer_id,
                            uint32_t from_height,
                            uint32_t max_count = 128);
    std::optional<std::vector<uint8_t>>
    peer_get_block(const std::string& peer_id,
                    const Hash256& hash);

    bool ingest_block(const std::vector<uint8_t>& bytes,
                       const Hash256& expected_hash,
                       const Hash256& prev_hash);

    bool satisfies_checkpoints(uint32_t height, const Hash256& hash) const;

    Chain&            chain_;
    net::RatsLink&    rats_;
    uint32_t          min_peers_;
    std::thread       worker_;
    std::atomic<bool> running_{false};

    // Discovery state: the mini-node's librats peer_id once we know it,
    // plus the latest routes.get response decoded into (full_node_peer_id,
    // public_address, reachability, ...) tuples. discover_full_nodes()
    // populates this; run_pass() consumes it.
    mutable std::mutex                       discovery_mu_;
    std::string                              mini_peer_id_;
    struct DiscoveredFull {
        std::string node_id;
        std::string rats_peer_id;
        std::string public_address;
        std::string reachability;
    };
    std::vector<DiscoveredFull>              discovered_;
    void discover_full_nodes();

    // Request correlation. Each outbound RPC stashes its req_id here so
    // the inbound reply path can hand the JSON back to the waiting
    // thread. Pending entries timeout-clean themselves out.
    struct PendingRpc {
        bool        delivered = false;
        std::string reply_json;
    };
    std::mutex                              pending_mu_;
    std::condition_variable                 pending_cv_;
    std::unordered_map<std::string, PendingRpc> pending_;
    std::atomic<uint64_t>                   next_req_id_{0};
};

} // namespace mc
