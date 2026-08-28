#pragma once
// explorer_index.h — read-only, in-memory blockchain-explorer index.
//
// Feeds the explorer verbs in rats_api.cpp (blocks.list / blocks.get /
// tx.get / address.summary / address.history / search / stats.overview /
// stats.artist / stats.top / song.detail / song.plays). STRICTLY additive
// read-only surface: it never writes chain state, never touches consensus,
// and reads blocks through the same "n:" / "b:" LevelDB rows the chain
// itself uses.
//
// Build model: LAZY + INCREMENTAL. The index is empty until the first
// explorer verb arrives; that call walks every block once (0..tip) and
// every subsequent call only indexes the delta above the last indexed
// height. A reorg (indexed tip hash no longer at its height) triggers a
// full rebuild — reorgs are bounded by FINALITY_DEPTH and a full walk of
// the current ~600-block chain is milliseconds, so the simple answer wins
// over surgical rewind. Everything lives behind one mutex; all queries are
// cheap map lookups after the sync step.
//
// Memory is O(total txs + addresses + songs): per tx ~56 B (hash→loc), per
// address touch ~12 B (ref) plus fixed ~200 B per distinct address, per
// song/artist small aggregates + a unique-listener set. See stats.overview's
// "index" object for live counts. For a chain that outgrows RAM, the same
// shapes can be persisted under NEW LevelDB prefixes (e.g. "xt:", "xa:")
// without touching any existing schema — not needed at current scale.

#include "../core/block.h"
#include "../core/chain.h"
#include "../core/transaction.h"
#include "../storage/database.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mc::api {

class ExplorerIndex {
public:
    ExplorerIndex(Chain& chain, Database& db) : chain_(chain), db_(db) {}

    // Every handler returns {http-ish status, JSON body text}; feed the pair
    // straight through wrap_handler_result in rats_api.cpp.
    using Result = std::pair<int, std::string>;

    Result blocks_list(const nlohmann::json& in);      // {offset,limit}|{from,to}|{since}
    Result blocks_get(const nlohmann::json& in);       // {height}|{hash}|{id}
    Result tx_get(const nlohmann::json& in);           // {hash}
    Result address_summary(const nlohmann::json& in);  // {address}
    Result address_history(const nlohmann::json& in);  // {address,offset,limit,role?}
    Result search(const nlohmann::json& in);           // {q,limit}
    Result stats_overview(const nlohmann::json& in);   // {days?,since?}
    Result stats_artist(const nlohmann::json& in);     // {artist_address}|{artist}
    Result stats_top(const nlohmann::json& in);        // {kind,n?,since?}
    Result song_detail(const nlohmann::json& in);      // {content_hash}
    Result song_plays(const nlohmann::json& in);       // {content_hash,offset,limit}
    Result moderation_list(const nlohmann::json& in);      // {offset,limit}
    Result moderation_hidden(const nlohmann::json& in);    // {}
    Result moderation_moderator(const nlohmann::json& in); // {address,offset,limit}

private:
    template <size_t N>
    struct ByteArrHash {
        size_t operator()(const std::array<uint8_t, N>& a) const {
            size_t h = 0;
            std::memcpy(&h, a.data(), sizeof(h) < N ? sizeof(h) : N);
            return h;
        }
    };
    using H32 = ByteArrHash<32>;
    using H20 = ByteArrHash<20>;

    // Role bits an address can play in a transaction.
    enum : uint16_t {
        R_SENDER    = 1 << 0,
        R_RECIPIENT = 1 << 1,
        R_LISTENER  = 1 << 2,
        R_ARTIST    = 1 << 3,
        R_SEEDER    = 1 << 4,
        R_RELAY     = 1 << 5,
        R_NODE      = 1 << 6,
    };

    struct BlockRow {
        Hash256  hash{};
        uint64_t timestamp_ms = 0;
        uint32_t tx_count     = 0;
        uint32_t size_bytes   = 0;
        uint64_t weight       = 0;   // this block's play contribution (count_plays)
        bool     has_song     = false;
        std::string song_title;      // small convenience for list rows
        std::string song_artist;
    };

