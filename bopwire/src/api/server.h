#pragma once
#include <cstdint>
#include "../core/chain.h"
#include "../consensus/candidate.h"
#include "../network/manager.h"
#include "../crypto/keys.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <array>
#include <atomic>
#include <thread>
#include <functional>

namespace mc::api {

// Per-heartbeat sample: wall-clock time when the node received the
// heartbeat, plus the position_ms inside the song the player claimed it
// was at. Used at session.complete to decide whether the listener
// genuinely consumed enough audio to deserve a mint.
struct HeartbeatSample {
    uint64_t wall_ms;
    uint64_t position_ms;
};

// Active play session (in-memory)
struct PlaySession {
    Hash256  session_id;
    Hash256  content_hash;
    Hash256  block_hash;
    Address  player_address;
    // Per-stream reward lanes (PlayProof v2): the peer that SERVED the bytes and
    // the relay (mini-node) that carried the stream, reported by the player at
    // session.start. Zero when the player didn't report them (legacy / direct).
    Address  seeder_address{};
    Address  mini_node_address{};
    uint64_t start_timestamp;
    uint64_t last_heartbeat;
    uint32_t heartbeat_count;
    bool     completed = false;
    // Claimed under sessions_mutex_ at the TOP of post_session_complete so two
    // concurrent completes for the same session_id can't both mint (and both
    // double-increment the daily counter): the second sees completing==true and
    // is rejected. Cleared on any error/reject exit so a legit retry can proceed;
    // subsumed by `completed` on success.
    bool     completing = false;

    // ---- Anti-farm device binding (#5, node-local, consensus-invisible) ----
    // device_id is SERVER-derived at session.start from the client's hardware
    // attestation (empty when the client sends none — never the wallet). It
    // keys the per-device daily coverage cap and, together with player_address,
    // the concurrency slot. att_level is "hardware"|"software"|"none".
    std::string device_id;
    std::string att_level;
    // Concurrency slot: device_id + "|" + player_address. Held from start until
    // the first terminal event (complete / reject / expiry). slot_held makes
    // release idempotent so a ret[ried complete or a reaper sweep can't
    // double-decrement live_by_device_.
    std::string slot_key;
    bool     slot_held  = false;
    // UTC day bucket (now_ms/86400000) bound ONCE at start and carried here so
    // the complete-time counter increment and any re-check use the SAME bucket
    // the start-time check used (a play that straddles UTC midnight is not split
    // across two buckets).
    uint64_t day_bucket = 0;

    // Position samples in arrival order. The last entry is also reflected
    // in last_heartbeat for fast expiry checks. We don't bound this
    // explicitly because a session times out after TIMEOUT_MS of silence
    // and is dropped from the map anyway.
    std::vector<HeartbeatSample> samples;

    static constexpr uint64_t TIMEOUT_MS = 120000;
    bool is_expired(uint64_t now_ms) const {
        return !completed && (now_ms - last_heartbeat) > TIMEOUT_MS;
    }
};

/// `HttpServer` is the verb container — every API request (whether arriving
/// over librats / mc_rats_quic or the new HTTP/3 listener) dispatches to
/// one of the verb_* methods below. It does not own a socket of its own
/// anymore; the HTTP/3 listener lives in `transport/h3_server.{h,cpp}` and
/// calls these methods, and the rats RPC router (`api/rats_api.cpp`) does
/// the same on the QUIC peer channel.
class HttpServer {
public:
    HttpServer(Chain& chain, CandidateManager& candidates,
               net::NetworkManager& network, Database& db,
               const net::NodeConfig& config,
               const mc::crypto::KeyPair& keypair);
    ~HttpServer();

    bool start();
    void stop();

    // Phase 1 wiring: RatsApi injects its ingest_tx here so post_session_complete
    // publishes the play reward as an on-chain (flooded + mempooled) MINT tx
    // instead of a local DB write — the producer mines it and every node applies
    // it through the block-apply forge gate, so balances replicate + survive
    // resync. Set once at construction, before any session can complete.
    void set_ingest_tx(std::function<bool(const std::string&)> cb) { ingest_tx_cb_ = std::move(cb); }
    // Phase 3: the epoch-close worker publishes a settlement's companion body
    // through here (-> RatsApi::ingest_settle_body: store sb:<root> + flood).
    void set_settle_body(std::function<bool(const std::string&)> cb) { settle_body_cb_ = std::move(cb); }

