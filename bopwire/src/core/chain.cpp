#include "chain.h"
#include "../consensus/slashing.h"    // EquivocationProof / FingerprintForgeryProof
#include "../audio/fingerprint.h"     // chromaprint similarity on replay
#include "../tokens/ledger.h"
#include "../tokens/mint.h"
#include "merkle.h"                    // merkle_root_bytes (settlement body root)
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include "../crypto/keys.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace mc {

namespace {
// Byte length of a serialized accumulator vector (1024 lanes * 2 bytes).
constexpr size_t kAccVecBytes = 1024 * 2;

// RAII: activate a state accumulator for a scope and ALWAYS clear it on exit,
// including on an exception thrown by a state writer — so Database::acc_ can
// never be left dangling at a destroyed stack accumulator (audit #17).
struct AccGuard {
    Database& db;
    AccGuard(Database& d, StateAccumulator* a) : db(d) { db.set_accumulator(a); }
    ~AccGuard() { db.set_accumulator(nullptr); }
    AccGuard(const AccGuard&) = delete;
    AccGuard& operator=(const AccGuard&) = delete;
};
} // namespace

Chain::Chain(Database& db) : db_(db), ledger_(db_) {
    // Seed the effective checkpoint set from the baked-in list (#7).
    for (const auto& c : hardcoded_checkpoints())
        checkpoints_[c.height] = c.hash;
    // Phase 2: restore founder-signed checkpoints persisted under cp:<height>
    // by apply_checkpoint, so the finality pins survive a restart.
    db_.for_each_with_prefix("cp:", [this](const std::string& key,
                                           const std::string& val) {
        if (val.size() != 32) return true;
        try {
            uint32_t h = static_cast<uint32_t>(std::stoul(key.substr(3)));
            Hash256 hash{};
            std::memcpy(hash.data(), val.data(), 32);
            checkpoints_[h] = hash;
        } catch (...) { /* skip malformed cp: key */ }
        return true;
    });
    // P4: restore the committed state_root accumulator. A valid sr:vec is
    // EXACTLY kAccVecBytes; anything shorter (absent, truncated, torn write) is
    // NOT silently treated as the zero/empty root — that would leave a non-empty
    // ledger paired with a zero accumulator and permanently wedge every
    // subsequent block at the state_root gate (audit #11). Instead re-derive it
    // from the persisted state via an order-independent scan and persist it. On
    // a genuinely fresh chain the scan sees no state leaves and returns the same
    // zero vector, so this is correct in both cases. (A torn *reorg* — where the
    // state itself is inconsistent — is separately caught by the sr:dirty marker
    // in init(), which triggers a full replay-and-gate rebuild.)
    working_acc_.db = &db_;
    auto v = db_.get("sr:vec");
    if (v && v->size() == kAccVecBytes) {
        committed_acc_ = StateAccumulator::deserialize_vec(*v);
    } else {
        committed_acc_ = db_.scan_state_accumulator();
        db_.put("sr:vec", StateAccumulator::serialize_vec(committed_acc_));
    }
}

bool Chain::checkpoint_ok(uint32_t height, const Hash256& hash) const {
    auto it = checkpoints_.find(height);
    if (it == checkpoints_.end()) return true;   // no checkpoint at this height
    return it->second == hash;
}

size_t Chain::add_config_checkpoints(const std::vector<Checkpoint>& cps) {
    // Startup-only, single-threaded by contract (before init()/start()).
    for (const auto& c : cps) checkpoints_[c.height] = c.hash;  // override on collision
    return cps.size();
}

namespace {
// Fork weight (#8) contribution of a block = number of MintTx
// (play-reward mints) it carries. One MintTx == one realtime, audited,
// device-gated play, so summing this across the chain gives "cumulative
// audited plays." Heartbeat / registration-only blocks contribute 0.
uint64_t count_plays(const Block& block) {
    uint64_t n = 0;
    for (const auto& tx : block.transactions) {
        if (tx.empty()) continue;
        const TxType t = static_cast<TxType>(tx[0]);
        if (t == TxType::MINT) {
            ++n;                                   // one play
        } else if (t == TxType::SETTLEMENT_MINT) {
            // A batched settlement contributes its constituent count, HARD-CAPPED
            // so a padded settlement can't buy cheap fork weight; combined with
            // the Phase-2 signed checkpoints + Phase-0 FINALITY_DEPTH this bounds
            // the costless-fork attack. (Counting DECLARED constituents rather
            // than re-verified ones is a documented simplification — the block
            // only connects if apply_settlement_mint matched the body/merkle.)
            SettlementMintTx sm;
            if (SettlementMintTx::deserialize(tx.data(), tx.size(), sm)) {
                uint32_t c = sm.constituent_count;
                if (c > MAX_CONSTITUENTS_PER_SETTLEMENT)
                    c = MAX_CONSTITUENTS_PER_SETTLEMENT;
                n += c;
            }
        }
    }
    return n;
}
} // namespace

bool Chain::init() {
    if (!load_tip()) return false;
    // Self-heal an interrupted reorg/rebuild. The sr:dirty marker is set before
    // the derived state is torn down and cleared only after the rebuild finishes
    // and sr:vec is repersisted, so if it survives a restart the committed state,
    // sr:vec, and the block store may be mutually inconsistent (a torn reorg that
    // already moved t:tip). A full replay-and-gate rebuild is the only safe
    // recovery; run it before serving or producing any block.
    if (tip_.height > 0 && db_.get("sr:dirty")) {
        std::cerr << "[chain] init: sr:dirty set — recovering from an interrupted "
                     "reorg/rebuild by re-deriving state before serving\n";
        rebuild_derived_state();
    }
    return true;
}

bool Chain::load_tip() {
    auto val = db_.get("t:tip");
    if (!val) {
        tip_ = {};
        return true;
    }
    // format: 32 bytes hash + 4 bytes height
    if (val->size() < 36) return false;
    std::memcpy(tip_.hash.data(), val->data(), 32);
    tip_.height = 0;
    for (int i = 0; i < 4; ++i)
        tip_.height |= (static_cast<uint32_t>((*val)[32+i]) << (8*i));
    // timestamp_ms isn't stored in t:tip — recover it from the block we
    // just pointed at. Needed so the fork-choice rule has a real value
    // to compare against immediately after startup instead of zero.
    if (auto tb = get_block(tip_.hash))
        tip_.timestamp_ms = tb->header.timestamp_ms;
    // Cumulative fork weight (#8) — recover from the per-block "cw:" index.
    tip_.weight = db_.get_u64("cw:" + db_.hex(tip_.hash)).value_or(0);
    return true;
}

bool Chain::connect_block(const Block& block) {
    std::lock_guard<std::mutex> lk(mu_);
    // validate_block reads tip_ and the DB; called under the same lock
    // so the producer thread and network thread can't race past the
    // prev_hash check.
    std::string err;
    if (!validate_block(block, err)) {
        std::cerr << "[chain] connect_block: validate failed: " << err << "\n";
        return false;
    }

    leveldb::WriteBatch batch;
    auto serialized = block.serialize();
    auto hash       = block.hash();

    // Store block
    db_.put_batch(batch, "b:" + db_.hex(hash), serialized);
    // Height → hash
    uint32_t new_height = tip_.height + 1;
    // Eclipse defense (#7): if a checkpoint pins this height, the block we
    // are about to accept MUST carry the pinned hash. No-op at heights with
    // no checkpoint, so this is inert until the list is populated.
    if (!checkpoint_ok(new_height, hash)) {
        std::cerr << "[chain] connect_block: checkpoint mismatch at height "
                  << new_height << " — refusing fork\n";
        return false;
    }
    db_.put_batch_u32(batch, "h:" + db_.hex(hash), new_height);
    // Cumulative fork weight (#8): parent weight + plays in this block.
    uint64_t new_weight = tip_.weight + count_plays(block);
    db_.put_batch_u64(batch, "cw:" + db_.hex(hash), new_weight);
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

    // P4: from here on, STATE writes (song index + apply_transactions) feed the
    // working accumulator seeded from the tip's committed root. The AccGuard
    // clears acc_ on every scope exit — including an exception thrown by a
    // writer (#17). Seeded AFTER the early-return checkpoint check so a rejected
    // block never leaves acc_ dangling.
    working_acc_.copy_from(committed_acc_);
    working_acc_.db = &db_;
    bool tx_ok;
    {
        AccGuard g(db_, &working_acc_);
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
            // The song has landed — drop its pending-song mempool row (sp:) in the
            // SAME batch as the block writes, so a restart / rejoin never
            // re-enqueues or re-floods a song that is already on chain. Runs for
            // both self-minted and peer-adopted blocks (every node calls
            // connect_block), so sp: converges to "not yet on chain" network-wide.
            db_.del_batch(batch, "sp:" + db_.hex(block.song.content_hash));
        }

        // Apply transactions
        tx_ok = apply_transactions(block, new_height, batch);
    }   // P4: acc_ cleared here — root now reflects post-apply state
    if (!tx_ok) {
        std::cerr << "[chain] connect_block: apply_transactions failed at height "
                  << new_height << " with " << block.transactions.size()
                  << " tx\n";
        return false;
    }
    // P4: gate the committed state_root, then persist the accumulator in the SAME
    // batch so it falls or commits atomically with the block.
    if (block.header.version >= 4 && working_acc_.root() != block.header.state_root) {
        std::cerr << "[chain] connect_block: state_root mismatch at height "
                  << new_height << "\n";
        return false;
    }
    db_.put_batch(batch, "sr:vec", StateAccumulator::serialize_vec(working_acc_.vec));

    // Bug fix #6: drain the same txs from the mempool in the SAME batch
    // as the block-state writes. Used to be a separate
    // db.del_pending_tx() call from candidate.cpp after connect_block
    // returned, which left a crash window where the tx had been applied
    // but stayed in the mempool — next startup would try to re-apply
    // and trip the nonce check, wedging the chain. Now they fall
    // together or not at all.
    for (const auto& raw_tx : block.transactions) {
        if (raw_tx.empty()) continue;
        auto th = crypto::sha256(raw_tx.data(), raw_tx.size());
        db_.del_batch(batch, "p:" + db_.hex(th));
        db_.del_batch(batch, "pt:" + db_.hex(th));   // paired submit_ms side-index
    }

    if (!db_.write(batch)) return false;
    committed_acc_ = working_acc_.vec;   // P4: promote the block's committed state_root

    // M2: the batch committed — now (and only now) promote any checkpoint pins
    // this block carried into the live set the fork-choice reads.
    for (const auto& [h, hh] : pending_checkpoints_) checkpoints_[h] = hh;
    pending_checkpoints_.clear();

    tip_.hash         = hash;
    tip_.height       = new_height;
    tip_.timestamp_ms = block.header.timestamp_ms;
    tip_.weight       = new_weight;
    return true;
}

