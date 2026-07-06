#pragma once
#include <cstdint>

#include "../core/chain.h"
#include "../storage/database.h"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc::curation {

// One curated row on the Discover home ("Rising: under 10k plays",
// "Best of Jazz", "2019 hits", ...). `songs` is the ordered hash list;
// metadata is joined downstream (gateway / client) so the wire stays cheap.
struct Collection {
    std::string id;        // stable, derived: "rising:", "top:", "genre:jazz", ...
    std::string kind;      // "rising" | "top" | "new" | "genre" | "year"
    std::string title;     // display title
    std::string subtitle;  // display subtitle
    std::string facet;     // normalized facet value ("jazz", "2019"), "" for globals
    std::vector<Hash256> songs;
};

struct CollectionSet {
    uint32_t    snapshot_height = 0;   // buried epoch lower boundary, NOT the tip
    uint64_t    epoch           = 0;   // tip_height / kEpochBlocks at generation
    Hash256     snapshot_block_hash{}; // chain hash at snapshot_height (cross-check)
    std::string content_digest;        // sha256 hex of the canonical byte form
    std::vector<Collection> collections;
};

/// CollectionCurator — deterministic Discover-feed generator.
///
/// Once per epoch (`epoch = tip_height / kEpochBlocks`) it scans the on-chain
/// song set and derives curated collections from consensus state ONLY
/// (play_count, first_play_block, genre, year — never swarm_size, wall clocks,
/// or map iteration order). Every sort tie-breaks on content_hash byte order,
/// so two honest nodes at the same epoch emit byte-identical hash lists and an
/// equal `content_digest`; clients corroborate by comparing digests across
/// nodes with no signing / trust anchor.
///
/// Threading mirrors RelayCreditTracker::loop: atomic running_, one worker,
/// 1-s-increment sleeps so stop() unblocks fast, heavy work done off-lock and
/// swapped in under a short mutex. Pure reader — never writes chain or db.
class CollectionCurator {
public:
    /// Epoch width in blocks. Collections are stable for a whole epoch, so
    /// shallow near-tip reorgs can't churn the answer between regenerations.
    /// Tune to real block cadence: too short = churn + wasted O(N) scans,
    /// too long = stale Discover.
    static constexpr uint32_t kEpochBlocks          = 360;
    static constexpr uint32_t kPollIntervalMs       = 30 * 1000;
    static constexpr size_t   kMaxSongsPerCollection = 50;
    static constexpr size_t   kMaxGenreRows          = 24;
    static constexpr size_t   kMaxYearRows           = 10;

    CollectionCurator(Chain& chain, Database& db);
    ~CollectionCurator();

    void start();
    void stop();

    bool ready() const;

    /// Full `collections.list` reply body. With `embed`, a deduplicated
    /// `songs` object (hash → metadata) rides along so a client can render
    /// without a second catalog fetch; the digest is always computed over the
    /// hashes-only canonical form, so embed never affects cross-checking.
    nlohmann::json list_json(bool embed) const;

    /// Single collection by id (same shape as one `collections` element,
    /// plus the set header). Returns a null json when the id is unknown.
    nlohmann::json get_json(const std::string& id, bool embed) const;

private:
    void loop();
    void regenerate(uint64_t epoch, uint32_t snapshot_height);

    Chain&    chain_;
    Database& db_;

    mutable std::mutex mu_;      // guards set_ + cached bodies + ready_
    CollectionSet      set_;
    // Reply bodies are pre-built at generation time so the RPC handler is a
    // copy under the lock — no db reads on the request path.
    std::shared_ptr<const nlohmann::json> body_plain_;
    std::shared_ptr<const nlohmann::json> body_embed_;
    bool               ready_ = false;

    uint64_t           last_epoch_ = UINT64_MAX;
    std::thread        worker_;
    std::atomic<bool>  running_{false};
};

} // namespace mc::curation