    // ---- Verb handlers -----------------------------------------------
    std::pair<int, std::string> verb_status()                                { return get_status(); }
    std::pair<int, std::string> verb_dht_peers()                             { return get_dht_peers(); }
    std::pair<int, std::string> verb_songs_list()                            { return get_songs_list(); }
    std::pair<int, std::string> verb_song_get(const std::string& hash)       { return get_song(hash); }
    std::pair<int, std::string> verb_wallet_balance(const std::string& addr) { return get_balance(addr); }
    std::pair<int, std::string> verb_wallet_escrow_balance(const std::string& addr)
        { return get_escrow_balance(addr); }
    std::pair<int, std::string> verb_wallet_nonce(const std::string& addr)   { return get_wallet_nonce(addr); }
    // Submit a SENDER-SIGNED TransferTx. Reuses post_transfer verbatim so the
    // rats path has IDENTICAL security to the HTTP route: verify_signature
    // (ECDSA + pubkey→from_address) + nonce replay check + balance, then mempool.
    // The node never moves funds without the sender's signature.
    std::pair<int, std::string> verb_wallet_transfer(const std::string& body)
        { return post_transfer(body); }
    std::pair<int, std::string> verb_session_start(const std::string& body)
        { return post_session_start(body); }
    std::pair<int, std::string> verb_session_heartbeat(const std::string& sid,
                                                       const std::string& body)
        { return post_session_heartbeat(sid, body); }
    std::pair<int, std::string> verb_session_complete(const std::string& sid,
                                                      const std::string& body)
        { return post_session_complete(sid, body); }
    std::pair<int, std::string> verb_songs_search_query(const std::string& q);
    std::pair<int, std::string> verb_songs_search_artist(const std::string& a);
    std::pair<int, std::string> verb_songs_search_genre(const std::string& g);

    // verb_song_audio + verb_upload_submit + verb_upload_status were
    // removed when the chain stopped ingesting and serving audio bytes.
    // Audio lives only with the players that announced themselves via
    // fingerprint.submit; clients hit them via the swarm list returned
    // from stream.open in rats_api.cpp.

private:
    Chain&                chain_;
    CandidateManager&     candidates_;
    net::NetworkManager&  network_;
    Database&             db_;
    net::NodeConfig       config_;
    mc::crypto::KeyPair   node_keypair_;
    // -> RatsApi::ingest_tx (Phase 1). Null until wired; post_session_complete
    // falls back to a direct apply only if this is unset (should never happen in
    // the node, which always wires RatsApi).
    std::function<bool(const std::string&)> ingest_tx_cb_;
    // -> RatsApi::ingest_settle_body (Phase 3). Used by the epoch-close worker.
    std::function<bool(const std::string&)> settle_body_cb_;
    // Phase 3 batched settlement (opt-in via BOPWIRE_BATCH_SETTLE): when ON,
    // session.complete ACCRUES the signed PlayProof to accplay:<epoch>:<session>
    // instead of emitting a per-play MINT, and the reaper thread closes epochs
    // into one SETTLEMENT_MINT (~100-1000x fewer txs). Default OFF keeps the
    // low-volume deploy on the simpler Phase-1 per-play path.
    std::atomic<bool>                       batch_settle_enabled_{false};
    // Node-local epoch batching interval (ms). NON-consensus: only affects which
    // plays share a settlement tx, never the credited amounts.
    static constexpr uint64_t               EPOCH_MS = 10000;
    uint64_t                                last_epoch_swept_ = 0;
    void settle_epoch_sweep();   // reaper-driven: emit closed epochs

    mutable std::mutex                            sessions_mutex_;
    std::unordered_map<std::string, PlaySession>  sessions_;

