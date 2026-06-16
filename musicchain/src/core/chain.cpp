#include "chain.h"
#include "../tokens/ledger.h"
#include "../tokens/mint.h"
#include <cstring>
#include <stdexcept>

namespace mc {

Chain::Chain(Database& db) : db_(db) {}

bool Chain::init() {
    return load_tip();
}

bool Chain::load_tip() {
    auto val = db_.get("t:tip");
    if (!val) {
        tip_ = {{}, 0};
        return true;
    }
    // format: 32 bytes hash + 4 bytes height
    if (val->size() < 36) return false;
    std::memcpy(tip_.hash.data(), val->data(), 32);
    tip_.height = 0;
    for (int i = 0; i < 4; ++i)
        tip_.height |= (static_cast<uint32_t>((*val)[32+i]) << (8*i));
    return true;
}

bool Chain::connect_block(const Block& block) {
    std::string err;
    if (!validate_block(block, err)) return false;

    leveldb::WriteBatch batch;
    auto serialized = block.serialize();
    auto hash       = block.hash();

    // Store block
    db_.put_batch(batch, "b:" + db_.hex(hash), serialized);
    // Height → hash
    uint32_t new_height = tip_.height + 1;
    db_.put_batch_u32(batch, "h:" + db_.hex(hash), new_height);
    // Index → hash
    db_.put_batch(batch, "n:" + std::to_string(new_height),
                  std::vector<uint8_t>(hash.begin(), hash.end()));

    // Store full-block checksum for peer verification
    Hash256 fh = Block::full_hash(serialized);
    db_.put_batch(batch, "k:" + std::to_string(new_height),
                  std::vector<uint8_t>(fh.begin(), fh.end()));

    // Update tip
    std::vector<uint8_t> tip_val(36);
    std::memcpy(tip_val.data(), hash.data(), 32);
    for (int i = 0; i < 4; ++i) tip_val[32+i] = (new_height >> (8*i)) & 0xFF;
    db_.put_batch(batch, "t:tip", tip_val);

    // Only song blocks populate the fingerprint / metadata / search
    // indexes. Heartbeat blocks (block.has_song == false) carry an
    // all-zero content_hash + empty fields; writing those would
    // corrupt the indexes and the duplicate-song guard.
    if (block.has_song) {
        db_.put_fingerprint(batch, block.song);
        db_.put_song_meta(batch, block.song.content_hash, block.song);
        db_.add_to_artist_index(batch, block.song.artist, block.song.content_hash);
        db_.add_to_genre_index(batch, block.song.genre, block.song.content_hash);
        db_.set_content_height(batch, block.song.content_hash, new_height);
    }

    // Apply transactions
    if (!apply_transactions(block, new_height, batch)) return false;

    if (!db_.write(batch)) return false;

    tip_.hash   = hash;
    tip_.height = new_height;
    return true;
}

bool Chain::apply_transfer(const TransferTx& tx, leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) return false;
    uint64_t expected = db_.get_nonce(tx.from_address);
    if (tx.nonce != expected) return false;
    Ledger ledger(db_);
    if (!ledger.transfer(batch, tx.from_address, tx.to_address, tx.amount)) return false;
    db_.set_nonce(batch, tx.from_address, expected + 1);
    return true;
}

bool Chain::apply_transactions(const Block& block, uint32_t height,
                               leveldb::WriteBatch& batch) {
    for (const auto& raw_tx : block.transactions) {
        if (raw_tx.empty()) continue;
        TxType type = static_cast<TxType>(raw_tx[0]);
        if (type == TxType::TRANSFER) {
            TransferTx tx;
            if (!TransferTx::deserialize(raw_tx.data(), raw_tx.size(), tx)) return false;
            if (!apply_transfer(tx, batch)) return false;
        } else if (type == TxType::MINT) {
            MintTx mint;
            if (!MintTx::deserialize(raw_tx.data(), raw_tx.size(), mint)) return false;
            // Get current play count before applying
            uint64_t play_count = db_.get_play_count(mint.proof.content_hash);
            if (!apply_mint(mint, play_count, batch)) return false;
        } else if (type == TxType::MODERATOR_OP) {
            ModeratorOpTx mod_tx;
            if (!ModeratorOpTx::deserialize(raw_tx.data(), raw_tx.size(), mod_tx)) return false;
            if (!apply_moderator_op(mod_tx, height, batch)) return false;
        }
    }
    return true;
}