Hash256 Chain::compute_candidate_state_root(const Block& block) {
    std::lock_guard<std::mutex> lk(mu_);
    // Trial-apply this block's STATE writes against a COPY of the committed
    // accumulator, into a THROWAWAY batch, and return the resulting root. Mirrors
    // exactly the state writes connect_block performs (song index + txs); the
    // accumulator hook filters non-state keys. This is the committed root the
    // producer stamps into the header, which connect_block then re-derives and
    // asserts. The trial mutates ledger_/staging, but connect_block's
    // apply_transactions resets them before the real apply.
    StateAccumulator acc;
    acc.copy_from(committed_acc_);
    acc.db = &db_;
    leveldb::WriteBatch throwaway;
    const uint32_t h = tip_.height + 1;
    {
        // AccGuard clears acc_ on every exit; critical here because `acc` is a
        // STACK local — a throw from a writer between set and reset would
        // otherwise leave Database::acc_ pointing at freed stack memory (#17).
        AccGuard g(db_, &acc);
        if (block.has_song) {
            db_.put_fingerprint(throwaway, block.song);
            db_.put_song_meta(throwaway, block.song.content_hash, block.song);
            db_.add_to_artist_index(throwaway, block.song.artist, block.song.content_hash);
            db_.add_to_genre_index(throwaway, block.song.genre, block.song.content_hash);
            db_.set_content_height(throwaway, block.song.content_hash, h);
        }
        apply_transactions(block, h, throwaway);
    }
    return acc.root();
}

bool Chain::apply_transfer(const TransferTx& tx, leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) return false;
    uint64_t expected = next_expected_nonce(tx.from_address);
    if (tx.nonce != expected) return false;
    Ledger& ledger = ledger_;   // C2: shared block-scoped overlay
    if (!ledger.transfer(batch, tx.from_address, tx.to_address, tx.amount)) return false;
    db_.set_nonce(batch, tx.from_address, expected + 1);
    record_applied_nonce(tx.from_address, expected + 1);
    return true;
}

uint64_t Chain::next_expected_nonce(const Address& addr) const {
    // applied_nonce_in_block_[addr] holds the NEXT-expected nonce that
    // matches what db_.get_nonce(addr) WILL return once the current
    // batch flushes. So we just return it directly (no +1) — the value
    // already represents "the nonce the next tx from this address must
    // present."
    auto it = applied_nonce_in_block_.find(addr);
    if (it != applied_nonce_in_block_.end()) return it->second;
    return db_.get_nonce(addr);
}

void Chain::record_applied_nonce(const Address& addr, uint64_t new_value) {
    // Store the SAME value we just passed to db_.set_nonce(...,
    // new_value). Subsequent next_expected_nonce calls in the same
    // block will see this and treat it as the new floor.
    applied_nonce_in_block_[addr] = new_value;
}

bool Chain::apply_transactions(const Block& block, uint32_t height,
                               leveldb::WriteBatch& batch) {
    // Reset the per-block vote staging set so quorum math only counts
    // votes from this block once they're explicitly recorded.
    proposal_votes_in_block_.clear();
    applied_nonce_in_block_.clear();
    sessions_used_in_block_.clear();
    pending_checkpoints_.clear();   // M2: staged pins, merged only post-commit
    ledger_.reset();   // C2: fresh block-scoped balance/supply overlay

    for (const auto& raw_tx : block.transactions) {
        if (raw_tx.empty()) continue;
        TxType type = static_cast<TxType>(raw_tx[0]);
        if (type == TxType::TRANSFER) {
            TransferTx tx;
            if (!TransferTx::deserialize(raw_tx.data(), raw_tx.size(), tx)) {
                std::cerr << "[chain] apply: TRANSFER deserialize failed\n"; return false;
            }
            if (!apply_transfer(tx, batch)) {
                std::cerr << "[chain] apply: TRANSFER apply failed (nonce/sig/balance)\n"; return false;
            }
        } else if (type == TxType::MINT) {
            MintTx mint;
            if (!MintTx::deserialize(raw_tx.data(), raw_tx.size(), mint)) {
                std::cerr << "[chain] apply: MINT deserialize failed\n"; return false;
            }
            // Forge gate (Phase 1): a MINT carried in a block must pass the SAME
            // validation the mempool preflight runs — serving-node signature vs
            // the v: registry, session unused, and declared outputs+burn ==
            // recompute_mint from on-chain song/supply — or the whole block
            // fails to connect. Without this a peer could inject a MINT crediting
            // arbitrary tokens to itself and every node would apply it.
            {
                std::string mint_err;
                if (!validate_mint(mint, db_, mint_err)) {
                    std::cerr << "[chain] apply: MINT validation failed: " << mint_err << "\n";
                    return false;
                }
            }
            // C1: consume the session at most once per block. is_session_used
            // (inside validate_mint) only sees COMMITTED u: markers, not this
            // block's staged ones, so [MINT,MINT] for one signed proof would
            // otherwise credit twice — deterministic supply inflation.
            if (!sessions_used_in_block_.insert(mint.proof.session_id).second) {
                std::cerr << "[chain] apply: duplicate session in block (MINT)\n"; return false;
            }
            uint64_t play_count = db_.get_play_count(mint.proof.content_hash);
            if (!apply_mint(mint, play_count, batch)) {
                std::cerr << "[chain] apply: MINT apply failed\n"; return false;
            }
        } else if (type == TxType::MODERATOR_OP) {
            ModeratorOpTx mod_tx;
            if (!ModeratorOpTx::deserialize(raw_tx.data(), raw_tx.size(), mod_tx)) {
                std::cerr << "[chain] apply: MODERATOR_OP deserialize failed\n"; return false;
            }
            if (!apply_moderator_op(mod_tx, height, batch)) {
                std::cerr << "[chain] apply: MODERATOR_OP apply failed (op_code="
                          << static_cast<int>(mod_tx.op_code) << " level="
                          << static_cast<int>(mod_tx.level) << " nonce="
                          << mod_tx.nonce << ")\n"; return false;
            }
        } else if (type == TxType::MODERATOR_PROPOSAL) {
            ProposalTx prop_tx;
            if (!ProposalTx::deserialize(raw_tx.data(), raw_tx.size(), prop_tx)) {
                std::cerr << "[chain] apply: PROPOSAL deserialize failed\n"; return false;
            }
            if (!apply_proposal(prop_tx, height, batch)) {
                std::cerr << "[chain] apply: PROPOSAL apply failed\n"; return false;
            }
        } else if (type == TxType::USERNAME_REGISTER) {
            UsernameTx un_tx;
            if (!UsernameTx::deserialize(raw_tx.data(), raw_tx.size(), un_tx)) {
                std::cerr << "[chain] apply: USERNAME_REGISTER deserialize failed\n"; return false;
            }
            if (!apply_username_register(un_tx, batch)) {
                std::cerr << "[chain] apply: USERNAME_REGISTER apply failed (name='"
                          << un_tx.name << "')\n"; return false;
            }
        } else if (type == TxType::SLASH) {
            SlashTx s_tx;
            if (!SlashTx::deserialize(raw_tx.data(), raw_tx.size(), s_tx)) {
                std::cerr << "[chain] apply: SLASH deserialize failed\n"; return false;
            }
            if (!apply_slash(s_tx, batch)) {
                std::cerr << "[chain] apply: SLASH apply failed\n"; return false;
            }
        } else if (type == TxType::RELAY_REWARD) {
            RelayRewardTx rr;
            if (!RelayRewardTx::deserialize(raw_tx.data(), raw_tx.size(), rr)) {
                std::cerr << "[chain] apply: RELAY_REWARD deserialize failed\n"; return false;
            }
            if (!apply_relay_reward(rr, batch)) {
                std::cerr << "[chain] apply: RELAY_REWARD apply failed\n"; return false;
            }
        } else if (type == TxType::NODE_AUTH) {
            NodeAuthTx na;
            if (!NodeAuthTx::deserialize(raw_tx.data(), raw_tx.size(), na)) {
                std::cerr << "[chain] apply: NODE_AUTH deserialize failed\n"; return false;
            }
            if (!apply_node_auth(na, batch)) {
                std::cerr << "[chain] apply: NODE_AUTH apply failed\n"; return false;
            }
        } else if (type == TxType::CHECKPOINT) {
            CheckpointTx cp;
            if (!CheckpointTx::deserialize(raw_tx.data(), raw_tx.size(), cp)) {
                std::cerr << "[chain] apply: CHECKPOINT deserialize failed\n"; return false;
            }
            if (!apply_checkpoint(cp, batch)) {
                std::cerr << "[chain] apply: CHECKPOINT apply failed\n"; return false;
            }
        } else if (type == TxType::SETTLEMENT_MINT) {
            SettlementMintTx sm;
            if (!SettlementMintTx::deserialize(raw_tx.data(), raw_tx.size(), sm)) {
                std::cerr << "[chain] apply: SETTLEMENT_MINT deserialize failed\n"; return false;
            }
            if (!apply_settlement_mint(sm, batch)) {
                std::cerr << "[chain] apply: SETTLEMENT_MINT apply failed\n"; return false;
            }
        } else {
            std::cerr << "[chain] apply: unknown TxType " << static_cast<int>(type) << "\n";
            return false;
        }
    }
    return true;
}