    // ---- Anti-farm per-device enforcement (node-local, consensus-invisible) ----
    // Live concurrency counters keyed on "<device_id>|<player_address>". Guarded
    // by sessions_mutex_ (same lock as sessions_) so check-and-increment at
    // session.start is atomic against the RPC workers.
    std::unordered_map<std::string, uint32_t>     live_by_device_;
    // Sharded locks so two concurrent completes for the SAME device serialize
    // their read-modify-write of the durable daily counter across the WHOLE
    // apply_mint + db_.write region (the batch is not committed by apply_mint).
    static constexpr size_t                       kDevShards = 64;
    std::array<std::mutex, kDevShards>            device_mint_mu_;
    // Dark by default: log would-reject but still admit/mint until an operator
    // sets BOPWIRE_DEVICE_CAP_ENFORCE=1 after a soak. The durable daily counter
    // is maintained in BOTH modes so the soak numbers are real.
    std::atomic<bool>                             device_cap_enforce_{false};
    std::thread                                   reaper_thread_;
    std::atomic<bool>                             reaper_stop_{false};

    // A device may hold a few concurrent live sessions (track-skip tear-down +
    // new stream, or 2 accounts on one PC). Cap is on (device,wallet), not the
    // device alone, so one wallet can't fan out — but a device is not bricked to
    // strictly serial. Concurrency does NOT apply to the offline replay path.
    static constexpr uint32_t kMaxConcurrentPerDevice = 3;
    // Free a concurrency slot after this much heartbeat silence (backgrounded /
    // crashed client) — well under PlaySession::TIMEOUT_MS so a device is never
    // locked out; the session itself lingers to TIMEOUT_MS for a late complete.
    static constexpr uint64_t kSlotIdleReleaseMs = 30000;
    // Time-based daily ceiling: cumulative COVERED listen-time per device per
    // UTC day. Length-agnostic (can't be gamed with short tracks) and encodes
    // "a device can't listen to more than a day in a day". ~20h leaves headroom
    // for a heavy real listener while flagging 24/7 bots.
    static constexpr uint64_t kDailyCoverageCapMs = 20ull * 60 * 60 * 1000;

    // Anti-farm helpers.
    void reaper_loop();
    // Decrement live_by_device_ for s's slot exactly once. Caller holds
    // sessions_mutex_. Idempotent via s.slot_held.
    void release_device_slot_locked(PlaySession& s);
    std::mutex& device_shard(const std::string& device_id) {
        return device_mint_mu_[std::hash<std::string>{}(device_id) % kDevShards];
    }

    // Route handlers returning JSON response body + HTTP status code
    std::pair<int, std::string> get_status();
    std::pair<int, std::string> get_peers();
    std::pair<int, std::string> get_dht_peers();
    std::pair<int, std::string> get_block(const std::string& hash_hex);
    std::pair<int, std::string> get_block_at_height(uint32_t height);
    std::pair<int, std::string> get_songs_list();
    std::pair<int, std::string> get_song(const std::string& content_hash_hex);
    std::pair<int, std::string> get_balance(const std::string& address_hex);
    std::pair<int, std::string> get_escrow_balance(const std::string& address_hex);
    std::pair<int, std::string> post_session_start(const std::string& body);
    std::pair<int, std::string> post_session_heartbeat(const std::string& session_id,
                                                        const std::string& body);
    std::pair<int, std::string> post_session_complete(const std::string& session_id,
                                                       const std::string& body);
    std::pair<int, std::string> post_wallet_create();
    std::pair<int, std::string> get_wallet_address();
    std::pair<int, std::string> get_wallet_nonce(const std::string& address_hex);
    std::pair<int, std::string> post_moderator_release(const std::string& body);
    std::pair<int, std::string> delete_song(const std::string& content_hash_hex,
                                             const std::string& body);
    std::pair<int, std::string> post_transfer(const std::string& body);
    std::pair<int, std::string> post_net_announce(const std::string& body);
    std::pair<int, std::string> post_sync_block(const std::string& body);

    // Session helpers
    std::string generate_session_id() const;
    PlaySession* find_session(const std::string& session_id);

    std::pair<int, std::string> _do_songs_search(const std::string& artist,
                                                 const std::string& genre,
                                                 const std::string& q);
};

} // namespace mc::api
