#pragma once
#include "block.h"
#include "transaction.h"
#include "../storage/database.h"
#include <optional>
#include <string>

namespace mc {

struct ChainTip {
    Hash256  hash;
    uint32_t height;
};

// Manages the canonical blockchain: connect/disconnect blocks, tip tracking,
// state derivation (balances, fingerprint index, song state).
class Chain {
public:
    explicit Chain(Database& db);

    // Initialize from database; rebuilds derived state if necessary.
    bool init();

    // Returns current tip (height and hash).
    ChainTip tip() const { return tip_; }

    // Connect a validated block to the chain.
    // Updates tip, balances, song state, fingerprint index atomically.
    bool connect_block(const Block& block);

    // Disconnect the most recent block (for reorg support).
    bool disconnect_block();

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

    // Rebuild all derived state by replaying all blocks.
    bool rebuild_derived_state();

    // Apply a transfer transaction (verifies signature + nonce, updates balances + nonce).
    // Called from post_transfer API handler and apply_transactions().
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

private:
    Database& db_;
    ChainTip  tip_{};

    bool apply_transactions(const Block& block, uint32_t height,
                            leveldb::WriteBatch& batch);
    bool load_tip();
};

} // namespace mc