bool Chain::apply_mint(const MintTx& mint, uint64_t play_count_before,
                       leveldb::WriteBatch& batch) {
    Ledger& ledger = ledger_;   // C2: shared block-scoped overlay

    // BUG FIX: reject mints where any output recipient is the zero
    // address. compute_mint_outputs already skips zero-recipient
    // discoverer credits, but explicit defense in depth here means
    // a corrupted MintTx replayed via the block-validation path
    // can't accidentally credit address 0 either.
    const Address zero_addr{};
    for (const auto& out : mint.outputs) {
        if (out.recipient == zero_addr) return false;
    }

    // Hard supply cap: refuse to credit a new mint that would push
    // total_supply at or past SUPPLY_CAP. This is the chain-frozen
    // state in the burn-rate curve — listeners trying to play past
    // this point fail at session.complete and no new tokens land.
    {
        uint64_t mint_total = 0;
        for (const auto& out : mint.outputs) mint_total += out.amount;
        uint64_t current_supply = ledger.total_supply();   // C2: overlay-aware
        if (mint_total > 0 && current_supply + mint_total > SUPPLY_CAP) {
            return false;
        }
    }
    // Burn tokens from player if applicable (post-10k plays + non-zero
    // burn rate from the current supply).
    if (mint.burn_amount > 0) {
        uint64_t bal = ledger.balance(mint.proof.player_address);   // C2: overlay-aware
        if (bal < mint.burn_amount) return false; // safety net: session_start already checked
        ledger.debit(batch, mint.proof.player_address, mint.burn_amount);
        ledger.decrement_supply(batch, mint.burn_amount);          // C2: overlay-aware
    }

    // BUG FIX: replay protection moved BEFORE the credit so a tx
    // that gets re-applied (block reorg, manual replay) on a
    // session_id we already minted on does not double-credit.
    // is_session_used is checked at session.complete entry; this
    // is the persistence step that makes the check stick.
    db_.put_batch(batch, "u:" + db_.hex(mint.proof.session_id), {});

    // Update song state (play_count++, discoverer on first play).
    db_.update_song_state(batch, mint.proof, play_count_before);

    // BUG FIX: previously this loop called ledger.credit() per
    // output. Each call read the OLD balance from disk so two
    // outputs to the same address (or the simultaneous artist-
    // escrow + node-share to a wallet that happens to own both)
    // would silently lose one credit. credit_many aggregates per-
    // address amounts and updates total_supply with the sum once.
    std::vector<std::pair<Address, uint64_t>> outs;
    outs.reserve(mint.outputs.size());
    for (const auto& out : mint.outputs) {
        outs.emplace_back(out.recipient, out.amount);
    }
    ledger.credit_many(batch, outs);
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
    uint64_t expected = next_expected_nonce(tx.proposer);
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
            // The subject must also match the pinned founder address when
            // one is baked in (chain.h). This is what makes the founder
            // un-re-bootstrappable: even in the empty-chain window after a
            // wipe, only the holder of the pinned wallet can claim it.
            // Unpinned (all-zero) => dev/test fallback: first self-grant wins.
            const bool bootstrap = !founder.has_value()
                                 && lv == ModLevel::FOUNDER
                                 && std::memcmp(tx.proposer.data(),
                                                tx.subject.data(), 20) == 0
                                 && (!founder_is_pinned()
                                     || std::memcmp(tx.subject.data(),
                                                    PINNED_FOUNDER_ADDRESS.data(),
                                                    20) == 0);
            if (bootstrap) {
                db_.set_mod_level(batch, tx.subject,
                                  static_cast<uint8_t>(ModLevel::FOUNDER));
                db_.set_mod_pubkey(batch, tx.subject, tx.subject_pubkey);
                db_.set_mod_active_block(batch, tx.subject, height);
                db_.set_founder(batch, tx.subject);
                db_.set_nonce(batch, tx.proposer, expected + 1);
                record_applied_nonce(tx.proposer, expected + 1);
                return true;
            }
            // Non-bootstrap GRANT via MODERATOR_OP is DISABLED. Every real
            // moderator grant now goes through a GRANT_MODERATOR proposal
            // that requires a unanimous vote of all current moderators
            // (see apply_proposal). This removes the founder's ability to
            // unilaterally appoint moderators — the whole point of the
            // unanimous-vote model. (When the founder is the sole mod,
            // unanimous == the founder alone, so bootstrapping the first
            // additional moderator still works.) REVOKE / TAG_LABEL_EDIT
            // are unchanged.
            return false;
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
            record_applied_nonce(tx.proposer, expected + 1);   // #18: match every
            return true;                                       // other apply path
        }
        case ModOpCode::TAG_LABEL_EDIT: {
            // Founder-only metadata edit. The "action" field in the
            // JSON payload picks the sub-op:
            //
            //   {"action":"label_define","name":"…","splits":[
            //       {"addr":"0x…","bp": 7000}, {"addr":"0x…","bp": 3000}]}
            //
            //   {"action":"label_assign","artist":"0x…","label":"…"}
            //
            // Returns false on any malformed payload, unknown action,
            // or splits that don't sum to 10 000 bp.
            if (!founder.has_value()) return false;
            if (proposer_level != static_cast<uint8_t>(ModLevel::FOUNDER)) return false;
            if (tx.meta_json.empty()) return false;
            try {
                auto j = nlohmann::json::parse(tx.meta_json);
                const std::string action = j.value("action", std::string());
                if (action == "label_define") {
                    const std::string name = j.value("name", std::string());
                    if (name.empty() || name.size() > 64) return false;
                    Database::LabelDef def;
                    def.display_name = name;
                    if (!j.contains("splits") || !j["splits"].is_array()) return false;
                    uint32_t total_bp = 0;
                    for (const auto& s : j["splits"]) {
                        Database::LabelSplit ls;
                        const std::string addr_hex = s.value("addr", std::string());
                        if (!crypto::parse_address(addr_hex, ls.wallet)) return false;
                        int bp = s.value("bp", 0);
                        if (bp <= 0 || bp > 10000) return false;
                        ls.basis_points = static_cast<uint16_t>(bp);
                        total_bp += bp;
                        def.splits.push_back(ls);
                    }
                    if (total_bp != 10000) return false;
                    if (def.splits.empty() || def.splits.size() > 16) return false;
                    db_.set_label(batch, name, def);
                } else if (action == "label_assign") {
                    Address artist{};
                    const std::string addr_hex = j.value("artist", std::string());
                    if (!crypto::parse_address(addr_hex, artist)) return false;
                    const std::string label = j.value("label", std::string());
                    // Empty label clears the assignment.
                    if (!label.empty()) {
                        auto def = db_.get_label(label);
                        if (!def.has_value()) return false; // can't assign to nonexistent label
                    }
                    db_.assign_artist_label(batch, artist, label);
                } else {
                    return false;
                }
            } catch (const std::exception&) {
                return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);   // #18: so a 2nd
            return true;                                        // op/block applies
        }
    }
    return false;
}

// ---- Username registration (Phase 3.5) ------------------------------

namespace {
bool username_is_well_formed(const std::string& s) {
    if (s.size() < 3 || s.size() > 30) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false; // must start with letter
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_';
        if (!ok) return false;
    }
    return true;
}
} // namespace

bool Chain::apply_username_register(const UsernameTx& tx,
                                    leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: verify_signature\n"; return false;
    }
    uint64_t expected = next_expected_nonce(tx.owner);
    if (tx.nonce != expected) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: nonce mismatch (tx=" << tx.nonce
                  << " expected=" << expected << ")\n"; return false;
    }
    if (!username_is_well_formed(tx.name)) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: not well-formed\n"; return false;
    }
    if (db_.username_taken(tx.name)) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: already taken\n"; return false;
    }
    if (db_.get_addr_username(tx.owner).has_value()) {
        std::cerr << "[chain] username '" << tx.name
                  << "' reject: owner already has a username\n"; return false;
    }
    db_.set_username(batch, tx.name, tx.owner);
    db_.set_nonce(batch, tx.owner, expected + 1);
    record_applied_nonce(tx.owner, expected + 1);
    return true;
}

// ---- Relay reward ---------------------------------------------------

bool Chain::apply_relay_reward(const RelayRewardTx& tx,
                               leveldb::WriteBatch& batch) {
    // Verify the issuer's signature first — the issuer is the only one
    // who can authorize a credit.
    if (!tx.verify_signature()) {
        std::cerr << "[chain] relay_reward reject: issuer signature invalid\n";
        return false;
    }
    // Issuer must be the founder for now. Phase 3 widens this to any
    // active validator with a non-slashed key.
    auto founder = db_.get_founder();
    if (!founder.has_value() ||
        std::memcmp(founder->data(), tx.issuer_address.data(), 20) != 0) {
        std::cerr << "[chain] relay_reward reject: issuer is not the founder\n";
        return false;
    }
    // Nonce check against issuer's chain nonce.
    uint64_t expected = next_expected_nonce(tx.issuer_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] relay_reward reject: nonce mismatch (tx="
                  << tx.nonce << " expected=" << expected << ")\n";
        return false;
    }
    // (#10) tx.count is now INTERNAL UNITS (1 MC = 1e8 units), pre-computed
    // by the relay tracker as corroborated_bytes × 10 (1 MC per 10 MB).
    // Bound the per-tx credit so a buggy counter can't mint a fortune in
    // one shot — matches the tracker's pre-clamp (kMaxCountPerTx there).
    constexpr uint64_t kMaxUnitsPerTx = 1'000'000'000'000ull;  // 10,000 MC
    if (tx.count == 0 || tx.count > kMaxUnitsPerTx) {
        std::cerr << "[chain] relay_reward reject: units out of range ("
                  << tx.count << ")\n";
        return false;
    }
    const uint64_t amount = tx.count;   // credit units directly (no ×1e8)
    // Hard supply cap — Ledger::credit bumps total_supply, so relay rewards
    // inflate supply; reject rather than overshoot SUPPLY_CAP.
    {
        uint64_t current_supply = ledger_.total_supply();   // C2: overlay-aware
        if (current_supply + amount > SUPPLY_CAP) {
            std::cerr << "[chain] relay_reward reject: would exceed SUPPLY_CAP\n";
            return false;
        }
    }
    Ledger& ledger = ledger_;   // C2: shared block-scoped overlay
    ledger.credit(batch, tx.target_address, amount);
    db_.set_nonce(batch, tx.issuer_address, expected + 1);
    record_applied_nonce(tx.issuer_address, expected + 1);
    std::cout << "[chain] RELAY_REWARD: +" << amount << " units ("
              << (amount / 100'000'000ull) << "." << ((amount / 1'000'000ull) % 100)
              << " MC) to " << db_.hex(tx.target_address).substr(0, 12) << "…\n";
    return true;
}

// ---- Node authorization (Phase 2: permissioned v: registry) ---------

