#include "server.h"
#include "routes.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include "../tokens/ledger.h"
#include "../tokens/mint.h"
#include "device_attestation.h"
#include "../core/transaction.h"
#include "../core/merkle.h"        // merkle_root_bytes (settlement emit, Phase 3)
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <random>
#include <algorithm>
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace mc::api {

// ---- Multipart parser -----------------------------------------------

struct MultipartPart {
    std::string          name;
    std::string          filename;
    std::vector<uint8_t> data;
};

static std::vector<MultipartPart> parse_multipart(const std::string& body,
                                                    const std::string& boundary) {
    std::vector<MultipartPart> parts;
    std::string delim     = "\r\n--" + boundary;
    std::string first_del = "--"     + boundary;

    size_t pos = body.find(first_del);
    if (pos == std::string::npos) return parts;
    pos += first_del.size();
    if (pos + 1 < body.size() && body[pos] == '\r') pos += 2;
    else return parts;

    while (pos < body.size()) {
        size_t next = body.find(delim, pos);
        if (next == std::string::npos) break;

        std::string part_str = body.substr(pos, next - pos);
        size_t hend = part_str.find("\r\n\r\n");
        if (hend == std::string::npos) break;

        MultipartPart part;
        std::string hdrs = part_str.substr(0, hend);
        size_t h = 0;
        while (h < hdrs.size()) {
            size_t nl = hdrs.find("\r\n", h);
            if (nl == std::string::npos) nl = hdrs.size();
            std::string line  = hdrs.substr(h, nl - h);
            std::string lline = line;
            for (auto& c : lline)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lline.find("content-disposition") == 0) {
                auto np = line.find("name=\"");
                if (np != std::string::npos) {
                    np += 6;
                    auto ne = line.find('"', np);
                    if (ne != std::string::npos) part.name = line.substr(np, ne - np);
                }
                auto fp = line.find("filename=\"");
                if (fp != std::string::npos) {
                    fp += 10;
                    auto fe = line.find('"', fp);
                    if (fe != std::string::npos) part.filename = line.substr(fp, fe - fp);
                }
            }
            h = (nl >= hdrs.size()) ? nl : nl + 2;
        }
        size_t body_start = hend + 4;
        part.data.assign(part_str.begin() + body_start, part_str.end());
        parts.push_back(std::move(part));

        pos = next + delim.size();
        if (pos + 1 < body.size() && body[pos] == '-') break; // terminal "--"
        if (pos + 1 < body.size() && body[pos] == '\r') pos += 2;
        else break;
    }
    return parts;
}

static std::string extract_boundary(const std::string& content_type) {
    auto p = content_type.find("boundary=");
    if (p == std::string::npos) return {};
    std::string b = content_type.substr(p + 9);
    // Strip optional quotes
    if (!b.empty() && b[0] == '"') b = b.substr(1);
    auto q = b.find('"');
    if (q != std::string::npos) b = b.substr(0, q);
    // Strip trailing whitespace/semicolons
    while (!b.empty() && (b.back() == ' ' || b.back() == ';')) b.pop_back();
    return b;
}

static uint64_t now_ms_api() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

namespace {
// Minimal scope-exit so post_session_complete releases the device concurrency
// slot on EVERY return path (gate reject, mint fail, success) without repeating
// the release at each of the ~8 exits.
template <class F> struct ScopeExit {
    F f;
    explicit ScopeExit(F fn) : f(std::move(fn)) {}
    ~ScopeExit() { f(); }
    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
};
} // namespace

// ---- Constructor / Destructor ---------------------------------------

HttpServer::HttpServer(Chain& chain, CandidateManager& candidates,
                       net::NetworkManager& network, Database& db,
                       const net::NodeConfig& config,
                       const mc::crypto::KeyPair& keypair)
    : chain_(chain), candidates_(candidates), network_(network), db_(db),
      config_(config), node_keypair_(keypair) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    // microhttpd HTTP/1.1 listener removed in Phase 2c. The class is kept
    // because both RatsApi (rats RPC) and H3Server (HTTP/3) dispatch to its
    // verb_* methods. No socket is opened here.
    //
    // Anti-farm per-device caps are DARK by default: unless the operator sets
    // BOPWIRE_DEVICE_CAP_ENFORCE=1, we log would-reject but still admit/mint so
    // a soak surfaces false-positives (skips, NAT, collisions) before enforcing.
    if (const char* e = std::getenv("BOPWIRE_DEVICE_CAP_ENFORCE"))
        device_cap_enforce_.store(e[0] == '1' || e[0] == 't' || e[0] == 'T');
    if (const char* e = std::getenv("BOPWIRE_BATCH_SETTLE"))
        batch_settle_enabled_.store(e[0] == '1' || e[0] == 't' || e[0] == 'T');
    // Stage 2, node-local and NON-consensus: refuse to ORIGINATE a mint whose
    // proof carries no listener co-signature. Lets an operator stop paying for
    // un-co-signed plays long before COSIGN_ACTIVATION_HEIGHT turns it into a
    // consensus rule, with zero fork risk (this node just declines to build the
    // tx; every other node's validation is unchanged).
    if (const char* e = std::getenv("BOPWIRE_REQUIRE_COSIGN"))
        require_cosign_.store(e[0] == '1' || e[0] == 't' || e[0] == 'T');
    if (require_cosign_.load())
        std::cout << "[cosign] REQUIRE_COSIGN on — this node only mints "
                     "three-party co-signed play proofs\n";
    if (batch_settle_enabled_.load())
        std::cout << "[settle] batched settlement ON (epoch=" << (EPOCH_MS/1000)
                  << "s) — session.complete accrues, reaper emits SETTLEMENT_MINT\n";
    std::cout << "[antifarm] per-device caps "
              << (device_cap_enforce_.load() ? "ENFORCING" : "dark (log-only)")
              << " (concurrency<=" << kMaxConcurrentPerDevice
              << "/device+wallet, daily<=" << (kDailyCoverageCapMs / 3600000)
              << "h covered/device)\n";
    reaper_stop_.store(false);
    reaper_thread_ = std::thread([this] { reaper_loop(); });
    return true;
}

void HttpServer::stop() {
    reaper_stop_.store(true);
    if (reaper_thread_.joinable()) reaper_thread_.join();
}

// Caller holds sessions_mutex_. Decrement the device's live concurrency slot
// exactly once per session lifetime; slot_held guards against a retried
// complete or a reaper sweep double-decrementing.
void HttpServer::release_device_slot_locked(PlaySession& s) {
    if (!s.slot_held) return;
    s.slot_held = false;
    if (s.slot_key.empty()) return;
    auto it = live_by_device_.find(s.slot_key);
    if (it != live_by_device_.end()) {
        if (it->second <= 1) live_by_device_.erase(it);
        else --it->second;
    }
}

