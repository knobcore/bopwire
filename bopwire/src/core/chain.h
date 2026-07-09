#pragma once
#include <cstdint>
#include "block.h"
#include "transaction.h"
#include "../storage/database.h"
#include <optional>
#include <string>
#include <map>
#include <mutex>
#include <set>

namespace mc {

struct ChainTip {
    Hash256  hash;
    uint32_t height;
    // Timestamp of the tip header (ms since epoch). Used by the
    // fork-choice rule below to break ties. Zero on a fresh chain.
    uint64_t timestamp_ms = 0;
    // Fork weight (#8) = cumulative count of MintTx (play-reward mints)
    // across every block from genesis to this tip. Each MintTx required a
    // realtime, per-device-gated, audited play to exist, so this is
    // "cumulative audited plays" — demand-backed and deterministic (every
    // node counts the same MintTxs). Heartbeat / song-registration blocks
    // with no plays add 0, so a fork padded with free heartbeats cannot
    // out-weight a chain carrying real plays. Persisted per block under
    // the "cw:" prefix; recovered on startup.
    uint64_t weight = 0;
};

// Reorg finality cap (Phase 0). No fork may rewrite history deeper than this
// many blocks below the current tip: reorg_to_branch rejects a branch whose
// fork point is older than tip.height - FINALITY_DEPTH. This bounds the depth a
// costless heavier fork can rewrite (fork weight is cheap without stake/PoW —
// hardened in the sybil/finality phase) and, symmetrically, makes it SAFE to
// prune tx bodies below this depth: nothing valid will ever reorg past it.
// Tunable pre-mainnet; must exceed the deepest realistic honest partition heal.
static constexpr uint32_t FINALITY_DEPTH = 1000;

// Hard cap on how many play constituents a single SETTLEMENT_MINT may carry
// (Phase 3). Bounds both the companion-body size (DoS) and, since fork weight
// counts VERIFIED constituents, the weight a single settlement can contribute —
// so an attacker can't declare a huge count for cheap fork weight.
static constexpr uint32_t MAX_CONSTITUENTS_PER_SETTLEMENT = 4096;

// Clock-skew allowance for a flooded tx's origin submit_ms relative to the
// current tip's block timestamp. Together with the lower bound
// (tip_ts - PROPAGATION_WINDOW_MS) this lets ingest reject both future-dated and
// backdated-"instantly-mature" txs, so a peer can't manufacture maturity.
static constexpr uint64_t MAX_SUBMIT_SKEW_MS = 5'000;

// Hardcoded checkpoint baked into the binary. Any chain we sync from a
// peer MUST contain this exact block hash at this exact height — if not,
// the peer is feeding us a fork that branches before the checkpoint,
// which we refuse. Eclipse defense: even if every peer we connect to is
// the attacker, they can't forge a chain that re-uses our checkpoint hash
// without already having the matching block. Update on every audited
// release; older checkpoints can stay in the list (they all have to pass).
struct Checkpoint {
    uint32_t height;
    Hash256  hash;
};
inline std::vector<Checkpoint> hardcoded_checkpoints() {
    // Empty for now — populate with (audited_height, audited_hash)
    // values once mainnet has run long enough that we trust a height
    // is irreversible. Until then sync still works (vacuously satisfies
    // every checkpoint), but the eclipse defense is just the per-block
    // validation + peer-diversity gate below.
    return {};
}

// Founder lock. There is no genesis block: the chain starts empty and the
// founder is claimed by the first valid self-GRANT that any node mines. To
// stop anyone from re-bootstrapping the network (e.g. in the empty-chain
// window right after a wipe), we pin the one address permitted to bootstrap
// the founder into the binary here. Every validating node checks this on
// block-apply (Chain::apply_moderator_op), so a block carrying a founder
// GRANT for any other address fails to apply and is rejected network-wide.
//
// All-zero = UNPINNED (dev/test): behavior falls back to "first valid
// self-grant wins", so local dev and the unit tests are unaffected. The
// real 20-byte address is baked in during the bootstrap ceremony (after the
// bopwire-admin wizard generates the founder wallet) and the release nodes
// are rebuilt. This is deliberately a compile-time constant, like the
// checkpoints above — an attacker can't change it without recompiling, and
// honest nodes running the real binary reject their fork.
inline constexpr Address PINNED_FOUNDER_ADDRESS = {{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
}};

inline bool founder_is_pinned() {
    for (uint8_t b : PINNED_FOUNDER_ADDRESS)
        if (b != 0) return true;
    return false;
}

// Fork-choice rule (#8, Model 1): "better" means we should adopt theirs.
//
//   1. More cumulative audited plays wins (higher weight). This is the
//      Sybil-resistant metric: a heavier chain required more realtime,
//      device-gated, audited plays — work an attacker can't fake for
//      free. Free heartbeat blocks add 0 weight, so they can't pump a
//      fork past a real chain (closes the old free-heartbeat height pump).
//   2. On equal weight, longer chain wins (higher height).
//   3. On ties, newer block timestamp wins.
//   4. On all ties, hash-bytewise wins (deterministic tiebreaker so every
//      node converges on the same winner; prevents oscillation).
//
// Every term is a deterministic function the receiving node recomputes
// itself, so "majority agreement" is convergence on this rule — no votes
// (see docs §22). Cheap to verify: weight + height + ts + hash all live
// in the tip the peer advertises.
inline bool tip_is_better(const ChainTip& candidate, const ChainTip& current) {
    if (candidate.weight != current.weight)
        return candidate.weight > current.weight;
    if (candidate.height != current.height)
        return candidate.height > current.height;
    if (candidate.timestamp_ms != current.timestamp_ms)
        return candidate.timestamp_ms > current.timestamp_ms;
    return candidate.hash > current.hash;
}

// Shared on-chain uniqueness verdict — the SINGLE duplicate-song test used on
// every path (enqueue, build-selection, connect-validate, replay, swarm-join),
// so "is this song already on chain?" is byte-for-byte identical on every node.
// Returns true iff `content_hash` is already registered (exact, via the
// fingerprint index) OR a chromaprint re-encode of it is (fuzzy bucket scan vs
// audio::kChromaprintSimThreshold). Reads only the Database, so the "chain so
// far" is whatever index is committed: the full chain on the connect path, the
// partially-rebuilt index on the replay path — identical semantics to the four
// hand-inlined checks it replaces. Making duplicate-ness one function (not a
// shared constant across copies) is what keeps the fork-hinge verdict from
// drifting between paths or binaries.
bool song_on_chain(const Database& db,
                   const Hash256& content_hash,
                   const std::string& compressed_fingerprint);

// Manages the canonical blockchain: connect/disconnect blocks, tip tracking,
// state derivation (balances, fingerprint index, song state).
class Chain {
public:
    explicit Chain(Database& db);