bool Chain::apply_node_auth(const NodeAuthTx& tx, leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) {
        std::cerr << "[chain] node_auth reject: issuer signature invalid\n"; return false;
    }
    // Only the founder may grant/revoke validators (Phase 2). Later a
    // moderator-quorum path can widen this — same shape as escrow release.
    auto founder = db_.get_founder();
    if (!founder.has_value() ||
        std::memcmp(founder->data(), tx.issuer_address.data(), 20) != 0) {
        std::cerr << "[chain] node_auth reject: issuer is not the founder\n"; return false;
    }
    uint64_t expected = next_expected_nonce(tx.issuer_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] node_auth reject: nonce mismatch\n"; return false;
    }
    // node_id MUST equal sha256(node_pubkey) — the identity is derived from the
    // key, so a grant can't bind an attacker key to a victim's node_id, and
    // check_play's v:[serving_node_id] lookup gets exactly the signing key.
    Hash256 derived_id = crypto::sha256(tx.node_pubkey.data(), 33);
    if (std::memcmp(derived_id.data(), tx.node_id.data(), 32) != 0) {
        std::cerr << "[chain] node_auth reject: node_id != sha256(pubkey)\n"; return false;
    }
    const std::string key  = "v:"  + db_.hex(tx.node_id);
    const std::string akey = "va:" + db_.hex(tx.node_id);  // ON-CHAIN-authorized marker
    if (tx.authorize) {
        db_.put_batch(batch, key,
                      std::vector<uint8_t>(tx.node_pubkey.begin(), tx.node_pubkey.end()));
        db_.put_batch(batch, akey, {});   // lets the founder emitter stop re-issuing (C3)
    } else {
        db_.del_batch(batch, key);    // via del_batch so the state_root accumulator sees it
        db_.del_batch(batch, akey);
    }
    db_.set_nonce(batch, tx.issuer_address, expected + 1);
    record_applied_nonce(tx.issuer_address, expected + 1);
    std::cout << "[chain] NODE_AUTH: " << (tx.authorize ? "grant" : "revoke")
              << " v:" << db_.hex(tx.node_id).substr(0, 12) << "…\n";
    return true;
}

// ---- Signed checkpoint (Phase 2: finality pin) ----------------------

bool Chain::apply_checkpoint(const CheckpointTx& tx, leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) {
        std::cerr << "[chain] checkpoint reject: issuer signature invalid\n"; return false;
    }
    auto founder = db_.get_founder();
    if (!founder.has_value() ||
        std::memcmp(founder->data(), tx.issuer_address.data(), 20) != 0) {
        std::cerr << "[chain] checkpoint reject: issuer is not the founder\n"; return false;
    }
    uint64_t expected = next_expected_nonce(tx.issuer_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] checkpoint reject: nonce mismatch\n"; return false;
    }
    // Pin the checkpoint. Persisted (cp:) so it survives restart, and mirrored
    // into the in-memory map that checkpoint_ok()/reorg_to_branch read. (A block
    // that fails to fully apply after this point leaves a stale in-memory pin
    // until restart re-derives checkpoints_ from cp:; harmless for the founder-
    // only, producer-pre-validated checkpoint path — flagged for the adversarial
    // pass.)
    pending_checkpoints_[tx.height] = tx.block_hash;   // M2: merged post-commit
    db_.put_batch(batch, "cp:" + std::to_string(tx.height),
                  std::vector<uint8_t>(tx.block_hash.begin(), tx.block_hash.end()));
    db_.set_nonce(batch, tx.issuer_address, expected + 1);
    record_applied_nonce(tx.issuer_address, expected + 1);
    std::cout << "[chain] CHECKPOINT: pinned height " << tx.height << " = "
              << db_.hex(tx.block_hash).substr(0, 12) << "…\n";
    return true;
}

// ---- Batched settlement mint (Phase 3) ------------------------------
//
// Recompute-authoritative: the tx declares NO amounts. We authenticate the
// serving node, bind the committed Merkle root to the flooded companion body,
// then walk the constituents in a fixed canonical order and credit exactly what
// committed state justifies. Every honest node computes the identical credited
// map from replicated inputs, so no vote is needed and nothing can be inflated.
bool Chain::apply_settlement_mint(const SettlementMintTx& tx,
                                  leveldb::WriteBatch& batch) {
    // 1) Serving node must be a registered validator; recover its pubkey.
    auto ventry = db_.get("v:" + db_.hex(tx.serving_node_id));
    if (!ventry || ventry->size() < 33) {
        std::cerr << "[chain] settlement reject: serving node not in v:\n"; return false;
    }
    PubKey33 pubkey;
    std::copy(ventry->begin(), ventry->begin() + 33, pubkey.begin());
    {
        auto msg = tx.sign_message();
        auto h   = crypto::sha256(msg.data(), msg.size());
        if (!crypto::verify_ecdsa(h, tx.node_signature, pubkey)) {
            std::cerr << "[chain] settlement reject: node signature invalid\n"; return false;
        }
    }
    // H6: declared serving wallet is bound to the registered key.
    const Address node_wallet = crypto::address_from_pubkey(pubkey);
    if (std::memcmp(node_wallet.data(), tx.serving_node_wallet.data(), 20) != 0) {
        std::cerr << "[chain] settlement reject: wallet != v: key\n"; return false;
    }
    if (tx.constituent_count == 0 ||
        tx.constituent_count > MAX_CONSTITUENTS_PER_SETTLEMENT) {
        std::cerr << "[chain] settlement reject: constituent_count out of range\n"; return false;
    }
    // Epoch replay guard (a node can't settle the same epoch twice).
    const std::string us_key =
        "us:" + db_.hex(tx.serving_node_id) + ":" + std::to_string(tx.epoch_id);
    if (db_.get(us_key).has_value()) {
        std::cerr << "[chain] settlement reject: epoch already settled\n"; return false;
    }
    // 2) Load the companion body (candidate.cpp keeps a settlement whose body is
    //    absent out of the block, so this only fails on a genuinely bad tx).
    auto braw = db_.get("sb:" + db_.hex(tx.constituents_merkle_root));
    if (!braw) { std::cerr << "[chain] settlement reject: body missing\n"; return false; }
    std::vector<PlayProof> proofs;
    if (!deserialize_settle_body(braw->data(), braw->size(), proofs)) {
        std::cerr << "[chain] settlement reject: body malformed\n"; return false;
    }
    if (proofs.size() != tx.constituent_count) {
        std::cerr << "[chain] settlement reject: count != body size\n"; return false;
    }
    // 3) Canonical order (content_hash, session_id); the root is over THIS order.
    std::sort(proofs.begin(), proofs.end(),
              [](const PlayProof& a, const PlayProof& b) {
        int c = std::memcmp(a.content_hash.data(), b.content_hash.data(), 32);
        if (c != 0) return c < 0;
        return std::memcmp(a.session_id.data(), b.session_id.data(), 32) < 0;
    });
    // M1: a duplicate (content_hash, session_id) ties the sort key, leaving a
    // platform-dependent leaf order and thus a divergent Merkle root -> fork.
    // Reject deterministically (a dup is a dup regardless of the tied order).
    for (size_t i = 1; i < proofs.size(); ++i) {
        if (std::memcmp(proofs[i].content_hash.data(),
                        proofs[i-1].content_hash.data(), 32) == 0 &&
            std::memcmp(proofs[i].session_id.data(),
                        proofs[i-1].session_id.data(), 32) == 0) {
            std::cerr << "[chain] settlement reject: duplicate (content_hash,session_id)\n";
            return false;
        }
    }
    {
        std::vector<std::vector<uint8_t>> leaves;
        leaves.reserve(proofs.size());
        for (const auto& pr : proofs) leaves.push_back(pr.serialize());
        Hash256 root = merkle_root_bytes(leaves);
        if (std::memcmp(root.data(), tx.constituents_merkle_root.data(), 32) != 0) {
            std::cerr << "[chain] settlement reject: merkle root mismatch\n"; return false;
        }
    }
    // 4) Recompute-authoritative walk with deterministic per-leaf skip.
    Ledger& ledger = ledger_;   // C2: shared block-scoped overlay
    std::map<Hash256, uint64_t> song_ord;       // stepped play_count per song
    std::map<Address, uint64_t> credits;        // recipient -> amount
    std::unordered_set<std::string> used_in_batch;
    const uint64_t base_supply = ledger.total_supply();   // C2: overlay-aware
    uint64_t running_supply = base_supply;
    for (const auto& pr : proofs) {
        std::string cperr;
        if (!check_play(pr, db_, cperr)) continue;                 // invalid proof -> skip
        const std::string sid = db_.hex(pr.session_id);
        // C1: skip a session already consumed within this body OR by any earlier
        // tx in the SAME block (a MINT or another settlement) — is_session_used
        // in check_play only sees committed u: markers.
        if (used_in_batch.count(sid) ||
            sessions_used_in_block_.count(pr.session_id)) continue;
        auto song = db_.get_song_section(pr.content_hash);
        if (!song) continue;                                       // song not on chain -> skip
        auto it = song_ord.find(pr.content_hash);
        const uint64_t pc = (it == song_ord.end())
            ? db_.get_play_count(pr.content_hash) : it->second;
        // Burn uses the COMMITTED base supply for cross-node determinism (pre-10k
        // it is 0; a running-delta refinement is a documented post-10k follow-up).
        const uint64_t burn =
            (pc >= FULL_REWARD_THRESHOLD) ? compute_burn_rate(base_supply) : 0;
        auto outs = compute_mint_outputs(pr, *song, pc,
                                         pr.serving_node_id, node_wallet);
        uint64_t out_sum = 0;
        for (const auto& o : outs) out_sum += o.amount;
        if (out_sum > 0 && running_supply + out_sum > SUPPLY_CAP) continue;   // skip
        if (burn > 0 && ledger.balance(pr.player_address) < burn)   continue;   // skip (C2)
        // Accept.
        for (const auto& o : outs) credits[o.recipient] += o.amount;
        running_supply += out_sum;
        song_ord[pr.content_hash] = pc + 1;
        used_in_batch.insert(sid);
        sessions_used_in_block_.insert(pr.session_id);  // C1: block-scoped consume
        db_.put_batch(batch, "u:" + sid, {});          // per-play replay marker
        db_.update_song_state(batch, pr, pc);           // play_count++ (+ discoverer)
    }
    // 5) Apply the aggregated credits. (Post-10k listener burn debit is a
    //    documented follow-up; pre-10k total_burn is 0.)
    std::vector<std::pair<Address, uint64_t>> outs_vec(credits.begin(), credits.end());
    ledger.credit_many(batch, outs_vec);
    db_.put_batch(batch, us_key, {});                   // epoch-settled marker
    std::cout << "[chain] SETTLEMENT_MINT: node "
              << db_.hex(tx.serving_node_id).substr(0, 12) << " epoch "
              << tx.epoch_id << " credited " << credits.size()
              << " recipients from " << proofs.size() << " plays\n";
    return true;
}