    struct TxLoc { uint32_t height = 0; uint32_t index = 0; };

    struct TxRef { uint32_t height = 0; uint32_t index = 0; uint16_t roles = 0; };

    struct AddrInfo {
        std::vector<TxRef> refs;                 // chain order (oldest first)
        uint32_t first_seen = 0, last_seen = 0;
        uint64_t plays_listener = 0, plays_artist = 0,
                 plays_seeder   = 0, plays_relay  = 0;
        uint64_t earned_artist = 0, earned_seeder = 0, earned_relay = 0,
                 earned_node   = 0, earned_listener = 0, earned_other = 0;
        uint64_t burned = 0;
        std::map<uint32_t, uint32_t> listen_days;  // utc-day -> plays as listener
    };

    // One play event. For a MINT, (height,index) is the mint tx and sub is
    // UINT32_MAX. For a settlement constituent, (height,index) is the
    // SETTLEMENT_MINT tx and sub is the proof's ordinal in the sb: body.
    struct PlayRef { uint32_t height = 0; uint32_t index = 0; uint32_t sub = UINT32_MAX; };

    struct SongAgg {
        uint32_t    reg_height = 0;
        Hash256     reg_block_hash{};
        std::string title, artist_name, genre;
        Address     artist_address{};
        uint64_t    plays = 0;
        uint64_t    artist_earned = 0;   // outputs to artist / artist escrow
        std::unordered_set<Address, H20> listeners;
        std::map<uint32_t, uint64_t>     plays_by_day;
        std::vector<PlayRef>             play_refs;   // chain order
    };

    struct ArtistAgg {
        std::string name;
        uint64_t    plays = 0, earned = 0;
        std::unordered_set<Address, H20>            listeners;
        std::unordered_map<Hash256, uint64_t, H32>  plays_per_song;
        std::map<uint32_t, uint64_t>                plays_by_day;
        std::unordered_map<Address, uint64_t, H20>  seeders, relays;
        std::set<uint32_t>                          blocks;  // heights the artist appears in
        // Cross-keying facts: for an ADDRESS-keyed agg, every display name the
        // wallet registered songs under; for a NAME-keyed agg, every address
        // that registered songs under the name.
        std::set<std::string>                       names;
        std::set<Address>                           addresses;
    };

    // One moderation action, from either public source:
    //   * source "block"  — a ModeratorOpTx / ProposalTx inside a block
    //     (grant / revoke / label_edit / proposal_* / vote_yes).
    //   * source "gossip" — a moderator-signed hide/unhide envelope from the
    //     replicated ml: mod log (mod_action.h). These are NOT block txs, so
    //     they carry ts_ms + sig instead of height + tx_hash; height is the
    //     chain height whose block was current when the action was signed
    //     (derived from block timestamps, 0 if unknown).
    struct ModAction {
        std::string kind;        // hide|unhide|grant|revoke|label_edit|proposal_*|vote_yes|forgery_report
        std::string category;    // artist|album|title|hash|moderator|content|escrow|label
        std::string value;       // hidden value / target hex / meta payload
        Address     moderator{}; // signer (zero when unknown)
        std::string pubkey_hex;  // signer pubkey (gossip envelopes)
        uint64_t    ts_ms = 0;
        uint32_t    height = 0;  // block height ("block") / height-at-signing ("gossip")
        std::string tx_hash;     // "block" source only
        std::string sig_hex;     // "gossip" source only (stable action id)
        std::string source;      // "block" | "gossip"
        uint8_t     level = 0;   // grant level, when applicable
    };

    // ---- sync ----
    void ensure_synced_locked();
    void sync_mod_log_locked();
    void add_mod_action_locked(ModAction&& a);
    uint32_t height_at_ts_locked(uint64_t ts_ms) const;
    void reset_locked();
    void index_block_locked(uint32_t height, const Hash256& hash,
                            const Block& block, size_t size_bytes);
    void index_play_locked(const PlayProof& proof, uint32_t height,
                           uint32_t tx_index, uint32_t sub, uint64_t day,
                           uint64_t ts_ms);
    AddrInfo& touch_locked(const Address& a, uint32_t height, uint32_t index,
                           uint16_t roles);