bool Chain::apply_mint(const MintTx& mint, uint64_t play_count_before,
                       leveldb::WriteBatch& batch) {
    // Hard supply cap: refuse to credit a new mint that would push
    // total_supply at or past SUPPLY_CAP. This is the chain-frozen
    // state in the burn-rate curve — listeners trying to play past
    // this point fail at session.complete and no new tokens land.
    {
        uint64_t mint_total = 0;
        for (const auto& out : mint.outputs) mint_total += out.amount;
        uint64_t current_supply = db_.get_total_supply();
        if (mint_total > 0 && current_supply + mint_total > SUPPLY_CAP) {
            return false;
        }
    }
    // Burn tokens from player if applicable (post-10k plays + non-zero
    // burn rate from the current supply).
    if (mint.burn_amount > 0) {
        uint64_t bal = db_.get_balance(mint.proof.player_address);
        if (bal < mint.burn_amount) return false; // safety net: session_start already checked
        Ledger ledger(db_);
        ledger.debit(batch, mint.proof.player_address, mint.burn_amount);
        uint64_t supply = db_.get_total_supply();
        db_.set_total_supply(batch,
            supply >= mint.burn_amount ? supply - mint.burn_amount : 0);
    }

    // Mark session as used
    db_.put_batch(batch, "u:" + db_.hex(mint.proof.session_id), {});

    // Update song state
    db_.update_song_state(batch, mint.proof, play_count_before);

    // Credit outputs
    Ledger ledger(db_);
    for (const auto& out : mint.outputs) {
        ledger.credit(batch, out.recipient, out.amount);
    }
    return true;
}

bool Chain::apply_moderator_op(const ModeratorOpTx& tx,
                               uint32_t height,
                               leveldb::WriteBatch& batch) {
    // Every op must self-verify regardless of role. The proposer pubkey
    // is carried inline (see ModeratorOpTx::verify_signature for why we
    // cross-check it against `proposer`).
    if (!tx.verify_signature()) return false;

    // Per-proposer nonce. We reuse the same `nv:` table as transfers
    // since the address space is the same — the chain has a single
    // monotonic nonce per address regardless of which tx type advances
    // it.
    uint64_t expected = db_.get_nonce(tx.proposer);
    if (tx.nonce != expected) return false;

    const ModOpCode op = static_cast<ModOpCode>(tx.op_code);
    const ModLevel  lv = static_cast<ModLevel>(tx.level);

    auto founder = db_.get_founder();
    uint8_t proposer_level = db_.get_mod_level(tx.proposer);

    switch (op) {
        case ModOpCode::GRANT: {
            // Bootstrap path: no founder yet, the proposer signs for
            // themselves at FOUNDER level. This is the one and only
            // permitted self-grant on the entire chain — every other
            // grant must come from an existing FOUNDER (Phase 2) or a
            // majority of OPs (Phase 3, future work).
            const bool bootstrap = !founder.has_value()
                                 && lv == ModLevel::FOUNDER
                                 && std::memcmp(tx.proposer.data(),
                                                tx.subject.data(), 20) == 0;
            if (bootstrap) {
                db_.set_mod_level(batch, tx.subject,
                                  static_cast<uint8_t>(ModLevel::FOUNDER));
                db_.set_mod_pubkey(batch, tx.subject, tx.subject_pubkey);
                db_.set_mod_active_block(batch, tx.subject, height);
                db_.set_founder(batch, tx.subject);
                db_.set_nonce(batch, tx.proposer, expected + 1);
                return true;
            }
            // Non-bootstrap GRANT: founder must already exist, proposer
            // must be the founder, and the new level can't escalate to
            // FOUNDER (there is exactly one founder per chain).
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (lv == ModLevel::NONE || lv == ModLevel::FOUNDER) return false;
            // Granting to an existing founder is a no-op-with-bad-intent.
            if (std::memcmp(tx.subject.data(), founder->data(), 20) == 0) return false;
            db_.set_mod_level(batch, tx.subject, static_cast<uint8_t>(lv));
            db_.set_mod_pubkey(batch, tx.subject, tx.subject_pubkey);
            db_.set_mod_active_block(batch, tx.subject, height);
            db_.set_nonce(batch, tx.proposer, expected + 1);
            return true;
        }
        case ModOpCode::REVOKE: {
            // Founder is the only one with revoke power in Phase 2. The
            // founder can't revoke themselves — stepping down is a
            // separate flow we'll add when the multi-founder/transfer
            // case actually exists.
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (std::memcmp(tx.subject.data(), founder->data(), 20) == 0) return false;
            uint8_t current = db_.get_mod_level(tx.subject);
            if (current == 0) return false; // nothing to revoke
            db_.set_mod_level(batch, tx.subject, 0);
            db_.set_nonce(batch, tx.proposer, expected + 1);
            return true;
        }
    }
    return false;
}

