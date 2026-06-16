#pragma once
#include "../core/block.h"
#include "../core/transaction.h"
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace mc {

struct SongState {
    uint64_t play_count          = 0;
    Address  discoverer_address  = {};
    uint32_t first_play_block    = 0;
    uint64_t first_play_timestamp = 0;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, SongState& out);
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    bool is_open() const { return db_ != nullptr; }

    // ---- Generic get/put/del ----------------------------------------
    std::optional<std::vector<uint8_t>> get(const std::string& key) const;
    bool put(const std::string& key, const std::vector<uint8_t>& value);
    bool del(const std::string& key);
    bool write(leveldb::WriteBatch& batch);

    // ---- Batch helpers -----------------------------------------------
    void put_batch(leveldb::WriteBatch& b, const std::string& key,
                   const std::vector<uint8_t>& value);
    void del_batch(leveldb::WriteBatch& b, const std::string& key);
    void put_batch_u32(leveldb::WriteBatch& b, const std::string& key, uint32_t v);
    void put_batch_u64(leveldb::WriteBatch& b, const std::string& key, uint64_t v);

    std::optional<uint32_t> get_u32(const std::string& key) const;
    std::optional<uint64_t> get_u64(const std::string& key) const;

    // ---- Song state --------------------------------------------------
    SongState get_song_state(const Hash256& content_hash) const;
    void      set_song_state(leveldb::WriteBatch& b, const Hash256& content_hash,
                             const SongState& state);
    uint64_t  get_play_count(const Hash256& content_hash) const;
    void      update_song_state(leveldb::WriteBatch& b, const PlayProof& proof,
                                uint64_t play_count_before);

    // ---- Balance ledger ----------------------------------------------
    uint64_t get_balance(const Address& address) const;
    void     set_balance(leveldb::WriteBatch& b, const Address& address, uint64_t balance);
    uint64_t get_total_supply() const;
    void     set_total_supply(leveldb::WriteBatch& b, uint64_t total);

    // ---- Session tracking -------------------------------------------
    bool is_session_used(const Hash256& session_id) const;
    void mark_session_used(leveldb::WriteBatch& b, const Hash256& session_id);

    // ---- Fingerprint index ------------------------------------------
    void put_fingerprint(leveldb::WriteBatch& b, const SongSection& song);
    void del_fingerprint(leveldb::WriteBatch& b, const Hash256& content_hash);

    struct FingerprintEntry {
        std::string compressed_fingerprint;
        Hash256     block_hash;
    };
    std::optional<FingerprintEntry> get_fingerprint(const Hash256& content_hash) const;

    /// Reverse index: SHA256(compressed_fingerprint) → content_hash. Used
    /// by `fingerprint.submit` so a player that hashed audio locally can
    /// learn whether the network already has that song and announce
    /// itself to the swarm if so. Returns nullopt when the fingerprint
    /// hash is unknown to the chain.
    std::optional<Hash256> get_content_hash_for_fingerprint(
        const Hash256& fingerprint_hash) const;

    // Get list of content hashes in a bucket
    std::vector<Hash256> get_bucket(uint16_t bucket_id) const;
    void add_to_bucket(leveldb::WriteBatch& b, uint16_t bucket_id,
                       const Hash256& content_hash);
    void remove_from_bucket(leveldb::WriteBatch& b, uint16_t bucket_id,
                            const Hash256& content_hash);

    // ---- Mempool -----------------------------------------------------
    bool put_pending_tx(const Hash256& tx_hash, const std::vector<uint8_t>& tx_data);
    bool del_pending_tx(const Hash256& tx_hash);
    std::vector<std::pair<Hash256, std::vector<uint8_t>>> get_all_pending_txs() const;

    // ---- Song metadata index (sm:, ia:, ig:) -------------------------
    struct SongMeta {
        std::string title;
        std::string artist;
        std::string genre;
        std::string album;
        uint32_t    duration_ms  = 0;
        uint16_t    year         = 0;
        uint16_t    track_number = 0;
        Hash256     content_hash;
    };
    void put_song_meta(leveldb::WriteBatch& b, const Hash256& ch, const SongSection& song);
    std::optional<SongMeta> get_song_meta(const Hash256& ch) const;
    std::vector<Hash256> get_all_song_hashes() const;
    void add_to_artist_index(leveldb::WriteBatch& b, const std::string& artist, const Hash256& ch);
    void add_to_genre_index(leveldb::WriteBatch& b, const std::string& genre, const Hash256& ch);
    std::vector<Hash256> get_songs_by_artist(const std::string& artist) const;
    std::vector<Hash256> get_songs_by_genre(const std::string& genre) const;

    // ---- Content-hash → block height (bh:) for streaming -------------
    void                    set_content_height(leveldb::WriteBatch& b, const Hash256& ch, uint32_t height);
    std::optional<uint32_t> get_content_height(const Hash256& ch) const;

    // ---- Moderator / deleted songs (m:, d:) --------------------------
    bool is_moderator(const Address& addr) const;
    void add_moderator(leveldb::WriteBatch& b, const Address& addr);
    bool is_song_deleted(const Hash256& ch) const;
    void mark_song_deleted(leveldb::WriteBatch& b, const Hash256& ch);
    void unmark_song_deleted(leveldb::WriteBatch& b, const Hash256& ch);

    // ---- On-chain moderator records (founder:, mlvl:, mpub:, mact:) --
    //
    // Records the GRANT/REVOKE state that drives quorum decisions in the
    // moderator system. The legacy `m:` table above is now derived state:
    // any non-zero mlvl entry implies membership and gets mirrored into
    // `m:` on apply, so existing call sites that only need a boolean
    // "is this a moderator at all" still work unchanged.
    //
    //   founder:        — single-entry sentinel. Empty = no founder yet,
    //                     present = founder bootstrap has happened and
    //                     value is the founder's 20-byte address.
    //   mlvl:<hex(addr)> — ModLevel (1 byte). Absence == NONE.
    //   mpub:<hex(addr)> — compressed pubkey (33 bytes). Lets other nodes
    //                     verify future ECIES messages without needing
    //                     the moderator online.
    //   mact:<hex(addr)> — block height at which the moderator was
    //                     granted. Used as "active since" for quorum.
    std::optional<Address> get_founder() const;
    void                   set_founder(leveldb::WriteBatch& b, const Address& addr);

    uint8_t                get_mod_level(const Address& addr) const;       // 0 if absent
    void                   set_mod_level(leveldb::WriteBatch& b,
                                         const Address& addr, uint8_t level);

    std::optional<PubKey33> get_mod_pubkey(const Address& addr) const;
    void                    set_mod_pubkey(leveldb::WriteBatch& b,
                                           const Address& addr,
                                           const PubKey33& pubkey);

    std::optional<uint32_t> get_mod_active_block(const Address& addr) const;
    void                    set_mod_active_block(leveldb::WriteBatch& b,
                                                 const Address& addr, uint32_t height);

    /// Every address currently holding a non-zero ModLevel. Order is
    /// undefined. Quorum math uses size() of this list against votes.
    std::vector<Address> list_active_moderators() const;

    // ---- Category hide lists (ha:, hb:, ht:) -------------------------
    //
    // Moderator-driven soft censorship by metadata. The chain still
    // carries the underlying SongSection block bytes — these tables just
    // mask the title / artist / album from songs.list and from any
    // metadata-driven search response. Match is case-insensitive on the
    // lowercased value.
    bool is_hidden_artist(const std::string& artist) const;
    bool is_hidden_album (const std::string& album)  const;
    bool is_hidden_title (const std::string& title)  const;
    void add_hidden_artist (leveldb::WriteBatch& b, const std::string& artist);
    void add_hidden_album  (leveldb::WriteBatch& b, const std::string& album);
    void add_hidden_title  (leveldb::WriteBatch& b, const std::string& title);
    void remove_hidden_artist(leveldb::WriteBatch& b, const std::string& artist);
    void remove_hidden_album (leveldb::WriteBatch& b, const std::string& album);
    void remove_hidden_title (leveldb::WriteBatch& b, const std::string& title);
    std::vector<std::string> list_hidden_artists() const;
    std::vector<std::string> list_hidden_albums()  const;
    std::vector<std::string> list_hidden_titles()  const;

    // ---- Moderation gossip log (ml:) ---------------------------------
    //
    // Every accepted hide/unhide is captured here as the signed wire
    // envelope (JSON). Keyed `ml:<be16-hex(ts_ms)>:<sig16>` so the log
    // can be range-scanned by timestamp and deduped by signature. New
    // nodes call mod.sync_since on connect and replay the diff so their
    // hidden_* and song_deleted tables converge to the moderator's
    // intent without anyone manually re-keying the actions.
    void append_mod_log_entry(leveldb::WriteBatch& b,
                              uint64_t            ts_ms,
                              const std::string&  sig_hex,
                              const std::string&  payload_json);
    bool mod_log_has_sig(const std::string& sig_hex) const;
    uint64_t latest_mod_log_ts() const;
    void iter_mod_log_since(
        uint64_t since_ts_ms,
        const std::function<bool(uint64_t ts_ms,
                                 const std::string& sig_hex,
                                 const std::string& payload_json)>& cb) const;

    // ---- Transfer nonce (nv:) ----------------------------------------
    uint64_t get_nonce(const Address& addr) const;
    void     set_nonce(leveldb::WriteBatch& b, const Address& addr, uint64_t nonce);

    // ---- Generic prefix iteration ------------------------------------
    // Visit every key/value pair whose key starts with [prefix]. The
    // callback returns false to stop, true to continue. Used by
    // SwarmIndex to slurp persisted swarm entries on startup without
    // exposing leveldb internals.
    void for_each_with_prefix(
        const std::string& prefix,
        const std::function<bool(const std::string&,
                                 const std::string&)>& cb) const;

    // ---- Utility -----------------------------------------------------
    std::string hex(const Hash256& h) const;
    std::string hex(const Address& a) const;

    // Wipe all derived state (balances, song states, fingerprint index, sessions)
    void clear_derived_state();

    // Get database approximate size in bytes
    uint64_t approximate_size() const;

private:
    leveldb::DB* db_ = nullptr;
    std::string  path_;

    std::string bucket_key(uint16_t bucket_id) const;
};

} // namespace mc