// Background sweep (off the hot path): free a device's concurrency slot after
// kSlotIdleReleaseMs of heartbeat silence (backgrounded/crashed client) so it is
// never locked out, and erase long-dead / old-completed sessions so sessions_
// and the sweep stay bounded. is_session_used is durable in LevelDB, so dropping
// a completed in-memory session cannot enable a replay mint.
void HttpServer::reaper_loop() {
    while (!reaper_stop_.load()) {
        for (int i = 0; i < 10 && !reaper_stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (reaper_stop_.load()) break;
        const uint64_t now = now_ms_api();
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            for (auto it = sessions_.begin(); it != sessions_.end(); ) {
                PlaySession& s = it->second;
                const uint64_t idle = now - s.last_heartbeat;
                // Release the concurrency slot early on silence, but keep the
                // session for a late complete until TIMEOUT_MS.
                if (s.slot_held && idle > kSlotIdleReleaseMs)
                    release_device_slot_locked(s);
                const bool drop = (s.completed && idle > PlaySession::TIMEOUT_MS) ||
                                  s.is_expired(now);
                if (drop) {
                    release_device_slot_locked(s);   // idempotent
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Phase 3: close matured epochs into SETTLEMENT_MINT (no session lock).
        if (batch_settle_enabled_.load())
            settle_epoch_sweep();
    }
}

// Close every epoch older than the current one: gather its accrued PlayProofs,
// build the canonical body + Merkle root, flood the body, then sign + ingest one
// SETTLEMENT_MINT. Emit timing is NODE-LOCAL wall clock (non-consensus); the
// credited amounts come only from the flooded tx + body + committed state.
void HttpServer::settle_epoch_sweep() {
    const uint64_t now           = now_ms_api();
    const uint64_t current_epoch = now / EPOCH_MS;
    // Group closed-epoch proofs (epoch < current_epoch).
    std::map<uint64_t, std::vector<PlayProof>> by_epoch;
    std::map<uint64_t, std::vector<std::string>> keys_by_epoch;
    db_.for_each_with_prefix("accplay:", [&](const std::string& key,
                                             const std::string& val) {
        const std::string rest = key.substr(8);          // strip "accplay:"
        const auto colon = rest.find(':');
        if (colon == std::string::npos) return true;
        uint64_t e = 0;
        try { e = std::stoull(rest.substr(0, colon)); } catch (...) { return true; }
        if (e >= current_epoch) return true;             // not closed yet
        PlayProof pr;
        if (PlayProof::deserialize(
                reinterpret_cast<const uint8_t*>(val.data()), val.size(), pr)) {
            by_epoch[e].push_back(pr);
            keys_by_epoch[e].push_back(key);
        }
        return true;
    });
    for (auto& [epoch, proofs] : by_epoch) {
        if (proofs.empty()) continue;
        // M3: drop the accrual scratch ONLY once the settlement is durable on
        // chain (us: marker written by apply_settlement_mint). Until then we
        // (re-)emit idempotently (tx_hash dedup) and KEEP the scratch, so a
        // settlement that failed at block-build (missing v:/body/underfunded) is
        // never silently lost with its accrued plays.
        const std::string us_key =
            "us:" + db_.hex(config_.node_id) + ":" + std::to_string(epoch);
        if (db_.get(us_key).has_value()) {
            for (const auto& k : keys_by_epoch[epoch]) db_.del(k);
            continue;
        }
        std::sort(proofs.begin(), proofs.end(),
                  [](const PlayProof& a, const PlayProof& b) {
            int c = std::memcmp(a.content_hash.data(), b.content_hash.data(), 32);
            if (c != 0) return c < 0;
            return std::memcmp(a.session_id.data(), b.session_id.data(), 32) < 0;
        });
        std::vector<std::vector<uint8_t>> leaves;
        leaves.reserve(proofs.size());
        for (const auto& pr : proofs) leaves.push_back(pr.serialize());
        const Hash256 root = mc::merkle_root_bytes(leaves);
        // Flood + store the companion body FIRST (apply needs sb:<root> present).
        if (settle_body_cb_)
            settle_body_cb_(crypto::to_hex(serialize_settle_body(proofs)));
        // Build + sign the settlement tx.
        SettlementMintTx sm;
        sm.serving_node_id          = config_.node_id;
        sm.serving_node_wallet      = node_keypair_.address;
        sm.epoch_id                 = epoch;
        sm.constituents_merkle_root = root;
        sm.constituent_count        = static_cast<uint32_t>(proofs.size());
        auto msg = sm.sign_message();
        Hash256 h = crypto::sha256(msg.data(), msg.size());
        sm.node_signature = crypto::sign_ecdsa(h, node_keypair_.private_key);
        if (ingest_tx_cb_) {
            const std::string env = "{\"tx\":\"" + crypto::to_hex(sm.serialize())
                                  + "\",\"submit_ms\":" + std::to_string(now) + "}";
            ingest_tx_cb_(env);
        }
        // NOTE: scratch is NOT dropped here (M3) — it is kept until the us:
        // durability marker appears (checked at the top of the next sweep).
        std::cout << "[settle] emitted SETTLEMENT_MINT epoch " << epoch
                  << " with " << proofs.size() << " plays (scratch kept until durable)\n";
    }
}

// MHD access_handler + handle_request removed in Phase 2c — HTTP routing
// now lives in transport/h3_server.cpp on top of msh3. The verb methods
// below stay as the canonical implementation, called by both the HTTP/3
// dispatcher and `rats_api.cpp` over QUIC.
#if 0  // ---- legacy MHD path, kept as comment for reference ----
auto _legacy_handle_request_signature_only(void* conn,
                                       const std::string& url,
                                       const std::string& method,
                                       const std::string& body) {
    auto segs = parse_path(url);

    // GET /status
    if (method == "GET" && segs.size() == 1 && segs[0] == "status")
        return send_json(conn, 200, get_status().second);

    // GET /peers
    if (method == "GET" && segs.size() == 1 && segs[0] == "peers")
        return send_json(conn, 200, get_peers().second);

    // /net/dht-peers and /upload removed — both now flow over librats.
    (void)body;

    // GET /blocks/{hash}
    if (method == "GET" && segs.size() == 2 && segs[0] == "blocks")
        return send_json(conn, 200, get_block(segs[1]).second);

    // GET /blocks/height/{height}
    if (method == "GET" && segs.size() == 3 && segs[0] == "blocks" && segs[1] == "height") {
        uint32_t h = static_cast<uint32_t>(std::stoul(segs[2]));
        return send_json(conn, 200, get_block_at_height(h).second);
    }

    // GET /songs/search?artist=X&genre=Y&q=Z  (must come before /songs/{hash})
    if (method == "GET" && segs.size() == 2 && segs[0] == "songs" && segs[1] == "search")
        return send_json(conn, 200, get_songs_search(conn).second);

    // GET /songs
    if (method == "GET" && segs.size() == 1 && segs[0] == "songs")
        return send_json(conn, 200, get_songs_list().second);

    // GET /songs/{content_hash}
    if (method == "GET" && segs.size() == 2 && segs[0] == "songs")
        return send_json(conn, 200, get_song(segs[1]).second);

    // /songs/{hash}/stream removed: the full node never holds audio bytes
    // under the post-pivot architecture. Clients fetch by hitting a swarm
    // peer (see fingerprint.submit + stream.open swarm reply).

    // GET /balances/{address}
    if (method == "GET" && segs.size() == 2 && segs[0] == "balances")
        return send_json(conn, 200, get_balance(segs[1]).second);

    // POST /sessions/start
    if (method == "POST" && segs.size() == 2 && segs[0] == "sessions" && segs[1] == "start")
        return send_json(conn, 200, post_session_start(body).second);

    // POST /sessions/{id}/heartbeat
    if (method == "POST" && segs.size() == 3 && segs[0] == "sessions" && segs[2] == "heartbeat")
        return send_json(conn, 200, post_session_heartbeat(segs[1], body).second);

    // POST /sessions/{id}/complete
    if (method == "POST" && segs.size() == 3 && segs[0] == "sessions" && segs[2] == "complete")
        return send_json(conn, 200, post_session_complete(segs[1], body).second);

    // GET /wallet/address
    if (method == "GET" && segs.size() == 2 && segs[0] == "wallet" && segs[1] == "address")
        return send_json(conn, 200, get_wallet_address().second);

    // GET /wallet/{address}/nonce
    if (method == "GET" && segs.size() == 3 && segs[0] == "wallet" && segs[2] == "nonce")
        return send_json(conn, 200, get_wallet_nonce(segs[1]).second);

    // POST /wallet/create
    if (method == "POST" && segs.size() == 2 && segs[0] == "wallet" && segs[1] == "create")
        return send_json(conn, 200, post_wallet_create().second);

    // POST /moderator/release
    if (method == "POST" && segs.size() == 2 && segs[0] == "moderator" && segs[1] == "release")
        return send_json(conn, 200, post_moderator_release(body).second);

    // DELETE /songs/{hash}
    if (method == "DELETE" && segs.size() == 2 && segs[0] == "songs")
        return send_json(conn, 200, delete_song(segs[1], body).second);

    // POST /transactions/transfer
    if (method == "POST" && segs.size() == 2 && segs[0] == "transactions" && segs[1] == "transfer")
        return send_json(conn, 200, post_transfer(body).second);

    // POST /net/announce  (nodes registering themselves so clients can discover them)
    if (method == "POST" && segs.size() == 2 && segs[0] == "net" && segs[1] == "announce")
        return send_json(conn, 200, post_net_announce(body).second);

    // GET /sync/blocks?after_height=N&limit=K  (gossip block pull)
    if (method == "GET" && segs.size() == 2 && segs[0] == "sync" && segs[1] == "blocks")
        return send_json(conn, 200, get_blocks_after(conn).second);

    // POST /sync/block  (gossip block push to local chain)
    if (method == "POST" && segs.size() == 2 && segs[0] == "sync" && segs[1] == "block")
        return send_json(conn, 200, post_sync_block(body).second);

    // bootstrap-file route removed along with the BitTorrent seeder.

    return 0;
}
#endif  // legacy MHD path

// ---- Route implementations -----------------------------------------

std::pair<int, std::string> HttpServer::get_status() {
    auto tip = chain_.tip();
    json j = {
        {"version", "0.1.0"},
        {"block_height", tip.height},
        {"block_hash", crypto::to_hex(tip.hash)},
        {"peer_count", network_.peer_count()},
        {"synced", true},
        {"validator_enabled", config_.validator_enabled},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_peers() {
    auto peers = network_.connected_peers();
    json j = json::array();
    for (const auto& p : peers) j.push_back(p);
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_dht_peers() {
    auto entries = network_.get_dht_peers();
    json j = json::array();
    for (const auto& e : entries) {
        j.push_back({
            {"node_id",  crypto::to_hex(e.node_id)},
            {"ipv6",     e.ipv6_str()},
            {"p2p_port", e.p2p_port},
            {"api_port", e.api_port},
            {"api_url",  e.api_url()},
            {"last_seen_ms", e.last_seen_ms},
        });
    }
    // Also include own node info if we have an IPv6
    json own = {
        {"own_ipv6", network_.own_ipv6_str()},
        {"node_id",  crypto::to_hex(config_.node_id)},
    };
    json resp = {{"peers", j}, {"self", own}};
    return {200, resp.dump()};
}

std::pair<int, std::string> HttpServer::get_block(const std::string& hash_hex) {
    Hash256 h;
    if (!crypto::parse_hash256(hash_hex, h))
        return {400, R"({"error":"invalid hash"})"};
    auto block = chain_.get_block(h);
    if (!block) return {404, R"({"error":"block not found"})"};
    json j = {
        {"hash", crypto::to_hex(block->hash())},
        {"height", chain_.get_block_height(h).value_or(0)},
        {"version", block->header.version},
        {"prev_hash", crypto::to_hex(block->header.prev_hash)},
        {"timestamp", block->header.timestamp_ms},
        {"song", {
            {"content_hash", crypto::to_hex(block->song.content_hash)},
            {"title", block->song.title},
            {"artist", block->song.artist},
            {"genre", block->song.genre},
            {"duration_ms", block->song.duration_ms},
        }},
        {"transaction_count", block->transactions.size()},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_block_at_height(uint32_t height) {
    auto hash = chain_.get_block_hash(height);
    if (!hash) return {404, R"({"error":"block not found"})"};
    return get_block(crypto::to_hex(*hash));
}

std::pair<int, std::string> HttpServer::get_songs_list() {
    auto hashes = db_.get_all_song_hashes();
    json j = json::array();
    for (const auto& ch : hashes) {
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        // Moderator category hide lists — block bytes stay on chain, but
        // any match (artist / album / title) is filtered out of the
        // public listing. Reversible: clear the corresponding hide key
        // and the song reappears.
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album (meta->album))  continue;
        if (db_.is_hidden_title (meta->title))  continue;
        auto state = db_.get_song_state(ch);
        const auto rat = db_.get_rating_counts(ch);
        // Include the SHA256 of the compressed fingerprint so clients can
        // do an O(1) "is this hash already on chain?" check without
        // hashing every track locally. The full constellation stays out
        // of songs.list (~400 KB each — would balloon the response).
        auto fp = db_.get_fingerprint(ch);
        std::string fp_hash_hex;
        if (fp) {
            const Hash256 h = crypto::sha256(
                reinterpret_cast<const uint8_t*>(fp->compressed_fingerprint.data()),
                fp->compressed_fingerprint.size());
            fp_hash_hex = crypto::to_hex(h);
        }
        j.push_back({
            {"content_hash",     crypto::to_hex(ch)},
            {"title",            meta->title},
            {"artist",           meta->artist},
            {"genre",            meta->genre},
            {"album",            meta->album},
            {"duration_ms",      meta->duration_ms},
            {"year",             meta->year},
            {"track_number",     meta->track_number},
            {"play_count",       state.play_count},
            {"fingerprint_hash", fp_hash_hex},
            // Listener ratings ride along with every metadata row so the
            // player / website can render thumbs without a second round trip.
            {"ratings_up",       rat.up},
            {"ratings_down",     rat.down},
        });
    }
    return {200, j.dump()};
}

// get_songs_search(MHD_Connection*) removed — the HTTP/3 dispatcher in
// transport/h3_server.cpp parses ?artist/?genre/?q from the URL and calls
// the verb_songs_search_* variants below directly.

std::pair<int, std::string> HttpServer::verb_songs_search_query(const std::string& q) {
    return _do_songs_search("", "", q);
}
std::pair<int, std::string> HttpServer::verb_songs_search_artist(const std::string& a) {
    return _do_songs_search(a, "", "");
}
std::pair<int, std::string> HttpServer::verb_songs_search_genre(const std::string& g) {
    return _do_songs_search("", g, "");
}

std::pair<int, std::string> HttpServer::_do_songs_search(const std::string& artist,
                                                          const std::string& genre,
                                                          const std::string& q) {
    std::vector<Hash256> candidates;
    bool filtered = false;

    if (!artist.empty()) {
        candidates = db_.get_songs_by_artist(artist);
        filtered = true;
    } else if (!genre.empty()) {
        candidates = db_.get_songs_by_genre(genre);
        filtered = true;
    }

    if (!filtered) {
        candidates = db_.get_all_song_hashes();
    }

    std::string q_str = q;
    // Lowercase query for case-insensitive match
    for (auto& c : q_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    json j = json::array();
    for (const auto& ch : candidates) {
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album (meta->album))  continue;
        if (db_.is_hidden_title (meta->title))  continue;
        if (!q_str.empty()) {
            // Check substring match against title/artist/genre
            auto lc = [](std::string s) {
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            if (lc(meta->title).find(q_str) == std::string::npos &&
                lc(meta->artist).find(q_str) == std::string::npos &&
                lc(meta->genre).find(q_str) == std::string::npos) {
                continue;
            }
        }
        auto state = db_.get_song_state(ch);
        const auto rat = db_.get_rating_counts(ch);
        j.push_back({
            {"content_hash", crypto::to_hex(ch)},
            {"title",        meta->title},
            {"artist",       meta->artist},
            {"genre",        meta->genre},
            {"album",        meta->album},
            {"duration_ms",  meta->duration_ms},
            {"year",         meta->year},
            {"track_number", meta->track_number},
            {"play_count",   state.play_count},
            {"ratings_up",   rat.up},
            {"ratings_down", rat.down},
        });
    }
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_song(const std::string& content_hash_hex) {
    Hash256 ch;
    if (!crypto::parse_hash256(content_hash_hex, ch))
        return {400, R"({"error":"invalid hash"})"};
    auto state = db_.get_song_state(ch);
    const auto rat = db_.get_rating_counts(ch);
    json j = {
        {"content_hash", content_hash_hex},
        {"play_count", state.play_count},
        {"discoverer", crypto::to_checksum_hex(state.discoverer_address)},
        {"ratings_up",   rat.up},
        {"ratings_down", rat.down},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_balance(const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    uint64_t bal = db_.get_balance(addr);
    json j = {{"address", address_hex}, {"balance", Ledger::format_balance(bal)}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_escrow_balance(
    const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    // Escrow lives in the regular ledger under a derived address that
    // has no private key — only the moderator can release via
    // post_moderator_release. Surface both the spendable address and
    // the escrow holding address so the artist UI can show both.
    Address escrow = crypto::escrow_address_for(addr);
    uint64_t bal = db_.get_balance(escrow);
    json j = {
        {"address",        crypto::to_checksum_hex(addr)},
        {"escrow_address", crypto::to_checksum_hex(escrow)},
        {"escrow_balance", Ledger::format_balance(bal)},
    };
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::post_session_start(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string ch_hex  = j["content_hash"];
        std::string pl_hex  = j["player_address"];

        Hash256 ch;
        Address pl;
        if (!crypto::parse_hash256(ch_hex, ch)) return {400, R"({"error":"bad content_hash"})"};
        if (!crypto::parse_address_checksummed(pl_hex, pl)) return {400, R"({"error":"bad player_address"})"};

        // ---- Anti-farm: server-derived device identity (never the wallet) ----
        // The client's hardware attestation rides session.start in `attestation`.
        // AcceptAllVerifier derives a device_id (empty when the client sent none).
        // We derive it HERE (not in the transport dispatch) and ignore any
        // client-supplied id, so every transport — rats RPC and a restored H3 —
        // gets the same server-authoritative device_id.
        static AcceptAllVerifier g_verifier;
        AttestationResult att = g_verifier.verify(
            j.value("attestation", json::object()), std::string(), std::string());
        std::string dev = att.device_id;   // "" => unidentifiable client
        // Phase 3c (attribution-only): a browser has no hardware attestation, so
        // fall back to its PERSISTENT listener wallet (arrives as tracking_address)
        // as the per-device cap key. This turns the previously uncapped web-play
        // identity into a stable, per-browser-wallet capped one — closing the
        // 0x000 seeder/mini farming hole (combined with the gateway per-IP layer
        // to bound sybil). It is a CAP KEY ONLY, never a payout: the gateway keeps
        // player_address = 0x000, so the web listener still earns nothing.
        if (dev.empty()) {
            const std::string track = j.value("tracking_address", std::string());
            if (track.size() >= 8) dev = "web:" + track;
        }

        // UTC day bucket, bound ONCE here and carried on the session; clamped
        // non-decreasing per device so a backward server clock step can't reset
        // the daily counter.
        uint64_t day = now_ms_api() / 86400000ULL;
        if (!dev.empty()) {
            const uint64_t hw = db_.get_u64("ddaymax:" + dev).value_or(0);
            if (day < hw) day = hw;
        }

        // Daily coverage cap — reject at START, before any listen time is
        // invested, so a completed genuine listen is never voided at the finish
        // line. The durable counter is only incremented at complete.
        if (!dev.empty()) {
            const uint64_t cum =
                db_.get_u64("ddur:" + dev + ":" + std::to_string(day)).value_or(0);
            if (cum >= kDailyCoverageCapMs) {
                std::cout << "[antifarm] would-reject daily-cap dev="
                          << dev.substr(0, 12) << " cum_ms=" << cum
                          << (device_cap_enforce_.load() ? " ENFORCED" : " dark") << "\n";
                if (device_cap_enforce_.load())
                    return {429, R"({"error":"daily_device_limit"})"};
            }
        }

        // Past the 10k-plays cliff, the listener has to burn tokens
        // per play. The required amount is dynamic — zero until the
        // chain hits SUPPLY_FLOOR, then ramps up cubically toward
        // SUPPLY_CAP (see compute_burn_rate). Reject session.start
        // when the player can't afford the burn rather than letting
        // them stream and then bouncing them at session.complete.
        uint64_t play_count = db_.get_play_count(ch);
        if (play_count >= FULL_REWARD_THRESHOLD) {
            const uint64_t supply = db_.get_total_supply();
            const uint64_t burn   = compute_burn_rate(supply);
            if (burn > 0) {
                const uint64_t bal = db_.get_balance(pl);
                if (bal < burn) {
                    std::ostringstream err;
                    err << R"({"error":"insufficient_balance","required":")"
                        << Ledger::format_balance(burn) << R"("})";
                    return {402, err.str()};
                }
            }
        }

        // Generate session_id
        std::string session_id_hex = generate_session_id();

        PlaySession session;
        crypto::parse_hash256(session_id_hex, session.session_id);
        session.content_hash     = ch;
        session.player_address   = pl;
        // Per-stream reward lanes (optional; PlayProof v2). The player reports the
        // SEEDER (peer that served the bytes) and the MINI-NODE (relay) as 40-hex
        // peer-ids, which ARE wallet addresses (single-identity). Lenient parse
        // (no checksum); absent/invalid -> zero -> that lane is skipped in the mint.
        {
            Address sa{}, ma{};
            if (crypto::parse_address(j.value("seeder_address", std::string()), sa))
                session.seeder_address = sa;
            if (crypto::parse_address(j.value("mini_node_address", std::string()), ma))
                session.mini_node_address = ma;
        }
        // Stage 2: the delivery_id from this play's stream.open. Unlike the two
        // addresses above it is NOT a claim — it is a 128-bit random handle the
        // NODE minted and handed only to the peer that opened the stream, so
        // presenting it proves nothing more than "I am the requester", and the
        // node then reads the seeder / mini identities out of its OWN
        // triangulation row rather than out of this request. 32 lowercase hex.
        // Stage 2: the listener's compressed pubkey. Self-verifying — we only
        // keep it when it hashes to the player_address that was already parsed
        // and checksum-validated above, so it is not a trust decision. Optional
        // here; session.complete accepts it too. Without it the node cannot
        // build a co-signable proof, because the v3 preimage covers it.
        {
            auto pk = crypto::from_hex(j.value("player_pubkey", std::string()));
            if (pk.size() == 33) {
                PubKey33 candidate{};
                std::copy(pk.begin(), pk.end(), candidate.begin());
                if (crypto::address_from_pubkey(candidate) == pl)
                    session.player_pubkey = candidate;
            }
        }
        {
            const std::string did = j.value("delivery_id", std::string());
            bool ok = (did.size() == 32);
            for (char c : did)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; }
            if (ok) session.delivery_id = did;
        }
        session.start_timestamp  = now_ms_api();
        session.last_heartbeat   = session.start_timestamp;
        session.heartbeat_count  = 0;

        // Find block_hash for this song (simplified)
        session.block_hash = {};

        session.device_id  = dev;
        session.att_level  = att.level;
        session.day_bucket = day;

        // Concurrency slot keyed on (device, wallet): one wallet can't fan out
        // parallel streams from a device, but the device is NOT bricked to serial
        // (track-skips, 2 accounts on one PC). Only for identified devices; an
        // empty device_id is left to the gateway per-IP layer. Check-and-
        // increment under the SAME lock as the insert => atomic vs. RPC workers.
        std::string slot_key;
        if (!dev.empty())
            slot_key = dev + "|" + crypto::to_hex(pl.data(), pl.size());
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            if (!slot_key.empty()) {
                uint32_t cur = 0;
                auto lit = live_by_device_.find(slot_key);
                if (lit != live_by_device_.end()) cur = lit->second;
                if (cur >= kMaxConcurrentPerDevice) {
                    std::cout << "[antifarm] would-reject concurrency dev="
                              << dev.substr(0, 12) << " live=" << cur
                              << (device_cap_enforce_.load() ? " ENFORCED" : " dark") << "\n";
                    if (device_cap_enforce_.load())
                        return {429, R"({"error":"too_many_concurrent_streams"})"};
                }
                live_by_device_[slot_key] = cur + 1;
                session.slot_key  = slot_key;
                session.slot_held = true;
            }
            sessions_[session_id_hex] = session;
        }

        json resp = {
            {"session_id", session_id_hex},
            {"block_hash", crypto::to_hex(session.block_hash)},
        };
        return {200, resp.dump()};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::post_session_heartbeat(
    const std::string& session_id, const std::string& body) {
    std::lock_guard<std::mutex> lk(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
    // BUG FIX: previously the heartbeat handler appended samples
    // even AFTER session.complete had landed for the session. The
    // beats never got used (complete already captured the sample
    // vector) but they bloated the in-memory PlaySession until
    // session expiry. Rejecting up front matches the player flow
    // (HeartbeatService.stop() fires on complete).
    if (it->second.completed)
        return {400, R"({"error":"session already completed"})"};

    try {
        const auto j = json::parse(body);
        const uint64_t now = now_ms_api();
        // BUG FIX: cap the per-session heartbeat rate so a malicious
        // client can't flood thousands of beats per second to inflate
        // the density gate at session.complete time. The legitimate
        // 5 s cadence sits well above kMinIntervalMs.
        constexpr uint64_t kMinIntervalMs = 1000;
        if (!it->second.samples.empty()) {
            const uint64_t last_wall = it->second.samples.back().wall_ms;
            if (now < last_wall + kMinIntervalMs)
                return {429, R"({"error":"heartbeat rate too high"})"};
        }
        // position_ms is the player's claim of "where in the song I
        // am". Missing or non-integer → 400; otherwise sanity check
        // against the song's declared duration if known. Defense in
        // depth — the aggregation loop also clamps by duration, but
        // rejecting at receive time keeps the in-memory sample
        // vector small and surfaces the bad client to the caller.
        constexpr uint64_t kPositionSlackMs = 5000;
        if (!j.contains("position_ms"))
            return {400, R"({"error":"missing position_ms"})"};
        const uint64_t pos_ms = j.value("position_ms",
                                         static_cast<uint64_t>(0));
        const auto height_opt = db_.get_content_height(it->second.content_hash);
        if (height_opt) {
            // BUG FIX: position_ms used to be accepted without bound,
            // so a single beat could carry position_ms = 999999999 and
            // (combined with kPlaybackGraceMs) push effective_ms past
            // any threshold. Now we clamp on receipt instead of relying
            // solely on the song_duration_hint clamp at aggregation time.
            //
            // Read the duration from the CANONICAL LevelDB block store, not
            // the best-effort .blk dump: that dump is bucketed by the producer
            // path but read flat here, so a locally-produced song's file was
            // missed and the clamp silently never fired (see
            // post_session_complete for the full write/read path mismatch).
            uint64_t dur = 0;
            if (auto bhash = chain_.get_block_hash(*height_opt)) {
                if (auto blk = chain_.get_block(*bhash); blk && blk->has_song)
                    dur = blk->song.duration_ms;
            }
            if (dur > 0 && pos_ms > dur + kPositionSlackMs)
                return {400, R"({"error":"position_ms past end of song"})"};
        }
        it->second.last_heartbeat = now;
        it->second.heartbeat_count++;
        it->second.samples.push_back({now, pos_ms});
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid body"})"};
    }
}

std::pair<int, std::string> HttpServer::post_session_complete(
    const std::string& session_id, const std::string& body) {
    // BUG FIX: previously we set `it->second.completed = true` here
    // before applying the mint. When the mint failed (any gate
    // rejected, apply_mint returned false, db.write failed), the
    // session was already marked completed and could never be
    // retried — including for transient errors. Now we only flip
    // the flag AFTER mint actually lands; rejected attempts can be
    // retried by the player on next playback once whatever was
    // wrong is fixed.
    PlaySession sess;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
        if (it->second.completed) return {400, R"({"error":"already completed"})"};
        // Stage 2: a session that already handed out a preimage must finish
        // through session.cosign. Re-completing would build a SECOND proof for
        // the same session_id (different play_end_timestamp, so a different
        // preimage) and leave the first one signable in parallel.
        if (it->second.awaiting_cosign)
            return {409, R"({"error":"awaiting co-signature","detail":"call session.cosign with the preimage already issued"})"};
        // Atomically CLAIM the session so two concurrent completes for the same
        // session_id can't both mint + both increment the daily counter (the
        // `completed` flag alone is only set after the mint, and apply_mint does
        // not re-check is_session_used).
        if (it->second.completing) return {409, R"({"error":"completion in progress"})"};
        it->second.completing = true;
        sess = it->second;
    }
    // Capture session_id so the success path can flip the flag without
    // re-resolving the iterator (the map could have been mutated in
    // the meantime — e.g., session expiry from a later patch).
    const std::string sid_copy = session_id;

    // Free the device concurrency slot exactly once when this complete returns,
    // whichever gate/branch exits. release_device_slot_locked is idempotent
    // (slot_held), so a retried complete after a gate-reject can't double-
    // decrement; the already-completed early return above returns BEFORE this
    // guard is built, leaving the first complete's release intact.
    ScopeExit slot_guard{[this, sid_copy] {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(sid_copy);
        if (it != sessions_.end()) {
            release_device_slot_locked(it->second);
            // Release the completion claim so a legit retry can proceed after a
            // gate-reject / error. On success `completed` is already true (set
            // before this guard runs) and the entry check rejects retries first.
            it->second.completing = false;
        }
    }};

    uint64_t now = now_ms_api();
    uint64_t duration_ms = now - sess.start_timestamp;

    // Reject replayed sessions before bothering with the listen-time math.
    if (db_.is_session_used(sess.session_id))
        return {400, R"({"error":"session already used"})"};

    // Load the SongSection (need artist_address + royalty_splits, and
    // duration_ms for the 50% listen threshold below) from the CANONICAL
    // block store in LevelDB — the same source /api/block uses.
    //
    // BUG FIX: this used to read the best-effort <data_dir>/blocks/<h>.blk
    // dump directly. But the producer path writes those bucketed
    // (blocks/<h/1000>/<h>.blk, candidate.cpp) while the sync path writes them
    // flat and this reader looked flat — so any locally-produced song's file
    // was MISSED. song_section then stayed default-constructed, which:
    //   * zeroed artist_address  -> the artist's reward routed to the
    //     unclaimed-escrow zero address instead of the artist, and
    //   * left duration_ms as stack garbage (~984 h) -> the 50% coverage
    //     gate could never be met, so real plays never minted.
    // chain_.get_block reads "b:"+hash from LevelDB (durable, path-independent)
    // and always resolves the true song section.
    SongSection song_section;
    auto height_opt = db_.get_content_height(sess.content_hash);
    if (height_opt) {
        if (auto bhash = chain_.get_block_hash(*height_opt)) {
            if (auto blk = chain_.get_block(*bhash); blk && blk->has_song)
                song_section = blk->song;
        }
    }

    // effective_ms = union of timestamp ranges within the song that the
    // listener actually played. Re-listening to the same chorus three
    // times counts once; skipping forward to a new section adds that
    // section's length. The threshold below compares this against the
    // song's duration so "completed enough of the song" is robust to
    // seeking around without farming play credit by replaying a 5 s clip
    // until the wall clock hits 30 s.
    //
    // Wall-time → song-time projection: between two consecutive
    // heartbeats the listener is presumed to have played [a.position_ms,
    // a.position_ms + min(wall_dt + grace, song_duration_ms)]. For a
    // monotonic forward-playing listener that equals their next reported
    // position. For paused / buffering windows it's bounded by wall_dt
    // so a long pause doesn't synthesize coverage. Backward seeks
    // produce a new range starting at the new position rather than
    // continuing the old one.
    // BUG FIX: kPlaybackGraceMs used to be 2000 — 100 samples
    // synthesised 200 s of listening that didn't happen. 500 ms is
    // generous enough to cover RPC / scheduling jitter on a 5 s
    // cadence without padding accumulated ranges.
    constexpr uint64_t kPlaybackGraceMs   = 500;
    // BUG FIX: cap per-sample advance at twice the expected
    // HeartbeatService cadence (10 s). The previous code only
    // capped on the next sample's position delta or song duration;
    // for the LAST sample (no next sample) wall_dt = now -
    // last_heartbeat, which could be 30+ s if the player paused
    // before completing — that pause time was credited as listening
    // time. Capping at 2× cadence kills the rubber-band.
    constexpr uint64_t kMaxAdvancePerSampleMs = 10000;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    const auto& samples = sess.samples;
    const uint64_t song_duration_ms_hint = song_section.duration_ms;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& a = samples[i];
        const uint64_t next_wall = (i + 1 < samples.size())
            ? samples[i + 1].wall_ms : now;
        // BUG FIX: previously `continue` on out-of-order — that
        // silently dropped the sample without recording even its
        // own position point. Instead clamp the slice to zero
        // advance so the sample still contributes its (a.position_ms,
        // a.position_ms+0) range, which the union code below treats
        // as a no-op but at least doesn't fall behind on counts.
        uint64_t wall_dt = next_wall >= a.wall_ms
            ? next_wall - a.wall_ms : 0;
        uint64_t advance = std::min<uint64_t>(
            wall_dt + kPlaybackGraceMs, kMaxAdvancePerSampleMs);
        // If we have a next heartbeat and it reports a higher
        // position for the same wall slice, that's the listener's
        // authoritative claim — bound advance by it. (Seeking
        // forward DURING the slice is still bounded by wall_dt+grace
        // and per-sample cap above, preventing skip-farming.)
        if (i + 1 < samples.size()) {
            const auto& b = samples[i + 1];
            if (b.position_ms > a.position_ms) {
                advance = std::min(advance, b.position_ms - a.position_ms);
            } else {
                // Seek back / no progress — this slice contributes
                // nothing new beyond the single-point position
                // itself.
                advance = 0;
            }
        }
        if (song_duration_ms_hint > 0 && a.position_ms < song_duration_ms_hint) {
            advance = std::min<uint64_t>(advance,
                song_duration_ms_hint - a.position_ms);
        }
        if (advance == 0) continue;
        ranges.emplace_back(a.position_ms, a.position_ms + advance);
    }
    // Sort + merge so re-listened ranges collapse to their union.
    std::sort(ranges.begin(), ranges.end());
    uint64_t effective_ms = 0;
    uint64_t cur_start = 0, cur_end = 0;
    bool have_cur = false;
    for (const auto& r : ranges) {
        if (!have_cur) {
            cur_start = r.first; cur_end = r.second; have_cur = true;
            continue;
        }
        if (r.first <= cur_end) {
            if (r.second > cur_end) cur_end = r.second;
        } else {
            effective_ms += cur_end - cur_start;
            cur_start = r.first; cur_end = r.second;
        }
    }
    if (have_cur) effective_ms += cur_end - cur_start;

    // Two gates the play has to clear before the chain mints. All
    // must hold; any failure returns 400 and logs a
    // [session.complete] REJECT line with the failing reason.
    //
    //  (1) Enough timestamps. The HeartbeatService ticks every 5 s;
    //      we accept down to one beat per 10 s of wall time AND a
    //      hard floor of 6 beats overall. A "press play, immediately
    //      press complete" loop with one or two beats can't claim a
    //      play.
    //
    //  (2) Timestamps cover the song. The set of reported position_ms
    //      values has to span enough of the song to make the claim
    //      credible. When the chain knows the song's duration we
    //      require positions covering ≥ 50 % of it (so a 5 s loop
    //      replayed for the wall-clock equivalent of the song still
    //      fails — same-range union collapses identical ranges). When
    //      the song isn't registered yet (duration_ms = 0) we fall
    //      back to ≥ 30 s of distinct content; the next block lands
    //      the duration and subsequent plays use the 50 % path.
    //
    // Heartbeats are allowed to be perfectly periodic — the
    // position_ms span gate is what makes a real listen
    // indistinguishable from a "looks human" play.
    constexpr uint64_t kPlayPercentRequired = 50;
    constexpr uint64_t kLegacyMinListenMs   = 30000;
    constexpr uint64_t kMinHeartbeats       = 6;
    constexpr uint64_t kMaxMsPerHeartbeat   = 10000;

    auto reject = [&](const std::string& err_json,
                      const std::string& reason) {
        std::cout << "[session.complete] REJECT sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12)
                  << " reason=" << reason
                  << " eff_ms=" << effective_ms
                  << " song_dur_ms=" << song_section.duration_ms
                  << " heartbeats=" << samples.size()
                  << " wall_ms=" << duration_ms << "\n";
        return std::pair<int, std::string>{400, err_json};
    };

    // ---- gate 1: enough timestamps ----------------------------------
    {
        const uint64_t density_min =
            std::max<uint64_t>(kMinHeartbeats,
                               duration_ms / kMaxMsPerHeartbeat);
        if (samples.size() < density_min) {
            std::ostringstream err;
            err << R"({"error":"sparse heartbeats","heartbeats":)"
                << samples.size()
                << R"(,"required_heartbeats":)" << density_min
                << R"(,"wall_duration_ms":)" << duration_ms << "}";
            return reject(err.str(), "sparse_heartbeats");
        }
    }

    // ---- gate 2: timestamps cover the song --------------------------
    {
        // Guard against a garbage on-chain duration. A buggy container once
        // registered 662646176 ms (= 184 h), and 50% of that can never be
        // listened, so the song became permanently un-rewardable. A real music
        // track is < 6 h; if the stored duration is implausible, fall back to
        // the fixed legacy listen threshold instead of a percentage of a bogus
        // value — so songs already registered with a bad duration still reward
        // on a genuine play.
        constexpr uint64_t kMaxSaneDurationMs = 6ull * 60 * 60 * 1000;
        const bool dur_sane = song_section.duration_ms > 0 &&
            uint64_t{song_section.duration_ms} < kMaxSaneDurationMs;
        const uint64_t required_ms = dur_sane
            ? (uint64_t{song_section.duration_ms}
                * kPlayPercentRequired / 100)
            : kLegacyMinListenMs;
        if (effective_ms < required_ms) {
            std::ostringstream err;
            err << R"({"error":"position_ms timestamps don't cover the song","effective_listened_ms":)"
                << effective_ms
                << R"(,"required_ms":)" << required_ms
                << R"(,"song_duration_ms":)" << song_section.duration_ms
                << R"(,"required_percent":)" << kPlayPercentRequired
                << R"(,"wall_duration_ms":)" << duration_ms << "}";
            return reject(err.str(), "below_threshold");
        }
    }

    // Build PlayProof
    PlayProof proof;
    proof.session_id           = sess.session_id;
    proof.content_hash         = sess.content_hash;
    proof.block_hash           = sess.block_hash;
    proof.artist_address       = song_section.artist_address;
    proof.player_address       = sess.player_address;
    proof.serving_node_id      = config_.node_id;
    proof.play_start_timestamp = sess.start_timestamp;
    proof.play_end_timestamp   = now;
    proof.total_duration_ms    = (duration_ms > 0xFFFFFFFFu)
        ? 0xFFFFFFFFu : static_cast<uint32_t>(duration_ms);
    proof.heartbeat_count      = (sess.heartbeat_count > 0xFFFFu)
        ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(sess.heartbeat_count);

    // ---- Per-stream reward lanes (PlayProof v2/v3), NODE-AUTHORITATIVE ------
    //
    // These two addresses are MONEY: each non-zero lane mints a whole token to
    // whoever it names. They used to be read straight out of this request body
    // (and, before that, out of session.start), so a client that named its own
    // wallet as `seeder_address` simply stole the seeder lane and a client that
    // named a friend's mini-node stole the relay lane. Nothing verified that
    // the named peer had served a single byte.
    //
    // Stage 2 resolves both from the node's OWN delivery-triangulation record
    // instead (see resolve_delivery_lanes): the broker minted the delivery_id
    // at stream.open and recorded the candidate holder set; the MINI-NODE
    // signed a relay.report naming the peer it actually received the audio
    // frames from; the peer→wallet mapping comes from the wallet-signed
    // presence.hello binding. The request body is no longer consulted at all.
    //
    // If that resolution fails for any reason — no delivery_id threaded
    // through, the mini never reported, the named peer has no signed presence
    // binding, the wallet isn't a holder of this song — BOTH lanes stay zero.
    // An unpaid lane is always the right failure mode; paying the wrong wallet
    // is not recoverable.
    Address  seeder_addr{};
    Address  mini_addr{};
    PubKey33 mini_pk{};
    {
        std::string why;
        if (!resolve_delivery_lanes(sess, seeder_addr, mini_addr, mini_pk, why)) {
            seeder_addr = Address{};
            mini_addr   = Address{};
            mini_pk     = PubKey33{};
            std::cout << "[session.complete] lanes unresolved sid="
                      << crypto::to_hex(sess.session_id).substr(0, 12)
                      << " reason=" << why << " (seeder+relay unpaid)\n";
        }
    }
    proof.seeder_address    = seeder_addr;
    proof.mini_node_address = mini_addr;
    proof.version           = 3;

    // ---- Fix EVERY identity before ANY signature ---------------------------
    //
    // PlayProof::sign_message() for v3 covers all three co-signer pubkeys
    // (serving_node, player, mini_node) as well as the play facts. That is
    // deliberate — it is what stops one party's signature being grafted onto a
    // proof naming somebody else — but it means the preimage does not exist
    // until all three identities are known, so they must be resolved here,
    // before the node signs:
    //
    //   serving_node_pubkey — ours; also lets any validator check the node
    //       signature without the founder v: registry, since
    //       serving_node_id == sha256(serving_node_pubkey).
    //   player_pubkey       — supplied by the client and accepted only if it
    //       hashes to the player_address we already hold (self-verifying).
    //   mini_node_pubkey    — from the mini's own wallet-signed relay.report,
    //       via the delivery row. Never client input.
    //
    // A relay lane is claimed ONLY when we have the matching pubkey: an
    // address we cannot bind to a key could never be co-signed, and post
    // activation a lane without its signature is a hard reject. Dropping the
    // lane costs one token; claiming an unsignable one would cost the mint.
    //
    // A pubkey is only ever written when its signature is going to follow.
    // check_play verifies a co-signature the moment its pubkey is non-zero, so
    // publishing a pubkey with a zero signature produces a proof that FAILS
    // consensus — i.e. a play that silently never mints. On the legacy
    // (un-co-signed) path both slots therefore stay zero, exactly as today.
    bool wants_cosign = false;
    try {
        auto jb = json::parse(body);
        wants_cosign = jb.value("cosign", false);
    } catch (...) { /* no body / not JSON -> legacy client */ }

    proof.serving_node_pubkey = node_keypair_.public_key;
    if (wants_cosign) {
        PubKey33 listener_pk = sess.player_pubkey;
        try {
            auto jb = json::parse(body);
            auto pk = crypto::from_hex(jb.value("player_pubkey", std::string()));
            if (pk.size() == 33) {
                PubKey33 candidate{};
                std::copy(pk.begin(), pk.end(), candidate.begin());
                if (crypto::address_from_pubkey(candidate) == proof.player_address)
                    listener_pk = candidate;
            }
        } catch (...) { /* no body / not JSON */ }
        if (listener_pk != PubKey33{} &&
            crypto::address_from_pubkey(listener_pk) == proof.player_address)
            proof.player_pubkey = listener_pk;

        if (mini_pk != PubKey33{} &&
            crypto::address_from_pubkey(mini_pk) == proof.mini_node_address) {
            proof.mini_node_pubkey = mini_pk;
        } else if (proof.mini_node_address != Address{}) {
            std::cout << "[session.complete] relay lane dropped sid="
                      << crypto::to_hex(sess.session_id).substr(0, 12)
                      << " (no verified mini-node pubkey for "
                      << crypto::to_checksum_hex(proof.mini_node_address) << ")\n";
            proof.mini_node_address = Address{};
        }
    }

    // Node signs the proof. This is the FIRST of the three signatures over the
    // identical PlayProof::sign_message() preimage.
    auto sign_msg = proof.sign_message();
    Hash256 sh    = crypto::sha256(sign_msg.data(), sign_msg.size());
    proof.node_signature = crypto::sign_ecdsa(sh, node_keypair_.private_key);

    // ---- Stage 2: hand the preimage back for co-signature ------------------
    //
    // Why the flow is shaped this way. play_end_timestamp, total_duration_ms
    // and heartbeat_count are node-authoritative and only exist NOW, so the
    // preimage cannot be signed at session.start; somebody has to see the
    // assembled proof before it is final. The alternative — letting the client
    // declare those three fields up front so it could pre-sign — would hand a
    // client control over the exact numbers the anti-farm gates are computed
    // from, which is strictly worse than one extra round trip.
    //
    // So: the node returns the preimage, the listener signs it, and the
    // listener also carries it one hop to the mini-node it is already relaying
    // through and brings back that signature too. Cost on the play path is
    // ZERO — the song has already finished playing by the time this runs; the
    // only thing waiting on it is the mint. The listener is a safe courier for
    // the mini's signature because it cannot forge, retarget or replay it: the
    // mini_node_address is chosen by the NODE (above) and is inside the signed
    // bytes, as is the session_id.
    //
    // Backwards compatibility: a client that does not send "cosign": true is
    // an un-upgraded player, and gets exactly the old behaviour — mint now,
    // co-signature slots left zero. That is what keeps the live chain minting
    // while players roll out. BOPWIRE_REQUIRE_COSIGN=1 lets an operator turn
    // that tolerance off on THIS node without touching consensus.
    if (!wants_cosign && require_cosign_.load()) {
        std::cout << "[session.complete] REJECT sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12)
                  << " reason=cosign_required (BOPWIRE_REQUIRE_COSIGN=1)\n";
        return {426, R"({"error":"cosign_required","detail":"this node only mints three-party co-signed play proofs; update your player"})"};
    }

    if (wants_cosign && proof.player_pubkey == PubKey33{}) {
        return {400, R"({"error":"player_pubkey required","detail":"send the listener's 33-byte compressed pubkey (hex) at session.start or in this body; the v3 preimage covers it, so it must be fixed before anything is signed"})"};
    }

    if (wants_cosign) {
        // Stash the assembled + node-signed proof and wait for session.cosign.
        // `completing` deliberately stays true so a second session.complete
        // can't build a SECOND proof for the same session; `completed` stays
        // false so the reaper still expires an abandoned session normally.
        const uint64_t deadline = now + kCosignDeadlineMs;
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            auto it = sessions_.find(sid_copy);
            if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
            it->second.awaiting_cosign      = true;
            it->second.pending_proof        = proof;
            it->second.pending_sign_hash    = sh;
            it->second.pending_song         = song_section;
            it->second.pending_effective_ms = effective_ms;
            it->second.pending_deadline_ms  = deadline;
        }
        // The device concurrency slot is released by slot_guard on return (the
        // play is over); the completion claim is NOT, so the pending proof is
        // the only thing that can still mint this session.
        json resp = {
            {"status",       "awaiting_cosign"},
            {"session_id",   crypto::to_hex(sess.session_id)},
            // sha256 of the preimage — this is the 32 bytes each party's ECDSA
            // signs. Clients that build the preimage themselves can and should
            // cross-check it against this value before signing.
            {"sign_hash",    crypto::to_hex(sh)},
            // The full preimage, hex. Handed over verbatim so a client can
            // parse the node-authoritative fields out of it rather than
            // trusting the JSON mirror below, and so the mini-node can rebuild
            // and re-derive it independently.
            {"preimage",     crypto::to_hex(sign_msg.data(), sign_msg.size())},
            {"proof_version", 3},
            {"deadline_ms",  deadline},
            // Structured mirror of every signed field, so the mini-node can
            // reconstruct the preimage with PlayProof::sign_message() instead
            // of blind-signing opaque bytes handed to it by a peer.
            {"proof", {
                {"session_id",           crypto::to_hex(proof.session_id)},
                {"content_hash",         crypto::to_hex(proof.content_hash)},
                {"block_hash",           crypto::to_hex(proof.block_hash)},
                {"artist_address",       crypto::to_hex(proof.artist_address.data(), 20)},
                {"player_address",       crypto::to_hex(proof.player_address.data(), 20)},
                {"serving_node_id",      crypto::to_hex(proof.serving_node_id)},
                {"play_start_timestamp", proof.play_start_timestamp},
                {"play_end_timestamp",   proof.play_end_timestamp},
                {"total_duration_ms",    proof.total_duration_ms},
                {"heartbeat_count",      proof.heartbeat_count},
                {"seeder_address",       crypto::to_hex(proof.seeder_address.data(), 20)},
                {"mini_node_address",    crypto::to_hex(proof.mini_node_address.data(), 20)},
                {"serving_node_pubkey",  crypto::to_hex(proof.serving_node_pubkey.data(), 33)},
                {"player_pubkey",        crypto::to_hex(proof.player_pubkey.data(), 33)},
                {"mini_node_pubkey",     crypto::to_hex(proof.mini_node_pubkey.data(), 33)},
            }},
            {"delivery_id", sess.delivery_id},
        };
        std::cout << "[session.complete] AWAIT-COSIGN sid="
                  << crypto::to_hex(sess.session_id).substr(0, 12)
                  << " player=" << crypto::to_checksum_hex(proof.player_address)
                  << " seeder=" << crypto::to_checksum_hex(proof.seeder_address)
                  << " mini="   << crypto::to_checksum_hex(proof.mini_node_address)
                  << "\n";
        return {202, resp.dump()};
    }

    // Legacy single-party path — unchanged behaviour for un-upgraded players.
    std::cout << "[session.complete] UNSIGNED-PROOF sid="
              << crypto::to_hex(sess.session_id).substr(0, 12)
              << " player=" << crypto::to_checksum_hex(proof.player_address)
              << " (listener did not co-sign; a serving node alone attests this "
                 "play — see COSIGN_ACTIVATION_HEIGHT)\n";
    auto result = emit_mint(sess, proof, song_section, effective_ms);
    if (result.first == 200) {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(sid_copy);
        if (it != sessions_.end()) it->second.completed = true;
    }
    return result;
}

// ---- Stage 2: session.cosign ----------------------------------------
//
// Second and final leg of the co-signed completion. The listener returns its
// own signature over the preimage session.complete handed back, and (when the
// node resolved a paid relay lane) the mini-node's signature too, which the
// listener collected over its existing one-hop relay connection.
//
// Request body:
//   {
//     "session_id":          "<64 hex>",          // also in the path/verb arg
//     "player_pubkey":       "<66 hex>",          // 33-byte compressed secp256k1
//     "player_signature":    "<128 hex>",         // 64-byte compact ECDSA
//     "mini_node_pubkey":    "<66 hex>",          // omit when no relay lane
//     "mini_node_signature": "<128 hex>"          // omit when no relay lane
//   }
//
// Every signature is ECDSA-secp256k1 over sha256(PlayProof::sign_message()) —
// i.e. over the SAME 32 bytes returned as `sign_hash`, with no extra hashing
// and no message prefix. Identical construction to node_signature.
std::pair<int, std::string> HttpServer::post_session_cosign(
    const std::string& session_id, const std::string& body) {
    PlaySession sess;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return {404, R"({"error":"session not found"})"};
        if (it->second.completed)  return {400, R"({"error":"already completed"})"};
        if (!it->second.awaiting_cosign)
            return {400, R"({"error":"session is not awaiting co-signature"})"};
        sess = it->second;
    }
    if (now_ms_api() > sess.pending_deadline_ms)
        return {408, R"({"error":"cosign deadline expired"})"};

    PlayProof proof = sess.pending_proof;
    const Hash256& sh = sess.pending_sign_hash;

    // Parse the two signature pairs out of the body.
    PubKey33 player_pk{}, mini_pk{};
    Sig64    player_sig{}, mini_sig{};
    bool     have_mini = false;
    try {
        auto jb = json::parse(body);
        auto pk = crypto::from_hex(jb.value("player_pubkey", std::string()));
        auto sg = crypto::from_hex(jb.value("player_signature", std::string()));
        if (pk.size() != 33) return {400, R"({"error":"player_pubkey must be 33 bytes hex"})"};
        if (sg.size() != 64) return {400, R"({"error":"player_signature must be 64 bytes hex"})"};
        std::copy(pk.begin(), pk.end(), player_pk.begin());
        std::copy(sg.begin(), sg.end(), player_sig.begin());

        const std::string mpk_hex = jb.value("mini_node_pubkey", std::string());
        const std::string msg_hex = jb.value("mini_node_signature", std::string());
        if (!mpk_hex.empty() || !msg_hex.empty()) {
            auto mpk = crypto::from_hex(mpk_hex);
            auto msg = crypto::from_hex(msg_hex);
            if (mpk.size() != 33) return {400, R"({"error":"mini_node_pubkey must be 33 bytes hex"})"};
            if (msg.size() != 64) return {400, R"({"error":"mini_node_signature must be 64 bytes hex"})"};
            std::copy(mpk.begin(), mpk.end(), mini_pk.begin());
            std::copy(msg.begin(), msg.end(), mini_sig.begin());
            have_mini = true;
        }
    } catch (...) {
        return {400, R"({"error":"invalid body"})"};
    }

    // ---- Listener leg. The pubkey must hash to the player_address the NODE
    // put in the proof (the client cannot substitute a different earner), and
    // the signature must verify over the preimage the NODE built.
    if (crypto::address_from_pubkey(player_pk) != proof.player_address)
        return {403, R"({"error":"player_pubkey does not match player_address"})"};
    if (player_pk != proof.player_pubkey)
        return {403, R"({"error":"player_pubkey differs from the one committed in the preimage"})"};
    if (!crypto::verify_ecdsa(sh, player_sig, player_pk))
        return {403, R"({"error":"invalid listener signature"})"};

    // ---- Relay leg. Only accepted when the node itself resolved a paid relay
    // lane, and only from the exact mini-node it resolved: mini_node_address is
    // node-chosen, is inside the signed bytes, and is re-derived from the
    // pubkey here. A client cannot point this lane at a wallet of its choosing.
    const Address zero_addr{};
    if (proof.mini_node_address != zero_addr) {
        if (!have_mini)
            return {428, R"({"error":"mini_node_signature required","detail":"this proof claims a paid relay lane; ask the relaying mini-node to co-sign via proof.cosign"})"};
        if (crypto::address_from_pubkey(mini_pk) != proof.mini_node_address)
            return {403, R"({"error":"mini_node_pubkey does not match the resolved mini_node_address"})"};
        if (mini_pk != proof.mini_node_pubkey)
            return {403, R"({"error":"mini_node_pubkey differs from the one committed in the preimage"})"};
        if (!crypto::verify_ecdsa(sh, mini_sig, mini_pk))
            return {403, R"({"error":"invalid mini-node signature"})"};
    } else if (have_mini) {
        // No relay lane was resolved, so a mini signature is meaningless here
        // and accepting it would put a pubkey in the proof whose address does
        // not match the (zero) mini_node_address — which check_play rejects.
        return {400, R"({"error":"no relay lane resolved for this session; do not send a mini-node signature"})"};
    }

    // Only the two SIGNATURE slots are filled here. The pubkeys are already in
    // the proof — the v3 preimage covers them, so they were fixed before the
    // node signed and cannot be changed now without invalidating every
    // signature including the node's own. What the checks above therefore
    // establish is that the submitted keys MATCH the ones already committed to.
    proof.player_signature = player_sig;
    if (have_mini) proof.mini_node_signature = mini_sig;
    {
        // Belt and braces: the preimage must be bit-identical to the one whose
        // digest we handed out. If a future edit to sign_message() ever starts
        // covering the signature fields, this catches it here instead of
        // shipping a proof that consensus silently drops.
        auto check_msg = proof.sign_message();
        Hash256 check_h = crypto::sha256(check_msg.data(), check_msg.size());
        if (std::memcmp(check_h.data(), sh.data(), 32) != 0) {
            std::cout << "[session.cosign] INTERNAL preimage drift sid="
                      << crypto::to_hex(sess.session_id).substr(0, 12) << "\n";
            return {500, R"({"error":"internal preimage mismatch"})"};
        }
    }

    // Local consensus preflight against the height this would land at, so a
    // proof that could never mint is refused here with a reason instead of
    // being flooded and silently dropped.
    {
        std::string err;
        if (!check_play(proof, db_, chain_.tip().height + 1, err)) {
            std::cout << "[session.cosign] REJECT sid="
                      << crypto::to_hex(sess.session_id).substr(0, 12)
                      << " reason=" << err << "\n";
            return {400, std::string(R"({"error":"proof rejected","detail":")") + err + "\"}"};
        }
    }

    auto result = emit_mint(sess, proof, sess.pending_song,
                            sess.pending_effective_ms);
    if (result.first == 200) {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.completed       = true;
            it->second.completing      = false;
            it->second.awaiting_cosign = false;
            release_device_slot_locked(it->second);
        }
    }
    std::cout << "[session.cosign] OK sid="
              << crypto::to_hex(sess.session_id).substr(0, 12)
              << " listener=SIGNED"
              << " mini=" << (have_mini ? "SIGNED" : "n/a") << "\n";
    return result;
}

// ---- Shared mint emission -------------------------------------------
//
// Everything from "the proof is final" to "the reply is built", shared by the
// legacy single-party completion and the co-signed one so the two paths can
// never drift in what they mint.
std::pair<int, std::string> HttpServer::emit_mint(const PlaySession& sess,
                                                  const PlayProof& proof,
                                                  const SongSection& song_section,
                                                  uint64_t effective_ms) {
    const uint64_t now = now_ms_api();

    // Compute mint outputs
    uint64_t play_count = db_.get_play_count(sess.content_hash);
    Address  node_addr  = node_keypair_.address;
    auto outputs = compute_mint_outputs(proof, song_section, play_count,
                                        config_.node_id, node_addr);

    // Burn rate scales with total minted supply; zero until the chain
    // reaches SUPPLY_FLOOR, growing cubically to "hyperdrive" near
    // SUPPLY_CAP. Below the 10k-plays threshold there's no burn at all
    // (the listener is in discovery tier).
    MintTx mint;
    mint.proof       = proof;
    mint.outputs     = outputs;
    mint.burn_amount = (play_count >= FULL_REWARD_THRESHOLD)
        ? compute_burn_rate(db_.get_total_supply())
        : 0;

    // Phase 1: publish the play reward as an ON-CHAIN MINT tx (flood + mempool)
    // rather than a direct local write. The producer mines it and EVERY node
    // applies it through the block-apply forge gate (validate_mint), so the
    // credit replicates and survives resync — the old direct apply_mint credited
    // only THIS node's LevelDB, which is exactly why balances weren't reaching
    // players across the mesh. The node-local per-device coverage counter
    // (ddur:/ddaymax:) is NOT consensus, so it goes to its OWN batch and never
    // rides a consensus write. Replay is handled downstream: the MintTx is
    // content-addressed (tx_hash dedups a re-submitted session in the mempool)
    // and apply_mint writes the "u:"+session_id marker at block-apply.
    {
        std::unique_lock<std::mutex> dev_lk;
        if (!sess.device_id.empty())
            dev_lk = std::unique_lock<std::mutex>(device_shard(sess.device_id));

        if (!sess.device_id.empty()) {
            leveldb::WriteBatch local;
            const std::string dk =
                "ddur:" + sess.device_id + ":" + std::to_string(sess.day_bucket);
            const uint64_t cum = db_.get_u64(dk).value_or(0);
            db_.put_batch_u64(local, dk, cum + effective_ms);
            if (db_.get_u64("ddaymax:" + sess.device_id).value_or(0) < sess.day_bucket)
                db_.put_batch_u64(local, "ddaymax:" + sess.device_id, sess.day_bucket);
            db_.write(local);
        }

        if (batch_settle_enabled_.load()) {
            // Phase 3: accrue the signed PlayProof to the node-local epoch bucket
            // (NON-consensus scratch, plain put). The reaper closes the epoch
            // into one SETTLEMENT_MINT that credits every constituent at once.
            const uint64_t epoch = now / EPOCH_MS;
            const std::string akey = "accplay:" + std::to_string(epoch) + ":"
                                   + crypto::to_hex(sess.session_id);
            db_.put(akey, proof.serialize());
        } else if (ingest_tx_cb_) {
            Transaction txw = Transaction::from_mint(mint);
            const std::string tx_hex = crypto::to_hex(txw.raw);
            const std::string env =
                "{\"tx\":\"" + tx_hex + "\",\"submit_ms\":" + std::to_string(now) + "}";
            if (!ingest_tx_cb_(env)) {
                // Duplicate (already in mempool) is idempotent + fine; a genuine
                // reject means the mint failed its own forge gate (should never
                // happen for a proof we just built + signed) — log, don't 500.
                std::cout << "[session.complete] mint not newly queued sid="
                          << crypto::to_hex(sess.session_id).substr(0, 12) << "\n";
            }
        } else {
            std::cout << "[session.complete] NO-INGEST-CB sid="
                      << crypto::to_hex(sess.session_id).substr(0, 12) << "\n";
        }
    }
    // Retire the delivery-resolution row so a single relayed stream can only
    // ever fund ONE play's seeder + relay lanes, even if the same delivery_id
    // is threaded through a second session.
    if (!sess.delivery_id.empty())
        consume_delivery_row(sess.delivery_id);

    std::cout << "[session.complete] OK sid="
              << crypto::to_hex(sess.session_id).substr(0, 12)
              << " player=" << crypto::to_checksum_hex(sess.player_address)
              << " artist=" << crypto::to_checksum_hex(song_section.artist_address)
              << " seeder=" << crypto::to_checksum_hex(proof.seeder_address)
              << " mini="   << crypto::to_checksum_hex(proof.mini_node_address)
              << " play_count=" << (play_count + 1)
              << " outputs=" << outputs.size()
              << " eff_ms=" << effective_ms
              << " heartbeats=" << sess.samples.size() << "\n";

    // Tally response amounts
    // Attribute each output to the lane it belongs to. Below the 10k-play
    // threshold the artist share lands in escrow_address_for(artist), not the
    // artist itself; it used to fall through into discoverer_amount and made
    // the reply claim the listener had earned the artist's tokens.
    uint64_t artist_amount = 0, node_amount = 0, discoverer_amount = 0;
    uint64_t seeder_amount = 0, mini_amount = 0, escrow_amount = 0;
    for (const auto& out : outputs) {
        if (out.recipient == song_section.artist_address)   artist_amount     += out.amount;
        else if (out.recipient == node_addr)                node_amount       += out.amount;
        else if (proof.seeder_address != Address{} &&
                 out.recipient == proof.seeder_address)     seeder_amount     += out.amount;
        else if (proof.mini_node_address != Address{} &&
                 out.recipient == proof.mini_node_address)  mini_amount       += out.amount;
        else if (out.recipient == proof.player_address)     discoverer_amount += out.amount;
        else                                                escrow_amount     += out.amount;
    }

    SongState new_state = db_.get_song_state(sess.content_hash);
    bool is_discoverer  = (new_state.play_count == 1); // this was the first play

    json resp = {
        {"status", "ok"},
        {"play_count", new_state.play_count},
        {"is_discoverer", is_discoverer},
        {"cosigned", {
            {"listener", proof.player_pubkey    != PubKey33{}},
            {"relay",    proof.mini_node_pubkey != PubKey33{}},
        }},
        {"tokens_minted", {
            {"artist_amount",     Ledger::format_balance(artist_amount)},
            {"node_amount",       Ledger::format_balance(node_amount)},
            {"discoverer_amount", Ledger::format_balance(discoverer_amount)},
            {"seeder_amount",     Ledger::format_balance(seeder_amount)},
            {"mini_node_amount",  Ledger::format_balance(mini_amount)},
            {"escrow_amount",     Ledger::format_balance(escrow_amount)},
        }},
    };
    return {200, resp.dump()};
}

// ---- Stage 2: node-authoritative seeder / relay lane resolution ------
//
// Where the identities genuinely come from, and why none of it is client input:
//
//   1. stream.open (RatsApi) mints a 128-bit delivery_id and writes a `pd:`
//      row recording the CANDIDATE seeder wallets — the wallets that both
//      published this content_hash in their DB2 library AND hold a live
//      wallet-SIGNED presence.hello binding — plus the requesting peer's
//      wallet.
//   2. The MINI-NODE, which is the party that physically forwards the audio
//      frames, signs a relay.report naming the delivery_id, the bytes it
//      relayed, its own wallet, and the peer_id it received the frames FROM.
//      That report is verified against the reporting mini's own pubkey.
//   3. The broker maps that peer_id back to a wallet through the same signed
//      presence binding, and refuses it unless the wallet is in the candidate
//      set from step 1 and is not the listener itself.
//   4. The corroborated result lands in a `dr:` row, which is what this
//      function reads.
//
// So the seeder lane names a wallet that (a) proved control of its key,
// (b) advertised this exact song, and (c) was independently observed serving
// the bytes by a third party that gains nothing from lying about it. The
// listener never gets a say, which is the whole point.
bool HttpServer::resolve_delivery_lanes(const PlaySession& sess,
                                        Address& seeder_out, Address& mini_out,
                                        PubKey33& mini_pk_out,
                                        std::string& why) const {
    seeder_out  = Address{};
    mini_out    = Address{};
    mini_pk_out = PubKey33{};
    if (sess.delivery_id.empty()) { why = "no delivery_id bound to session"; return false; }

    auto row_opt = db_.get("dr:" + sess.delivery_id);
    if (!row_opt) row_opt = db_.get("pd:" + sess.delivery_id);
    if (!row_opt) { why = "no delivery row (not brokered here, or expired)"; return false; }

    json row = json::parse(std::string(row_opt->begin(), row_opt->end()),
                           nullptr, /*allow_exceptions=*/false);
    if (!row.is_object()) { why = "delivery row malformed"; return false; }

    // The delivery must be for the song this session actually played.
    if (row.value("ch", std::string()) != crypto::to_hex(sess.content_hash)) {
        why = "delivery content_hash != session content_hash";
        return false;
    }
    // ... and must have been opened BY this listener. `lw` is the wallet the
    // broker resolved from the stream.open requester's signed presence
    // binding, so this blocks one listener replaying another's delivery_id.
    const std::string lw = row.value("lw", std::string());
    if (!lw.empty()) {
        Address listener{};
        if (!crypto::parse_address(lw, listener) || listener != sess.player_address) {
            why = "delivery was opened by a different wallet";
            return false;
        }
    }

    bool any = false;
    Address a{};
    // Seeder: only present once a mini-node's SIGNED relay.report named a peer
    // the broker could resolve to a presence-bound holder of this song.
    if (crypto::parse_address(row.value("sd", std::string()), a) && a != Address{}) {
        if (a == sess.player_address) {
            // Self-seed: compute_mint_outputs would skip this lane anyway, but
            // zero it here too so the on-chain proof doesn't carry a claim the
            // chain silently ignores.
            std::cout << "[lanes] self-seed suppressed for "
                      << crypto::to_checksum_hex(a) << "\n";
        } else {
            seeder_out = a;
            any = true;
        }
    }
    // Relay: the mini wallet AND pubkey, both taken from the same wallet-signed
    // relay.report (the broker verified address_from_pubkey(pubkey) == wallet
    // before storing either). The pubkey is needed here and not just at payout
    // time because the v3 preimage covers it.
    a = Address{};
    if (crypto::parse_address(row.value("mw", std::string()), a) && a != Address{}) {
        mini_out = a;
        auto pk = crypto::from_hex(row.value("mpk", std::string()));
        if (pk.size() == 33) {
            PubKey33 candidate{};
            std::copy(pk.begin(), pk.end(), candidate.begin());
            if (crypto::address_from_pubkey(candidate) == a) mini_pk_out = candidate;
        }
        any = true;
    }
    if (!any) { why = "delivery row carries neither a corroborated seeder nor a relay"; return false; }
    return true;
}

void HttpServer::consume_delivery_row(const std::string& delivery_id_hex) {
    db_.del("dr:" + delivery_id_hex);
    db_.del("pd:" + delivery_id_hex);
}



// ---- Wallet routes --------------------------------------------------

std::pair<int, std::string> HttpServer::get_wallet_address() {
    json j = {{"address", crypto::to_checksum_hex(node_keypair_.address)}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::get_wallet_nonce(const std::string& address_hex) {
    Address addr;
    if (!crypto::parse_address_checksummed(address_hex, addr))
        return {400, R"({"error":"invalid address"})"};
    uint64_t nonce = db_.get_nonce(addr);
    json j = {{"address", crypto::to_checksum_hex(addr)}, {"nonce", nonce}};
    return {200, j.dump()};
}

std::pair<int, std::string> HttpServer::post_wallet_create() {
    auto kp = crypto::generate_keypair();
    json j = {
        {"address",     crypto::to_checksum_hex(kp.address)},
        {"public_key",  crypto::to_hex(kp.public_key.data(), 33)},
        {"private_key", crypto::to_hex(kp.private_key.data(), kp.private_key.size())},
    };
    return {200, j.dump()};
}

// ---- Moderator routes -----------------------------------------------

static bool verify_moderator_sig(const std::string& mod_addr_hex,
                                  const std::string& sig_hex,
                                  const std::string& sign_msg,
                                  Database& db) {
    Address mod_addr;
    if (!crypto::parse_address_checksummed(mod_addr_hex, mod_addr)) return false;
    if (!db.is_moderator(mod_addr)) return false;
    auto sig_bytes = crypto::from_hex(sig_hex);
    if (sig_bytes.size() != 64) return false;
    Sig64 sig;
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    Hash256 hash = crypto::sha256(reinterpret_cast<const uint8_t*>(sign_msg.data()),
                                   sign_msg.size());
    return crypto::verify_ecdsa_from_address(hash, sig, mod_addr);
}

std::pair<int, std::string> HttpServer::post_moderator_release(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string mod_addr_hex = j["moderator_address"];
        std::string mod_sig_hex  = j["moderator_signature"];
        std::string from_hex_str = j["from_address"];
        std::string to_hex_str   = j["to_address"];
        std::string amount_str   = j["amount"];

        std::string sign_msg = from_hex_str + to_hex_str + amount_str;
        if (!verify_moderator_sig(mod_addr_hex, mod_sig_hex, sign_msg, db_))
            return {403, R"({"error":"unauthorized"})"};

        Address from_addr, to_addr;
        if (!crypto::parse_address_checksummed(from_hex_str, from_addr))
            return {400, R"({"error":"bad from_address"})"};
        if (!crypto::parse_address_checksummed(to_hex_str, to_addr))
            return {400, R"({"error":"bad to_address"})"};

        uint64_t amount = 0;
        if (!Ledger::parse_balance(amount_str, amount))
            return {400, R"({"error":"bad amount"})"};

        leveldb::WriteBatch batch;
        Ledger ledger(db_);
        if (!ledger.transfer(batch, from_addr, to_addr, amount))
            return {400, R"({"error":"insufficient balance"})"};
        db_.write(batch);

        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::delete_song(const std::string& content_hash_hex,
                                                      const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string mod_addr_hex = j["moderator_address"];
        std::string mod_sig_hex  = j["moderator_signature"];

        std::string sign_msg = "delete:" + content_hash_hex;
        if (!verify_moderator_sig(mod_addr_hex, mod_sig_hex, sign_msg, db_))
            return {403, R"({"error":"unauthorized"})"};

        Hash256 ch;
        if (!crypto::parse_hash256(content_hash_hex, ch))
            return {400, R"({"error":"bad content_hash"})"};

        leveldb::WriteBatch batch;
        db_.mark_song_deleted(batch, ch);
        db_.write(batch);

        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

// ---- Transfer route -------------------------------------------------

std::pair<int, std::string> HttpServer::post_transfer(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string from_hex_str = j["from_address"];
        std::string to_hex_str   = j["to_address"];
        std::string amount_str   = j["amount"];
        uint64_t    nonce        = j["nonce"].get<uint64_t>();
        std::string sig_hex      = j["signature"];
        std::string pub_hex      = j.value("from_pubkey", std::string());

        Address from_addr, to_addr;
        if (!crypto::parse_address_checksummed(from_hex_str, from_addr))
            return {400, R"({"error":"bad from_address"})"};
        if (!crypto::parse_address_checksummed(to_hex_str, to_addr))
            return {400, R"({"error":"bad to_address"})"};

        uint64_t amount = 0;
        if (!Ledger::parse_balance(amount_str, amount))
            return {400, R"({"error":"bad amount"})"};

        auto sig_bytes = crypto::from_hex(sig_hex);
        if (sig_bytes.size() != 64)
            return {400, R"({"error":"bad signature"})"};

        // from_pubkey is required now that verify_signature cross-checks
        // the inline pubkey against from_address (no ECDSA recovery).
        auto pub_bytes = crypto::from_hex(pub_hex);
        if (pub_bytes.size() != 33)
            return {400, R"({"error":"bad from_pubkey"})"};

        TransferTx tx;
        tx.from_address = from_addr;
        tx.to_address   = to_addr;
        tx.amount       = amount;
        tx.nonce        = nonce;
        std::copy(pub_bytes.begin(), pub_bytes.end(), tx.from_pubkey.begin());
        std::copy(sig_bytes.begin(), sig_bytes.end(), tx.signature.begin());

        // Consensus safety (tx-set determinism): do NOT apply the transfer to
        // the ledger here, and do NOT drop an un-flooded tx into the mempool.
        // Applying a transfer OUTSIDE a block mutates consensus-derived state on
        // THIS node alone (divergence), and a p: row that is stored but never
        // flooded gets included by this node's producer (submit_ms 0 <= block_ts)
        // yet is absent on every peer — two nodes then mint different bodies at
        // the same height (tx-set fork). Wallet transfers now reach the chain
        // ONLY through the flooded, pt:-stamped path — the rats `wallet.transfer`
        // handler reconstructs this same signed tx and submits it via ingest_tx,
        // exactly like username.register / proposal.submit. This legacy HTTP
        // route has no rats client to flood with, so it just verifies the
        // signature and returns the tx_hash; the actual submission happens over
        // the mesh. (verify_signature also cross-checks from_pubkey ->
        // from_address; nonce/balance are enforced at block-apply.)
        if (!tx.verify_signature())
            return {400, R"({"error":"transfer rejected"})"};

        json resp = {{"status", "ok"}, {"tx_hash", crypto::to_hex(tx.tx_hash())}};
        return {200, resp.dump()};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}

std::pair<int, std::string> HttpServer::post_net_announce(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string ipv6        = j.value("ipv6",     "");
        uint16_t    api_port    = static_cast<uint16_t>(j.value("api_port", 0));
        std::string node_id_hex = j.value("node_id",  "");

        if (node_id_hex.size() != 64)
            return {400, R"({"error":"invalid node_id"})"};
        if (ipv6.empty())
            return {400, R"({"error":"ipv6 required"})"};

        Hash256 node_id{};
        if (!crypto::parse_hash256(node_id_hex, node_id))
            return {400, R"({"error":"invalid node_id"})"};

        network_.inject_peer(ipv6, api_port, node_id);
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid json"})"};
    }
}

// ---- Block sync routes ----------------------------------------------

// Block sync via HTTP removed — moves to rats binary chunks later.
#if 0
static std::pair<int, std::string> _legacy_get_blocks_after(uint32_t after_height, uint32_t limit) { (void)after_height; (void)limit; return {200, "[]"}; }
#endif

std::pair<int, std::string> HttpServer::post_sync_block(const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string raw_hex = j.value("raw_hex", "");
        if (raw_hex.empty())
            return {400, R"({"error":"raw_hex required"})"};

        auto raw = crypto::from_hex(raw_hex);
        if (raw.empty())
            return {400, R"({"error":"invalid hex"})"};

        Block block;
        if (!Block::deserialize(raw.data(), raw.size(), block))
            return {400, R"({"error":"block deserialization failed"})"};

        std::string err;
        if (!chain_.validate_block(block, err))
            return {400, json{{"error", "block validation failed"}, {"detail", err}}.dump()};

        // Write .blk file before connecting (at expected next height)
        uint32_t new_height = chain_.tip().height + 1;
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << new_height << ".blk";
        auto blocks_dir = std::filesystem::path(config_.data_dir) / "blocks";
        std::filesystem::create_directories(blocks_dir);
        auto block_path = blocks_dir / fname.str();
        {
            std::ofstream f(block_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(raw.data()), raw.size());
        }

        if (!chain_.connect_block(block)) {
            std::filesystem::remove(block_path);
            return {400, R"({"error":"block rejected by chain"})"};
        }

        std::cout << "[sync] connected block at height " << chain_.tip().height << "\n";
        return {200, R"({"status":"ok"})"};
    } catch (...) {
        return {400, R"({"error":"invalid request"})"};
    }
}


// ---- Helpers --------------------------------------------------------

// send_json / send_binary removed — HTTP/3 responses are emitted by
// transport/h3_server.cpp via MsH3RequestSend(). The dispatcher there
// reads the verb's {status, json-or-bytes, content_type} tuple and ships it.

std::string HttpServer::generate_session_id() const {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    Hash256 id{};
    for (int i = 0; i < 4; ++i) {
        uint64_t v = dist(gen);
        for (int j = 0; j < 8; ++j) id[i*8+j] = (v >> (j*8)) & 0xFF;
    }
    return crypto::to_hex(id);
}

} // namespace mc::api