    // Initialize from database; rebuilds derived state if necessary.
    bool init();

    // Merge operator-supplied checkpoints (from config.json) on top of
    // the baked-in hardcoded_checkpoints(). A config entry at a height
    // that is also hardcoded overrides the baked value (the operator
    // explicitly took responsibility); otherwise the lists are unioned.
    // Call once at startup, BEFORE init()/sync, so the eclipse gate is
    // live before any block is accepted. Returns the count merged.
    size_t add_config_checkpoints(const std::vector<Checkpoint>& cps);

    // Returns current tip (height and hash). Atomic snapshot under the
    // chain's internal mutex so producer + network threads see a
    // consistent (hash, height) pair.
    ChainTip tip() const {
        std::lock_guard<std::mutex> lk(mu_);
        return tip_;
    }

    // Connect a validated block to the chain.
    // Updates tip, balances, song state, fingerprint index atomically.
    bool connect_block(const Block& block);

    // Disconnect the most recent block (for reorg support).
    bool disconnect_block();

    // Fork-choice reorg (#8). `branch` is a contiguous run of blocks
    // starting at fork_height+1, with branch[0].prev_hash == fork_hash
    // (a block already on our chain at fork_height). Adopts the branch
    // IFF its cumulative weight (audited plays) exceeds our current tip's.
    // Implemented as a try-and-rollback: rewrite the height→hash index to
    // the branch, rebuild_derived_state() to re-derive balances/indexes
    // deterministically, and if the result isn't actually heavier (or a
    // branch block fails validation) restore the previous chain. Returns
    // true only when the reorg succeeded and is now canonical. Safe to
    // call with a non-heavier branch — it just returns false.
    bool reorg_to_branch(const Hash256& fork_hash, uint32_t fork_height,
                         const std::vector<Block>& branch, std::string& err);

    // Cumulative audited-play weight at the current tip (fork weight).
    uint64_t tip_weight() const { return tip().weight; }

    // Retrieve block by hash.
    std::optional<Block> get_block(const Hash256& hash) const;

    // Retrieve block hash at given height.
    std::optional<Hash256> get_block_hash(uint32_t height) const;

    // Height of a block hash (or nullopt if unknown).
    std::optional<uint32_t> get_block_height(const Hash256& hash) const;