    // ---- shared JSON builders (call with mu_ held) ----
    nlohmann::json tx_to_json_locked(const std::vector<uint8_t>& raw) const;
    nlohmann::json block_row_json_locked(uint32_t height, const BlockRow& r) const;
    nlohmann::json mod_action_json_locked(const ModAction& a) const;
    nlohmann::json artist_agg_json_locked(const ArtistAgg& ar,
                                          const std::string& scope) const;

    Chain&    chain_;
    Database& db_;
    std::mutex mu_;

    std::map<uint32_t, BlockRow>                blocks_;
    std::unordered_map<Hash256, TxLoc, H32>     tx_loc_;
    std::unordered_map<Address, AddrInfo, H20>  addr_;
    std::unordered_map<Hash256, SongAgg, H32>   songs_;
    // Artist aggregates are kept in TWO keyings. By ADDRESS: the wallet view
    // (one uploader wallet can register many artist names — its address agg
    // merges them, which is correct for wallet-level accounting). By NAME
    // (lowercased): the catalog view — "stats for Surfbort" scoped to songs
    // registered under that name, which is what `artist <name>` means.
    std::unordered_map<Address, ArtistAgg, H20>  artists_;
    std::unordered_map<std::string, ArtistAgg>   artists_by_name_;  // lc name
    std::unordered_map<std::string, Address>     artist_by_name_;   // lc name -> addr
    std::map<std::string, uint64_t>             genre_counts_;    // lowercased

    uint64_t total_txs_ = 0, total_plays_ = 0, total_minted_ = 0, total_burned_ = 0;
    std::map<uint8_t, uint64_t> tx_type_counts_;
    std::unordered_set<Address, H20> uniq_listeners_, uniq_artists_,
                                     uniq_seeders_, uniq_relays_;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> daily_;  // day -> {plays, minted}

    // Moderator-vote provenance: every ProposalTx we saw, keyed by its tx
    // hash, with the YES votes that referenced it. Whether it EXECUTED comes
    // from the public propstatus: db row (authoritative), not from replaying
    // quorum math.
    struct VoteRef { Address voter{}; uint32_t height = 0; std::string tx_hash; };
    struct ProposalInfo {
        uint8_t     kind = 0;          // ProposalKind
        Hash256     target_hash{};     // HIDE_CONTENT: content hash
        Address     target_addr{};
        uint64_t    amount = 0;
        Address     proposer{};
        uint32_t    height = 0;
        uint64_t    ts_ms  = 0;
        std::string tx_hash;
        std::vector<VoteRef> votes;
    };
    std::unordered_map<Hash256, ProposalInfo, H32>          proposals_;
    std::unordered_map<Hash256, std::vector<Hash256>, H32>  proposals_by_target_;

    // Provenance of one hidden item: founder / moderator / vote /
    // forgery_quorum / unknown, with the supporting detail. The on-chain
    // envelope has NO reason field, so `reason` is always "unrecorded".
    nlohmann::json provenance_json_locked(const std::string& category,
                                          const std::string& value) const;
    nlohmann::json proposal_json_locked(const ProposalInfo& p) const;

    // Moderation (block txs + ml: gossip log), chain/ts order.
    std::vector<ModAction>                                mod_actions_;
    std::unordered_map<Address, std::vector<size_t>, H20> mod_by_addr_;
    std::unordered_set<std::string>                       mod_seen_sigs_;  // sig16 prefixes
    uint64_t mod_log_synced_ts_ = 0;
    uint64_t mod_hides_ = 0, mod_unhides_ = 0, mod_grants_ = 0,
             mod_revokes_ = 0, mod_label_edits_ = 0;

    uint32_t indexed_height_ = 0;
    bool     have_any_       = false;
    Hash256  indexed_tip_hash_{};
};

} // namespace mc::api