std::optional<Block> Chain::get_block(const Hash256& hash) const {
    auto val = db_.get("b:" + db_.hex(hash));
    if (!val) return std::nullopt;
    Block block;
    if (!Block::deserialize(val->data(), val->size(), block)) return std::nullopt;
    return block;
}

std::optional<Hash256> Chain::get_block_hash(uint32_t height) const {
    auto val = db_.get("n:" + std::to_string(height));
    if (!val || val->size() < 32) return std::nullopt;
    Hash256 h;
    std::memcpy(h.data(), val->data(), 32);
    return h;
}

std::optional<uint32_t> Chain::get_block_height(const Hash256& hash) const {
    return db_.get_u32("h:" + db_.hex(hash));
}

bool Chain::validate_block(const Block& block, std::string& error) const {
    if (!block.validate()) { error = "block internal validation failed"; return false; }
    if (block.header.prev_hash != tip_.hash) {
        error = "prev_hash mismatch";
        return false;
    }
    // Reject duplicate songs (same content hash already in chain). Only
    // meaningful for song-bearing blocks; heartbeats carry zero hashes
    // by construction.
    if (block.has_song && db_.get_fingerprint(block.song.content_hash)) {
        error = "duplicate song";
        return false;
    }
    return true;
}

bool Chain::disconnect_block() {
    if (tip_.height == 0) return false;
    // Remove tip entries
    leveldb::WriteBatch batch;
    db_.del_batch(batch, "b:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "h:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "n:" + std::to_string(tip_.height));

    // Restore previous tip
    auto prev_hash = get_block_hash(tip_.height - 1);
    if (!prev_hash) {
        // Genesis revert
        db_.del_batch(batch, "t:tip");
        db_.write(batch);
        tip_ = {{}, 0};
        return true;
    }
    std::vector<uint8_t> tip_val(36);
    std::memcpy(tip_val.data(), prev_hash->data(), 32);
    uint32_t new_height = tip_.height - 1;
    for (int i = 0; i < 4; ++i) tip_val[32+i] = (new_height >> (8*i)) & 0xFF;
    db_.put_batch(batch, "t:tip", tip_val);
    db_.write(batch);
    tip_.hash   = *prev_hash;
    tip_.height = new_height;
    return true;
}

std::optional<Hash256> Chain::get_block_full_hash(uint32_t height) const {
    auto val = db_.get("k:" + std::to_string(height));
    if (!val || val->size() < 32) return std::nullopt;
    Hash256 h;
    std::memcpy(h.data(), val->data(), 32);
    return h;
}

bool Chain::rebuild_derived_state() {
    // Replay all blocks from height 1 to tip
    // This wipes and rebuilds balances, song states, fingerprint index
    db_.clear_derived_state();
    for (uint32_t h = 1; h <= tip_.height; ++h) {
        auto bhash = get_block_hash(h);
        if (!bhash) return false;
        auto block = get_block(*bhash);
        if (!block) return false;
        leveldb::WriteBatch batch;
        db_.put_fingerprint(batch, block->song);
        db_.put_song_meta(batch, block->song.content_hash, block->song);
        db_.add_to_artist_index(batch, block->song.artist, block->song.content_hash);
        db_.add_to_genre_index(batch, block->song.genre, block->song.content_hash);
        apply_transactions(*block, h, batch);
        db_.write(batch);
    }
    return true;
}

} // namespace mc