    // Full-block SHA256 stored at a given height (for peer checksum verification).
    std::optional<Hash256> get_block_full_hash(uint32_t height) const;

    // Validate a candidate block against current chain state.
    // Does NOT connect it.
    bool validate_block(const Block& block, std::string& error) const;

    // Validate a producer's candidate block (signed by the producer,
    // about to be co-signed by validators) WITHOUT enforcing prev_hash.
    // Used by the consensus path on follower nodes: a follower may not
    // yet have applied the producer's previous block via BlockPropagator
    // when the candidate broadcast arrives, so the strict prev_hash
    // gate in validate_block would reject the candidate and the
    // producer would time out waiting for confirmations even though
    // the block is structurally fine. The actual block-application
    // path (Chain::connect_block) still enforces prev_hash, so a
    // candidate that looks OK here but fails the strict check later
    // simply doesn't connect — and the producer's authoritative chain
    // wins via the longest-chain rule.
    bool validate_candidate(const Block& block, std::string& error) const;

    // Fast yes/no — is `content_hash` already on chain? Used by the
    // producer's re-queue logic so a duplicate-song registration gets
    // dropped instead of looping forever.
    bool validate_block_quick_duplicate(const Hash256& content_hash) const;

    // Rebuild all derived state by replaying all blocks.
    bool rebuild_derived_state();

    // Producer liveness helper. Trial-applies the given transaction list against
    // the CURRENT tip state — in a throwaway batch that is NEVER written — and
    // returns the index of the FIRST tx that fails to apply (nonce / balance /
    // authority), or -1 if every tx applies. Uses the exact same all-or-nothing
    // apply_transactions the real connect path runs, so its verdict matches
    // connect_block precisely. The producer uses it to isolate and evict a
    // single unappliable (e.g. flooded zero-balance) tx instead of letting it
    // re-enter every candidate and wedge the chain forever. Takes the chain
    // mutex; call OFF the connect path (no re-entrancy).
    int first_unappliable_tx(const std::vector<std::vector<uint8_t>>& txs);

    // Apply a transfer transaction (verifies signature + nonce, updates balances + nonce).
    // Called from apply_transactions() only. (The post_transfer API handler no
    // longer applies transfers directly — that mutated the ledger outside a
    // block and diverged nodes; transfers now flow through the flooded mempool
    // and land via block-apply like every other tx.)
    bool apply_transfer(const TransferTx& tx, leveldb::WriteBatch& batch);

    // Apply a mint transaction directly (credits outputs, updates song state).
    // Called from session_complete API handler and apply_transactions().
    bool apply_mint(const MintTx& mint, uint64_t play_count_before,
                    leveldb::WriteBatch& batch);

    // Apply a moderator op (GRANT/REVOKE). `height` is the chain
    // height the op is being applied at (== tip_.height + 1 when
    // called from connect_block) — recorded as the moderator's "active
    // since" so quorum logic in Phase 3 can ignore freshly-granted
    // seats.
    bool apply_moderator_op(const ModeratorOpTx& tx,
                            uint32_t height,
                            leveldb::WriteBatch& batch);

    // Apply a Phase-3 proposal or vote tx. Routes HIDE_CONTENT /
    // RELEASE_ESCROW / VOTE_YES sub-codes, tallies votes, and on
    // reaching majority quorum executes the action atomically within
    // the same block batch.
    bool apply_proposal(const ProposalTx& tx,
                        uint32_t height,
                        leveldb::WriteBatch& batch);

    // First-come-first-served username registration. Anyone signs for
    // their own address; chain checks (a) name well-formedness, (b)
    // not already taken, (c) nonce, (d) signature.
    bool apply_username_register(const UsernameTx& tx,
                                 leveldb::WriteBatch& batch);

    // Slashing — verifies the cryptographic claim inside the SlashTx
    // evidence (EquivocationProof or FingerprintForgeryProof), then
    // marks target_address as slashed in the validator registry. From
    // this point forward Confirmation messages signed by their key are
    // not counted toward block-finality quorum.
    bool apply_slash(const SlashTx& tx, leveldb::WriteBatch& batch);

    // Relay reward — issued by a full node to credit a mini-node for
    // serving binary tunnel traffic. Verifies the issuer signature,
    // verifies issuer is current chain founder (Phase 2; Phase 3 widens
    // to any validator), then mints count*1_00000000 internal units to
    // target_address. Advances issuer nonce.
    bool apply_relay_reward(const RelayRewardTx& tx,
                            leveldb::WriteBatch& batch);