// ---- Slashing -------------------------------------------------------

bool Chain::apply_slash(const SlashTx& tx, leveldb::WriteBatch& batch) {
    // Authenticate the reporter's signature first — anyone can file
    // a slash, but they have to prove they did so to prevent third-
    // party replay attacks that would double-slash a valid target.
    if (!tx.verify_signature()) {
        std::cerr << "[chain] slash reject: reporter signature invalid\n";
        return false;
    }
    uint64_t expected = next_expected_nonce(tx.reporter_address);
    if (tx.nonce != expected) {
        std::cerr << "[chain] slash reject: nonce mismatch\n";
        return false;
    }
    // target_pubkey must match target_address (proves the target the
    // reporter is naming actually owns the pubkey whose signature
    // appears inside the evidence).
    {
        Address derived = crypto::address_from_pubkey(tx.target_pubkey);
        if (std::memcmp(derived.data(), tx.target_address.data(), 20) != 0) {
            std::cerr << "[chain] slash reject: target pubkey doesn't match "
                         "target address\n";
            return false;
        }
    }
    // Verify the cryptographic claim inside the evidence. Both proof
    // kinds derive in consensus/slashing.h; both have a verify()
    // that returns false if the underlying signatures don't line up.
    bool ok = false;
    switch (tx.kind) {
        case SlashKind::EQUIVOCATION: {
            // Equivocation proof = two distinct Confirmations from the
            // same validator at the same height. EquivocationProof
            // wire format starts directly at evidence.data() (no kind
            // byte — that's in SlashTx.kind already).
            // Layout:
            //   u32 height
            //   Confirmation conf_a   (32+33+64 = 129 bytes)
            //   Confirmation conf_b   (129 bytes)
            //   Hash256 block_a_hash  (32 bytes)
            //   Hash256 block_b_hash  (32 bytes)
            constexpr size_t kEvidenceLen = 4 + 129 + 129 + 32 + 32;
            if (tx.evidence.size() != kEvidenceLen) {
                std::cerr << "[chain] slash reject: equivocation evidence "
                             "wrong size\n"; return false;
            }
            EquivocationProof p{};
            const uint8_t* e = tx.evidence.data();
            std::memcpy(&p.height, e, 4); e += 4;
            std::memcpy(p.conf_a.validator_id.data(), e, 32); e += 32;
            std::memcpy(p.conf_a.pubkey.data(),       e, 33); e += 33;
            std::memcpy(p.conf_a.signature.data(),    e, 64); e += 64;
            std::memcpy(p.conf_b.validator_id.data(), e, 32); e += 32;
            std::memcpy(p.conf_b.pubkey.data(),       e, 33); e += 33;
            std::memcpy(p.conf_b.signature.data(),    e, 64); e += 64;
            std::memcpy(p.block_a_hash.data(),         e, 32); e += 32;
            std::memcpy(p.block_b_hash.data(),         e, 32);
            // Bind the proof to the named target: both confirmations
            // must actually be from target_pubkey.
            if (p.conf_a.pubkey != tx.target_pubkey ||
                p.conf_b.pubkey != tx.target_pubkey) {
                std::cerr << "[chain] slash reject: evidence pubkey doesn't "
                             "match target_pubkey\n"; return false;
            }
            ok = p.verify();
            break;
        }
        case SlashKind::FINGERPRINT_FORGERY: {
            // FingerprintForgeryProof — opaque to apply_slash for the
            // cryptographic part beyond the reporter sig already
            // checked. The semantic check (re-fetch audio + recompute
            // chromaprint) was done by the reporter; other validators
            // can independently re-do it from the evidence transcript.
            // For now we trust the reporter sig + accept; deepening to
            // multi-validator confirmation of the forgery before
            // slashing happens in a follow-up.
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::cerr << "[chain] slash reject: evidence verification failed\n";
        return false;
    }
    // Idempotent: if the target's already slashed, this is a no-op.
    // Anybody who tries to spam multiple slashes for the same target
    // burns their nonce but doesn't change state.
    std::vector<uint8_t> marker{1};
    const std::string target_hex = db_.hex(tx.target_address);
    db_.put_batch(batch, "slashed:" + target_hex, marker);
    db_.set_nonce(batch, tx.reporter_address, expected + 1);
    record_applied_nonce(tx.reporter_address, expected + 1);
    std::cerr << "[chain] SLASH applied: target=" << target_hex
              << " kind=" << static_cast<int>(tx.kind) << "\n";
    return true;
}

bool Chain::is_slashed(const Address& addr) const {
    return db_.get("slashed:" + db_.hex(addr)).has_value();
}

// ---- Phase 3: multi-mod proposals + votes ---------------------------

namespace {

bool address_is_zero(const Address& a) {
    for (uint8_t b : a) if (b) return false;
    return true;
}

bool hash_is_zero(const Hash256& h) {
    for (uint8_t b : h) if (b) return false;
    return true;
}

} // namespace

size_t Chain::effective_vote_count(const Hash256& prop_hash) const {
    size_t n = db_.count_proposal_votes(prop_hash);
    auto it = proposal_votes_in_block_.find(prop_hash);
    if (it != proposal_votes_in_block_.end()) {
        for (const Address& a : it->second) {
            if (!db_.has_proposal_vote(prop_hash, a)) ++n;
        }
    }
    return n;
}

bool Chain::grant_is_unanimous(const Hash256& prop_hash) const {
    // Voting-eligible moderators are OP and FOUNDER (VOICE is observer-only
    // and cannot cast a VOTE_YES — see apply_proposal's proposer_level gate).
    // Requiring a VOICE mod's vote would deadlock, so we count only OP+.
    auto it = proposal_votes_in_block_.find(prop_hash);
    size_t eligible = 0;
    for (const Address& m : db_.list_active_moderators()) {
        if (db_.get_mod_level(m) < static_cast<uint8_t>(ModLevel::OP)) continue;
        ++eligible;
        const bool voted =
            db_.has_proposal_vote(prop_hash, m) ||
            (it != proposal_votes_in_block_.end() && it->second.count(m));
        if (!voted) return false;
    }
    // Guard: with zero eligible moderators, "all voted" is vacuously true —
    // that would let a permissionless request self-execute with no approval.
    return eligible > 0;
}

bool Chain::execute_proposal(const ProposalTx& prop,
                             const Hash256& prop_hash,
                             uint32_t height,
                             leveldb::WriteBatch& batch) {
    const ProposalKind kind = static_cast<ProposalKind>(prop.kind);
    switch (kind) {
        case ProposalKind::HIDE_CONTENT: {
            // Idempotent — re-hiding already-hidden content is a no-op
            // but still flips propstatus so the proposal table is
            // self-consistent.
            db_.mark_song_deleted(batch, prop.target_hash);
            db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
            return true;
        }
        case ProposalKind::RELEASE_ESCROW: {
            // The escrow address is deterministic from the artist
            // address (crypto::escrow_address_for). The proposal
            // carries the amount the mods want to release; we cap at
            // the current escrow balance so a stale proposal that
            // overruns the balance just transfers what's there
            // instead of failing the whole tx.
            const Address escrow = crypto::escrow_address_for(prop.target_addr);
            uint64_t balance     = ledger_.balance(escrow);   // C2: overlay-aware
            uint64_t to_send     = std::min(balance, prop.amount);
            if (to_send == 0) {
                // Nothing to release — still flip status so the
                // proposal slot doesn't sit forever.
                db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
                return true;
            }
            Ledger& ledger = ledger_;   // C2: shared block-scoped overlay
            // If the artist is assigned to a record label, route the
            // escrow through the label's wallet splits instead of
            // crediting the artist directly. Splits are in basis points
            // (0..10000); any dust from integer rounding lands in the
            // LAST split so total credited == to_send exactly.
            auto label_name = db_.get_artist_label(prop.target_addr);
            std::optional<Database::LabelDef> label_def;
            if (label_name) label_def = db_.get_label(*label_name);
            if (label_def && !label_def->splits.empty()) {
                uint64_t credited = 0;
                for (size_t i = 0; i + 1 < label_def->splits.size(); ++i) {
                    const auto& s = label_def->splits[i];
                    uint64_t portion = (to_send * s.basis_points) / 10000;
                    if (portion > 0) {
                        if (!ledger.transfer(batch, escrow, s.wallet, portion)) {
                            return false;
                        }
                    }
                    credited += portion;
                }
                uint64_t tail = to_send - credited;
                if (tail > 0) {
                    if (!ledger.transfer(batch, escrow,
                            label_def->splits.back().wallet, tail)) {
                        return false;
                    }
                }
            } else if (!ledger.transfer(batch, escrow, prop.target_addr, to_send)) {
                return false;
            }
            db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
            return true;
        }
        case ProposalKind::GRANT_MODERATOR: {
            // Applied only after grant_is_unanimous() passed. Promote the
            // subject to the requested level (VOICE/OP). Never touch the
            // founder seat (that's set once, at bootstrap).
            const ModLevel lv = static_cast<ModLevel>(prop.amount);
            if (lv != ModLevel::VOICE && lv != ModLevel::OP) return false;
            auto founder = db_.get_founder();
            if (founder && std::memcmp(prop.target_addr.data(),
                                       founder->data(), 20) == 0) {
                // Idempotent close: never re-level the founder.
                db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
                return true;
            }
            db_.set_mod_level(batch, prop.target_addr, static_cast<uint8_t>(lv));
            db_.set_mod_pubkey(batch, prop.target_addr, prop.subject_pubkey);
            db_.set_mod_active_block(batch, prop.target_addr, height);
            db_.set_proposal_status(batch, prop_hash, Database::PROP_EXECUTED);
            return true;
        }
        case ProposalKind::VOTE_YES: {
            // VOTE_YES isn't itself executable — we never call execute
            // on a vote tx. If we somehow got here, treat it as a no-op.
            return false;
        }
    }
    return false;
}

bool Chain::apply_proposal(const ProposalTx& tx,
                           uint32_t height,
                           leveldb::WriteBatch& batch) {
    if (!tx.verify_signature()) return false;

    // Nonce is per-proposer regardless of which tx kind moved it
    // forward (same address-space as TRANSFER / MODERATOR_OP).
    uint64_t expected = next_expected_nonce(tx.proposer);
    if (tx.nonce != expected) return false;

    const ProposalKind kind = static_cast<ProposalKind>(tx.kind);

    // Only OP and FOUNDER may propose or vote — EXCEPT a GRANT_MODERATOR
    // request, which is permissionless: anyone can request a grant for any
    // wallet, but it still takes a unanimous moderator vote to take effect.
    // (VOTE_YES on a grant remains moderator-only, and only OP+ votes count
    // toward unanimity.)
    uint8_t proposer_level = db_.get_mod_level(tx.proposer);
    if (kind != ProposalKind::GRANT_MODERATOR
        && proposer_level < static_cast<uint8_t>(ModLevel::OP)) return false;

    // Majority threshold for HIDE / RELEASE / VOTE_YES: strict majority of
    // currently active moderators. Single-mod chains (e.g. just the founder)
    // execute on the proposer's own implicit YES. GRANT_MODERATOR ignores
    // this and uses grant_is_unanimous() instead.
    const size_t active_n = db_.list_active_moderators().size();
    const size_t needed   = (active_n / 2) + 1;

    switch (kind) {
        case ProposalKind::HIDE_CONTENT: {
            // Unused fields must be zeroed so the tx hash is canonical.
            if (!address_is_zero(tx.target_addr)) return false;
            if (tx.amount != 0)                   return false;
            // target_hash is the content hash being hidden — can't be
            // an all-zero placeholder.
            if (hash_is_zero(tx.target_hash))     return false;

            Hash256 prop_hash = tx.tx_hash();
            if (db_.has_proposal(prop_hash))      return false;

            db_.put_proposal(batch, prop_hash, tx.serialize());
            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            proposal_votes_in_block_[prop_hash].insert(tx.proposer);

            if (effective_vote_count(prop_hash) >= needed) {
                if (!execute_proposal(tx, prop_hash, height, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
        case ProposalKind::RELEASE_ESCROW: {
            if (!hash_is_zero(tx.target_hash))    return false;
            if (address_is_zero(tx.target_addr))  return false;
            if (tx.amount == 0)                   return false;

            Hash256 prop_hash = tx.tx_hash();
            if (db_.has_proposal(prop_hash))      return false;

            db_.put_proposal(batch, prop_hash, tx.serialize());
            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            proposal_votes_in_block_[prop_hash].insert(tx.proposer);

            if (effective_vote_count(prop_hash) >= needed) {
                if (!execute_proposal(tx, prop_hash, height, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
        case ProposalKind::VOTE_YES: {
            if (!address_is_zero(tx.target_addr)) return false;
            if (tx.amount != 0)                   return false;
            // target_hash is the proposal hash being voted on.
            if (hash_is_zero(tx.target_hash))     return false;

            const Hash256& prop_hash = tx.target_hash;
            auto raw = db_.get_proposal(prop_hash);
            if (!raw)                             return false;
            if (db_.get_proposal_status(prop_hash) != Database::PROP_PENDING)
                                                  return false;

            // Idempotency: same voter can't double-cast.
            if (db_.has_proposal_vote(prop_hash, tx.proposer)) return false;
            auto& in_block = proposal_votes_in_block_[prop_hash];
            if (in_block.count(tx.proposer))      return false;

            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            in_block.insert(tx.proposer);

            // The approval rule depends on the *referenced* proposal's kind:
            // GRANT_MODERATOR requires a unanimous moderator vote; everything
            // else is a simple majority.
            ProposalTx prop;
            if (!ProposalTx::deserialize(raw->data(), raw->size(), prop)) return false;
            const bool approved =
                (static_cast<ProposalKind>(prop.kind) == ProposalKind::GRANT_MODERATOR)
                    ? grant_is_unanimous(prop_hash)
                    : (effective_vote_count(prop_hash) >= needed);
            if (approved) {
                if (!execute_proposal(prop, prop_hash, height, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
            return true;
        }
        case ProposalKind::GRANT_MODERATOR: {
            // Permissionless request (proposer_level gate skipped above).
            // Field discipline: target_addr = subject wallet, amount = target
            // level (VOICE/OP), target_hash unused. subject_pubkey↔target_addr
            // is bound in verify_signature().
            if (!hash_is_zero(tx.target_hash))    return false;
            if (address_is_zero(tx.target_addr))  return false;
            const ModLevel lv = static_cast<ModLevel>(tx.amount);
            if (lv != ModLevel::VOICE && lv != ModLevel::OP) return false;
            // No grants until a founder exists — otherwise there'd be nobody
            // to approve them and grant_is_unanimous would (correctly) never
            // fire, but reject early for clarity.
            if (!db_.get_founder().has_value())   return false;
            // Nothing to do if the subject is already at/above that level
            // (also rejects targeting the founder, who is FOUNDER > OP).
            if (db_.get_mod_level(tx.target_addr) >= static_cast<uint8_t>(lv))
                return false;

            Hash256 prop_hash = tx.tx_hash();
            if (db_.has_proposal(prop_hash))      return false;

            db_.put_proposal(batch, prop_hash, tx.serialize());
            // Record the proposer's implicit YES. It only counts toward
            // unanimity if the proposer is itself an OP+ moderator; a
            // non-moderator requester's vote is ignored by grant_is_unanimous.
            db_.add_proposal_vote(batch, prop_hash, tx.proposer);
            proposal_votes_in_block_[prop_hash].insert(tx.proposer);

            if (grant_is_unanimous(prop_hash)) {
                if (!execute_proposal(tx, prop_hash, height, batch)) return false;
            }
            db_.set_nonce(batch, tx.proposer, expected + 1);
            record_applied_nonce(tx.proposer, expected + 1);
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

bool song_on_chain(const Database& db,
                   const Hash256& content_hash,
                   const std::string& compressed_fingerprint) {
    // Exact: this content_hash is already registered. The authoritative exact
    // marker is the content-height index (bh:), NOT get_fingerprint: put_
    // fingerprint writes the f: / bucket rows ONLY when audio::Fingerprint::
    // from_compressed can decode the blob, so a song whose compressed_
    // fingerprint does not decode leaves NO f: row — it would stay invisible to
    // the exact check forever and every node would re-mint the same content_
    // hash. set_content_height runs UNCONDITIONALLY for every song block in both
    // connect_block and rebuild_derived_state (and bh: is cleared in clear_
    // derived_state, so replay rebuilds it from scratch and a block's own row is
    // absent until its own step-4 write — no self-false-positive / replay
    // stall). We still also accept an f: hit for defence in depth.
    if (db.get_content_height(content_hash).has_value()) return true;
    if (db.get_fingerprint(content_hash).has_value())    return true;

    // Fuzzy: a different encoding of the same song hashes to a different
    // content_hash, so the exact index misses it. Probe the chromaprint bucket
    // index; anything scoring >= the shared threshold is the SAME song. Skips
    // its own content_hash and early-exits on the first hit — identical to the
    // replay + swarm-join scans this consolidates (same constant, same
    // algorithm, same de-dup-by-seen-set).
    if (compressed_fingerprint.empty()) return false;
    auto fp = audio::Fingerprint::from_compressed(compressed_fingerprint);
    if (!fp) return false;
    std::unordered_set<std::string> seen;
    for (auto bucket : fp->bucket_ids()) {
        for (const auto& cand_ch : db.get_bucket(bucket)) {
            if (cand_ch == content_hash) continue;
            const std::string key = crypto::to_hex(cand_ch);
            if (!seen.insert(key).second) continue;
            auto entry = db.get_fingerprint(cand_ch);
            if (!entry) continue;
            auto other = audio::Fingerprint::from_compressed(
                entry->compressed_fingerprint);
            if (!other) continue;
            // Integer/fixed-point same-song verdict (NOT similarity() >= 0.70f):
            // the fuzzy branch is on the consensus path (validate/replay), so a
            // float compare near the threshold would fork heterogeneous builds.
            if (fp->same_song(*other))
                return true;
        }
    }
    return false;
}

bool Chain::validate_block(const Block& block, std::string& error) const {
    // Caller (connect_block) holds the chain mutex. We're a const
    // function so any external direct call is fine; the lock above is
    // sufficient for the producer/network race.
    if (!block.validate()) { error = "block internal validation failed"; return false; }
    if (block.header.prev_hash != tip_.hash) {
        error = "prev_hash mismatch";
        return false;
    }
    // Reject duplicate songs. Only meaningful for song-bearing blocks;
    // heartbeats carry zero hashes by construction. Uses the SHARED
    // song_on_chain verdict — exact AND fuzzy — so the live accept path catches
    // the same near-duplicate the replay path does. Previously connect was
    // exact-only while replay was fuzzy, so a fuzzy near-dup slipped onto the
    // live chain and was only caught on a later replay (which truncated),
    // letting two nodes replaying at different times diverge. One verdict on
    // both paths removes that accept-vs-replay asymmetry.
    if (block.has_song && song_on_chain(db_, block.song.content_hash,
                                        block.song.compressed_fingerprint)) {
        error = "duplicate song";
        return false;
    }
    return true;
}

bool Chain::validate_block_quick_duplicate(const Hash256& content_hash) const {
    return db_.get_fingerprint(content_hash).has_value();
}

bool Chain::validate_candidate(const Block& block, std::string& error) const {
    // Structural validation only — signatures, fingerprint hash
    // consistency, format. prev_hash deliberately skipped: the
    // follower may not yet have the producer's previous block applied
    // via BlockPropagator's inv/getdata loop when this candidate
    // arrives. See chain.h declaration for the full rationale.
    if (!block.validate()) { error = "block internal validation failed"; return false; }
    // Duplicate-song check still applies — if our chain already has
    // the song, the producer's block can't be canonical for it. (Race
    // window: another producer might've registered the same song
    // milliseconds before us; in that case the slower producer's block
    // legitimately can't connect anyway.) Same shared exact+fuzzy verdict
    // as validate_block / replay so all paths agree on duplicate-ness.
    if (block.has_song && song_on_chain(db_, block.song.content_hash,
                                        block.song.compressed_fingerprint)) {
        error = "duplicate song";
        return false;
    }
    return true;
}

bool Chain::disconnect_block() {
    std::lock_guard<std::mutex> lk(mu_);
    if (tip_.height == 0) return false;
    // Remove tip entries
    leveldb::WriteBatch batch;
    db_.del_batch(batch, "b:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "h:" + db_.hex(tip_.hash));
    db_.del_batch(batch, "n:" + std::to_string(tip_.height));
    db_.del_batch(batch, "cw:" + db_.hex(tip_.hash));   // #8 weight index

    // Restore previous tip
    auto prev_hash = get_block_hash(tip_.height - 1);
    if (!prev_hash) {
        // Genesis revert
        db_.del_batch(batch, "t:tip");
        db_.write(batch);
        tip_ = {};   // hash{}, height 0, timestamp 0, weight 0
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
    // Restore cumulative weight from the now-current tip's "cw:" entry.
    tip_.weight = db_.get_u64("cw:" + db_.hex(*prev_hash)).value_or(0);
    // Restore timestamp from the now-current tip's block.
    if (auto pb = get_block(*prev_hash))
        tip_.timestamp_ms = pb->header.timestamp_ms;
    else
        tip_.timestamp_ms = 0;
    return true;
}

// ---- Fork-choice reorg (#8) -----------------------------------------
//
// Adopt a heavier branch via try-and-rollback. We rewrite the
// height→hash index (n:) and per-block weight (cw:) to the new branch,
// then rebuild_derived_state() replays it deterministically. If the
// re-derived chain isn't actually heavier (a branch block failed to
// apply, so rebuild rolled it back), we restore the previous chain. Old
// block bytes (b:/h:/cw: keyed by hash) are never overwritten by the new
// branch — only the per-height n: index and t:tip move — so restore just
// rewrites those back. Rare-path: holds the chain mutex throughout;
// derived state (balances/song index) is briefly cleared during rebuild,
// so a concurrent RPC read can momentarily see empty state. Acceptable
// for a young chain where reorgs are infrequent.
//
// NOTE: this path is new and not exercised by an automated test yet —
// see docs §22.5 / §21 #8.
bool Chain::reorg_to_branch(const Hash256& fork_hash, uint32_t fork_height,
                            const std::vector<Block>& branch, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (branch.empty()) { err = "empty branch"; return false; }

    // Fork point must be a block on our current chain at fork_height.
    {
        auto fh = get_block_height(fork_hash);
        if (!fh || *fh != fork_height) { err = "fork point not on chain"; return false; }
    }
    // Finality cap (Phase 0): refuse to rewrite history deeper than
    // FINALITY_DEPTH below the tip. Fork weight is costless without stake/PoW,
    // so an unbounded reorg is a cheap chain-takeover; capping the depth turns
    // deep history irreversible and makes pruning below this depth safe. (The
    // signed-checkpoint gate below is the other half of finality.)
    if (tip_.height > FINALITY_DEPTH && fork_height < tip_.height - FINALITY_DEPTH) {
        err = "reorg beyond finality depth (fork_height " + std::to_string(fork_height)
            + " < tip " + std::to_string(tip_.height) + " - " + std::to_string(FINALITY_DEPTH) + ")";
        return false;
    }
    // Branch must chain-link from fork_hash and be intrinsically valid.
    {
        Hash256 expect_prev = fork_hash;
        for (const auto& b : branch) {
            if (b.header.prev_hash != expect_prev) { err = "branch not contiguous"; return false; }
            if (!b.validate())                     { err = "branch block invalid"; return false; }
            expect_prev = b.hash();
        }
    }
    // Checkpoint gate (#7): every branch block lands at a known height; if
    // any is pinned by a checkpoint, its hash must match, or we refuse the
    // whole reorg — an attacker can't roll us across a checkpoint even with
    // a heavier branch.
    for (size_t i = 0; i < branch.size(); ++i) {
        const uint32_t h = fork_height + 1 + static_cast<uint32_t>(i);
        if (!checkpoint_ok(h, branch[i].hash())) {
            err = "branch violates checkpoint at height " + std::to_string(h);
            return false;
        }
    }
    // Adopt only if the branch is "better" by the canonical fork-choice
    // rule (weight → height → timestamp → hash) — the SINGLE source of
    // truth, shared with the propagator's peer-ahead check.
    const uint64_t fork_weight = db_.get_u64("cw:" + db_.hex(fork_hash)).value_or(0);
    uint64_t branch_weight = fork_weight;
    for (const auto& b : branch) branch_weight += count_plays(b);
    const uint32_t new_height   = fork_height + static_cast<uint32_t>(branch.size());
    const Hash256  new_tip_hash = branch.back().hash();
    ChainTip cand{ new_tip_hash, new_height,
                   branch.back().header.timestamp_ms, branch_weight };
    if (!tip_is_better(cand, tip_)) { err = "branch not better than current tip"; return false; }

    // Snapshot the current chain above the fork so we can restore.
    const uint32_t old_height = tip_.height;
    const Hash256  old_hash   = tip_.hash;
    const uint64_t old_weight = tip_.weight;
    const uint64_t old_ts     = tip_.timestamp_ms;
    std::vector<Hash256> old_hashes;
    for (uint32_t h = fork_height + 1; h <= old_height; ++h) {
        auto bh = get_block_hash(h);
        if (!bh) { err = "missing old block during snapshot"; return false; }
        old_hashes.push_back(*bh);
    }

    // Write a sequence of blocks into the index starting at base_height+1.
    auto write_branch = [&](const std::vector<Block>& blocks,
                            uint32_t base_height, uint64_t base_weight,
                            const Hash256& tip_hash, uint32_t tip_h) -> bool {
        leveldb::WriteBatch batch;
        uint64_t w = base_weight;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            const uint32_t h = base_height + 1 + static_cast<uint32_t>(i);
            auto ser = b.serialize();
            auto hh  = b.hash();
            db_.put_batch(batch, "b:" + db_.hex(hh), ser);
            db_.put_batch_u32(batch, "h:" + db_.hex(hh), h);
            db_.put_batch(batch, "n:" + std::to_string(h),
                          std::vector<uint8_t>(hh.begin(), hh.end()));
            Hash256 fh = Block::full_hash(ser);
            db_.put_batch(batch, "k:" + std::to_string(h),
                          std::vector<uint8_t>(fh.begin(), fh.end()));
            w += count_plays(b);
            db_.put_batch_u64(batch, "cw:" + db_.hex(hh), w);
            // Drain adopted txs + their submit_ms side-index from the mempool,
            // same as connect_block, so they aren't re-proposed and rejected on
            // nonce after the reorg. Song blocks also drop their sp: pending row.
            for (const auto& raw_tx : b.transactions) {
                if (raw_tx.empty()) continue;
                auto th = crypto::sha256(raw_tx.data(), raw_tx.size());
                db_.del_batch(batch, "p:" + db_.hex(th));
                db_.del_batch(batch, "pt:" + db_.hex(th));
            }
            if (b.has_song)
                db_.del_batch(batch, "sp:" + db_.hex(b.song.content_hash));
        }
        std::vector<uint8_t> tv(36);
        std::memcpy(tv.data(), tip_hash.data(), 32);
        for (int i = 0; i < 4; ++i) tv[32+i] = (tip_h >> (8*i)) & 0xFF;
        db_.put_batch(batch, "t:tip", tv);
        return db_.write(batch);
    };
    auto clear_n_above = [&](uint32_t from_h, uint32_t to_h) {
        if (from_h > to_h) return;
        leveldb::WriteBatch b;
        for (uint32_t h = from_h; h <= to_h; ++h)
            db_.del_batch(b, "n:" + std::to_string(h));
        db_.write(b);
    };

    // Crash-atomicity marker for the ADOPT tip move: set BEFORE write_branch
    // moves t:tip, so any crash between the tip move and rebuild_derived_state
    // finishing is detectable at next startup (init() re-runs the rebuild).
    // rebuild clears it once it repersists a consistent sr:vec. The RESTORE path
    // below re-arms its own marker, because this one is cleared by the failed-
    // adopt rebuild before restore moves the tip back.
    db_.put("sr:dirty", std::vector<uint8_t>{1});
    if (!write_branch(branch, fork_height, fork_weight, new_tip_hash, new_height)) {
        err = "failed to write branch"; return false;
    }
    if (new_height < old_height) clear_n_above(new_height + 1, old_height);

    // Re-derive ALL state from the rewritten index. rebuild walks
    // n:1..tip_.height, so point the tip at the new branch first.
    tip_.hash = new_tip_hash;
    tip_.height = new_height;
    tip_.timestamp_ms = branch.back().header.timestamp_ms;
    tip_.weight = branch_weight;
    rebuild_derived_state();   // may roll the tip back if a branch block fails

    // Success ONLY if the FULL branch applied (tip is exactly the branch
    // tip). If a middle block failed apply, rebuild rolled back to a prefix
    // — even if that prefix out-weighs the old chain we reject and restore,
    // so we never silently adopt a truncated branch the peer didn't offer.
    if (tip_.hash == new_tip_hash) {
        std::cout << "[chain] REORG: adopted branch — height "
                  << old_height << "→" << tip_.height << ", weight "
                  << old_weight << "→" << tip_.weight << "\n";
        return true;
    }

    // A branch block failed re-validation → restore the previous chain.
    err = "branch failed re-validation; restored previous chain";
    std::cerr << "[chain] REORG aborted, restoring previous chain\n";
    // Re-arm the crash-atomicity marker for the RESTORE tip move. The failed-
    // adopt rebuild above already CLEARED sr:dirty at its end (it rolled back to
    // a valid prefix and, from its own standalone view, reached consistency), so
    // without this the restore's write_branch (below) would move t:tip back to
    // the old chain with no dirty flag — a crash between that tip move and the
    // restore rebuild would leave t:tip / derived state / sr:vec inconsistent and
    // undetected. Set it here so init() re-derives on any crash through restore;
    // the restore rebuild clears it again on success.
    db_.put("sr:dirty", std::vector<uint8_t>{1});
    std::vector<Block> old_blocks;
    for (const auto& oh : old_hashes) {
        auto ob = get_block(oh);
        if (!ob) break;                  // old bytes gone (shouldn't happen)
        old_blocks.push_back(std::move(*ob));
    }
    // Drop the orphaned per-hash index entries the abandoned branch wrote
    // (n: is rewritten below; b:/h:/cw: are keyed by branch hash and would
    // otherwise leak).
    {
        leveldb::WriteBatch ob;
        for (const auto& b : branch) {
            const std::string hh = db_.hex(b.hash());
            db_.del_batch(ob, "b:"  + hh);
            db_.del_batch(ob, "h:"  + hh);
            db_.del_batch(ob, "cw:" + hh);
        }
        db_.write(ob);
    }
    clear_n_above(fork_height + 1, std::max(new_height, old_height));
    if (old_blocks.size() == old_hashes.size()) {
        if (!write_branch(old_blocks, fork_height, fork_weight, old_hash, old_height))
            std::cerr << "[chain] REORG restore: write_branch failed\n";
        tip_.hash = old_hash; tip_.height = old_height;
        tip_.weight = old_weight; tip_.timestamp_ms = old_ts;
        rebuild_derived_state();
    }
    return false;
}

std::optional<Hash256> Chain::get_block_full_hash(uint32_t height) const {
    auto val = db_.get("k:" + std::to_string(height));
    if (!val || val->size() < 32) return std::nullopt;
    Hash256 h;
    std::memcpy(h.data(), val->data(), 32);
    return h;
}

bool Chain::rebuild_derived_state() {
    // Replay every block from height 1 to tip. Used at startup and after
    // a peer-driven sync. Validates each block as it's replayed so a
    // corrupted on-disk block (or a chain a malicious peer pushed past
    // an earlier weaker check) is caught at the earliest possible point
    // rather than silently producing a broken index.
    //
    // Per-block validation runs the SAME deterministic checks every node
    // applies at ingest time (Model 1 — no block-level votes):
    //   1. block.validate()                   — internal consistency:
    //        sha256(compressed_fingerprint) == header.fingerprint_hash,
    //        merkle_root matches txs, structural fields well-formed.
    //   2. prev_hash chain link               — points at h-1's block hash.
    //   3. chromaprint fuzzy uniqueness       — incoming song's
    //        fingerprint isn't >= audio::kChromaprintSimThreshold (0.70)
    //        similar to anything already replayed. Catches duplicate
    //        registrations.
    //   4. apply_transactions                 — every tx is well-formed,
    //        has sufficient balance / valid nonce / valid signature.
    //
    // If any block fails, we stop the replay and roll the chain back to
    // the last-known-good height. The DB tip pointer is rewritten so a
    // following startup picks up where we left off.
    //
    // P4: this path is ALSO where a reorg adopts a branch, so it must apply the
    // SAME committed state_root gate as connect_block — otherwise a branch whose
    // blocks carry a forged header.state_root would be accepted here yet rejected
    // by a node that received the same blocks linearly (audit #2/#10). We keep a
    // rolling committed accumulator (starts at the empty-state zero vector, since
    // clear_derived_state just wiped everything) and, per block, seed a working
    // copy, feed its state writes through the accumulator hook, and require
    // working.root() == header.state_root before promoting — exactly mirroring
    // connect_block. sr:vec is persisted in each block's own batch so it commits
    // atomically with that block's derived state.
    //
    // Crash-atomicity (audit #3/#8): set sr:dirty BEFORE tearing down derived
    // state and clear it only after the loop repersists a consistent sr:vec, so
    // an interrupted rebuild is re-run by init() at the next startup instead of
    // leaving t:tip/state/sr:vec mutually inconsistent.
    db_.put("sr:dirty", std::vector<uint8_t>{1});
    db_.clear_derived_state();

    StateAccumulator acc;      acc.db = &db_;          // rolling committed root
    StateAccumulator working;  working.db = &db_;      // per-block working copy
    Hash256 prev_hash{};                             // genesis prev = zero
    uint32_t last_good_height = 0;
    uint64_t cum_weight = 0;                          // #8: cumulative plays
    for (uint32_t h = 1; h <= tip_.height; ++h) {
        auto bhash = get_block_hash(h);
        if (!bhash) return false;
        auto block = get_block(*bhash);
        if (!block) return false;

        // 1. Internal consistency.
        if (!block->validate()) {
            std::cerr << "[chain] replay: block " << h
                      << " failed internal validation — stopping\n";
            break;
        }
        // 2. Chain link.
        if (block->header.prev_hash != prev_hash) {
            std::cerr << "[chain] replay: block " << h
                      << " prev_hash break — stopping\n";
            break;
        }
        // 2b. Checkpoint gate (#7): catches a chain a weaker earlier build
        //     (or DB tampering) wrote past a height since pinned in config.
        if (!checkpoint_ok(h, *bhash)) {
            std::cerr << "[chain] replay: checkpoint mismatch at height " << h
                      << " — truncating to last good height " << last_good_height << "\n";
            break;
        }
        // 3. (Model 1) No block-level confirmation/quorum check. Blocks
        //    carry no validator votes; validity is the deterministic
        //    content + history check (steps 1, 2, 4, 5). Transaction
        //    signatures are still verified in apply_transactions (step 5).

        // 3. Uniqueness against the partial index we've built so far, via the
        //    SAME shared verdict every other path uses. Exact (get_fingerprint)
        //    OR fuzzy chromaprint (bucket scan vs kChromaprintSimThreshold).
        //    Skipped for heartbeat / non-song blocks. block h's own fingerprint
        //    isn't written until step 4 below, so a fresh song falls through to
        //    the fuzzy scan exactly as before; only a genuine re-registration
        //    of an already-replayed content_hash trips the exact half.
        if (block->has_song &&
            song_on_chain(db_, block->song.content_hash,
                          block->song.compressed_fingerprint)) {
            std::cerr << "[chain] replay: block " << h
                      << " carries a duplicate song (content/fingerprint already "
                         "on chain) — stopping\n";
            break;
        }

        // 4. Apply — mirror connect_block's accumulator lifecycle + state_root
        //    gate so a reorg-adopted block is accepted iff connect_block would.
        leveldb::WriteBatch batch;
        working.copy_from(acc.vec);        // seed from the rolling committed root
        bool apply_ok;
        {
            AccGuard g(db_, &working);     // feed this block's state writes to it
            if (block->has_song) {
                db_.put_fingerprint(batch, block->song);
                db_.put_song_meta(batch, block->song.content_hash, block->song);
                db_.add_to_artist_index(batch, block->song.artist,
                                         block->song.content_hash);
                db_.add_to_genre_index(batch, block->song.genre,
                                        block->song.content_hash);
                // Rebuild the content-height marker (bh:) that song_on_chain's
                // exact check consults. It is cleared in clear_derived_state, so
                // replay MUST repopulate it here or the exact duplicate check
                // goes blind on a restart / reorg. Written AFTER step 3's
                // song_on_chain call above, so block h's own row is absent when h
                // is checked (no self-hit), exactly like put_fingerprint.
                db_.set_content_height(batch, block->song.content_hash, h);
            }
            applied_nonce_in_block_.clear();
            proposal_votes_in_block_.clear();
            sessions_used_in_block_.clear();
            apply_ok = apply_transactions(*block, h, batch);
        }   // AccGuard clears acc_ here, even if a writer threw
        if (!apply_ok) {
            std::cerr << "[chain] replay: block " << h
                      << " apply_transactions failed — stopping\n";
            break;
        }
        // P4 state_root gate: reject a block whose committed root doesn't match
        // its header (identical to connect_block, so reorg == linear connect).
        if (block->header.version >= 4 &&
            working.root() != block->header.state_root) {
            std::cerr << "[chain] replay: block " << h
                      << " state_root mismatch — stopping\n";
            break;
        }
        // #8: accumulate fork weight and persist the per-block "cw:" index +
        // the committed sr:vec in the SAME batch as this block's derived state,
        // so the accumulator commits atomically with the block it reflects.
        cum_weight += count_plays(*block);
        db_.put_batch_u64(batch, "cw:" + db_.hex(*bhash), cum_weight);
        db_.put_batch(batch, "sr:vec",
                      StateAccumulator::serialize_vec(working.vec));
        db_.write(batch);

        acc.vec         = working.vec;   // promote this block's committed root
        prev_hash       = *bhash;
        last_good_height = h;
    }

    // The tip's cumulative weight is whatever we accumulated up to the
    // last good height (rebuild stops on the first bad block).
    tip_.weight = cum_weight;

    // Roll the tip back to the last good height if we stopped early.
    if (last_good_height < tip_.height) {
        std::cerr << "[chain] replay rolled tip back from " << tip_.height
                  << " to " << last_good_height
                  << " (bad blocks past this point will be ignored on "
                     "next startup until they're re-fetched)\n";
        tip_.height = last_good_height;
        tip_.hash   = prev_hash;
        std::vector<uint8_t> v(36);
        std::memcpy(v.data(), tip_.hash.data(), 32);
        for (int i = 0; i < 4; ++i)
            v[32 + i] = static_cast<uint8_t>((tip_.height >> (8*i)) & 0xff);
        db_.put("t:tip", v);
    }
    // M2: re-derive the live checkpoint set from the persisted cp: after a
    // replay (a reorg may have adopted a branch that pinned a new checkpoint;
    // the in-memory set is otherwise only refreshed at construction).
    db_.for_each_with_prefix("cp:", [this](const std::string& key,
                                           const std::string& val) {
        if (val.size() != 32) return true;
        try {
            uint32_t h = static_cast<uint32_t>(std::stoul(key.substr(3)));
            Hash256 hh{}; std::memcpy(hh.data(), val.data(), 32);
            checkpoints_[h] = hh;
        } catch (...) {}
        return true;
    });
    // P4: committed_acc_ is the rolling root promoted through the last good
    // block (the gate above proved each equals its header.state_root); after a
    // tip rollback it correctly reflects last_good_height. Persist it (covers the
    // zero-block fresh-chain case where the loop never ran) so the next
    // connect_block's gate uses the right baseline.
    committed_acc_ = acc.vec;
    db_.put("sr:vec", StateAccumulator::serialize_vec(committed_acc_));
    // Clear the crash-atomicity marker LAST: only now are t:tip, the derived
    // state, and sr:vec mutually consistent. A crash before this point leaves
    // sr:dirty set, so init() re-runs the rebuild at the next startup.
    db_.del("sr:dirty");
    return true;
}

int Chain::first_unappliable_tx(const std::vector<std::vector<uint8_t>>& txs) {
    std::lock_guard<std::mutex> lk(mu_);
    // Trial-apply growing prefixes [0..i] into a throwaway batch (never
    // written). The first i whose inclusion makes apply_transactions fail is the
    // culprit — [0..i-1] applied cleanly, so adding txs[i] is what broke it.
    // apply_transactions is reused verbatim (same nonce/balance/authority rules,
    // same per-sender nonce staging), so this agrees bit-for-bit with what
    // connect_block would do at tip_.height + 1. The prefix walk keeps sequential
    // same-sender nonces intact: txs[i] is only flagged if it fails GIVEN every
    // earlier tx already applied. O(n^2) apply work, but n is mempool-bounded and
    // this runs only on the rare wedge/commit-failure path.
    Block probe;
    probe.transactions.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); ++i) {
        probe.transactions.push_back(txs[i]);
        leveldb::WriteBatch batch;                 // discarded — no db mutation
        if (!apply_transactions(probe, tip_.height + 1, batch))
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace mc