    // Node authorization (Phase 2) — founder grants/revokes a validator in the
    // on-chain v: registry that check_play/validate_mint read. Founder-signed,
    // nonce-protected; node_id must equal sha256(node_pubkey).
    bool apply_node_auth(const NodeAuthTx& tx, leveldb::WriteBatch& batch);

    // Signed checkpoint (Phase 2) — founder pins {height, block_hash} into the
    // checkpoint set (persisted cp: + in-memory checkpoints_) so a costless
    // heavier fork can't rewrite history across it. Founder-signed, nonce-
    // protected.
    bool apply_checkpoint(const CheckpointTx& tx, leveldb::WriteBatch& batch);

    // Batched settlement mint (Phase 3) — recompute-authoritative. Authenticates
    // the serving validator, binds the committed Merkle root to the flooded
    // companion body (sb:), then walks constituents in canonical order and
    // credits exactly what committed state justifies (per-leaf skip on
    // used/underfunded/over-cap). Declares no amounts, so nothing can inflate.
    bool apply_settlement_mint(const SettlementMintTx& tx, leveldb::WriteBatch& batch);

    // Has this address been slashed? Used by the consensus path that
    // tallies confirmations — slashed addresses' votes return zero
    // weight. Reads the "slashed:" prefix in the DB.
    bool is_slashed(const Address& addr) const;

private:
    // Serializes every connect_block / disconnect_block / tip() read.
    // Producer thread + network thread + RPC thread can all call into
    // the chain concurrently; this mutex stops them tearing the tip_
    // updates and double-applying blocks.
    mutable std::mutex mu_;
    Database& db_;
    ChainTip  tip_{};

    // Effective checkpoint set (#7): seeded from hardcoded_checkpoints()
    // at construction, extended via add_config_checkpoints(). Keyed by
    // height so checkpoint_ok() is an O(log n) lookup. Empty today (no
    // audited mainnet height yet) → the gate is a no-op until populated.
    std::map<uint32_t, Hash256> checkpoints_;

    // Returns true unless `height` is a checkpoint AND `hash` differs from
    // the pinned value. A height with no checkpoint always passes. This is
    // the eclipse defense: even if every peer we see is the attacker, they
    // can't reproduce an audited hash without holding the real block.
    bool checkpoint_ok(uint32_t height, const Hash256& hash) const;

    // Votes recorded earlier in the *current* block but not yet
    // flushed to leveldb. We consult both this set and the persistent
    // votes when computing quorum, so a proposal and its first vote in
    // the same block see each other.
    std::map<Hash256, std::set<Address>> proposal_votes_in_block_;

    // Most-recently-applied nonce per address within the current block
    // (the value just written into the WriteBatch). Lets apply_*
    // functions handle "two txs from the same address in the same
    // block" without reading from the unflushed batch — the canonical
    // case is bootstrap, which emits GRANT (nonce 0) + UsernameTx
    // (nonce 1) for the same founder address back-to-back.
    std::map<Address, uint64_t> applied_nonce_in_block_;

    // Read the next-expected nonce for `addr` considering both the DB
    // and any nonces already advanced earlier in the current block.
    uint64_t next_expected_nonce(const Address& addr) const;
    void     record_applied_nonce(const Address& addr, uint64_t new_value);

    bool apply_transactions(const Block& block, uint32_t height,
                            leveldb::WriteBatch& batch);

    // Count YES votes for a proposal across both the on-disk record
    // and the in-block staging set, deduping addresses.
    size_t effective_vote_count(const Hash256& prop_hash) const;

    // True iff EVERY currently voting-eligible (level >= OP) moderator has
    // a YES vote recorded for `prop_hash` (on disk or staged in the current
    // block). Empty voter set => false (never vacuously unanimous). Used
    // only by GRANT_MODERATOR, which requires a unanimous moderator vote
    // rather than a simple majority. Re-evaluated against the *current*
    // moderator set on each apply (adding a mod mid-vote raises the bar;
    // a revoked mod's stale vote no longer counts).
    bool grant_is_unanimous(const Hash256& prop_hash) const;

    // Execute a HIDE/RELEASE/GRANT_MODERATOR proposal directly into
    // `batch` and flip its propstatus to EXECUTED. Returns false if the
    // action itself is invalid (e.g. release amount exceeds escrow
    // balance). `height` records the mod's active-since block on grant.
    bool execute_proposal(const ProposalTx& prop,
                          const Hash256& prop_hash,
                          uint32_t height,
                          leveldb::WriteBatch& batch);

    bool load_tip();
};

} // namespace mc
