#include "candidate.h"
#include "../core/chain.h"
#include "../core/transaction.h"
#include "../storage/database.h"
#include "../network/manager.h"
#include "../audio/fingerprint.h"
#include "../audio/ogg_validator.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <iomanip>

namespace mc {

namespace fs = std::filesystem;

static uint64_t now_ms_c() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string random_hex(size_t bytes) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    for (size_t i = 0; i < (bytes + 7) / 8; ++i)
        ss << std::hex << std::setw(16) << std::setfill('0') << dist(gen);
    return ss.str().substr(0, bytes * 2);
}

// ---- CandidateManager: block producer ------------------------------

void CandidateManager::start(Chain& chain, Database& db,
                              net::NetworkManager& network,
                              const net::NodeConfig& cfg,
                              const crypto::KeyPair& keypair) {
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        if (running_) return;
        running_ = true;
        // Bug fix #3: do NOT push last_block_at_ms_ to "now" on boot.
        // That would delay the first heartbeat block by a full 5 min,
        // which means the founder GRANT enqueued immediately after a
        // fresh start sits in the mempool well past the operator's
        // patience. Leaving last_block_at_ms_ at 0 means the loop's
        // first iteration sees (now - 0) > HEARTBEAT_INTERVAL_MS and
        // mints a block right away if anything is pending — the
        // exact behaviour we want.
        last_block_at_ms_ = 0;
    }
    // Model 1 (proof-of-unique-song): there is NO producer/follower split and
    // NO central block producer. EVERY node runs the producer. A block is
    // minted only when there is real content — a unique-song registration or
    // pending txs — and the network converges on the heaviest valid chain
    // (one-song-once uniqueness + cumulative-audited-play weight + reorg), so
    // there is no central point of failure. The old `validator_enabled`
    // "follower" gate existed ONLY because every node also minted empty
    // heartbeat blocks, forking the chain each interval; empty heartbeats are
    // now gone (see the producer body), so the gate is gone with them.
    // (`validator_enabled` remains in the config schema but no longer gates
    // anything.)
    heartbeat_thread_ = std::thread([this, &chain, &db, &network, &cfg, &keypair] {
        heartbeat_loop(chain, db, network, cfg, keypair);
    });
}

void CandidateManager::stop() {
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        running_ = false;
    }
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

bool CandidateManager::enqueue_registration(PendingRegistration reg) {
    {
        std::lock_guard<std::mutex> lk(regs_mutex_);
        // Insert-or-replace keyed by content_hash. The authoritative min-merge
        // (which registration variant + submit_ms wins for a content_hash)
        // already happened in RatsApi::ingest_fpsubmit against the persisted
        // sp: mempool before we get here, so we just adopt the DB-canonical
        // winner into the producer's in-memory view. Every node converges its
        // sp: mempool to the same winner and thus builds the identical entry
        // here (invariant I1).
        pending_songs_[reg.content_hash] = std::move(reg);
    }
    // Nudge the heartbeat loop. wake_requested_ is what the wait predicate
    // actually checks (a bare non-empty buffer must NOT wake it — an immature
    // song has to wait out the propagation window without spinning).
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = 0; // force the timer check to fire
        wake_requested_   = true;
    }
    heartbeat_cv_.notify_all();
    return true;
}

size_t CandidateManager::pending_registration_count() const {
    std::lock_guard<std::mutex> lk(regs_mutex_);
    return pending_songs_.size();
}

void CandidateManager::wake() {
    // Reset the heartbeat clock so the producer's first check after
    // waking sees the threshold as exceeded and mints immediately,
    // AND raise the wake_requested_ flag so the wait_for predicate
    // breaks out of sleep even when nothing is in pending_songs_ (e.g. a
    // freshly-flooded tx that needs a tx-only block).
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = 0;
        wake_requested_   = true;
    }
    heartbeat_cv_.notify_all();
}

// ---- commit_block: shared finalize path -----------------------------

bool CandidateManager::commit_block(
    Block& block,
    Chain& chain, Database& db,
    net::NetworkManager& network,
    const net::NodeConfig& cfg,
    const crypto::KeyPair& keypair,
    const std::vector<std::pair<Hash256, std::vector<uint8_t>>>& consumed_txs,
    std::string& err) {

    // MODEL 1 — vote-free deterministic consensus.
    //
    // A block becomes canonical the instant chain.connect_block accepts
    // it. connect_block runs the full deterministic validation
    // (block.validate(): fingerprint/merkle commitment; prev_hash link
    // to the current tip; apply_transactions: every tx's signature,
    // nonce, and balance). There is no candidate registration, no self-
    // signed block confirmation, and no quorum wait — those were the
    // "vote" machinery, now removed. Every peer that later receives this
    // block re-derives the same verdict independently (BlockPropagator
    // INV/getdata + DHT, then ingest_block_bytes runs the identical
    // deterministic checks). Genesis is no longer special-cased: at
    // height 0 connect_block simply accepts the first valid block as the
    // chain's genesis.
    //
    // network / keypair are no longer used here (no candidate broadcast,
    // no block-level signing); consumed_txs is informational because the
    // mempool drain is folded into connect_block's leveldb batch.
    (void)network;
    (void)keypair;
    (void)consumed_txs;

    // P4: stamp the committed state_root this block will yield BEFORE connect_block
    // re-derives + asserts it, and BEFORE the header is hashed (state_root is
    // folded into the block-hash preimage). Producer path only; peers recompute.
    if (block.header.version >= 4)
        block.header.state_root = chain.compute_candidate_state_root(block);

    if (!chain.connect_block(block)) {
        err = "Chain connect_block rejected";
        return false;
    }

    // Gossip the new block out — INV broadcast to connected peers +
    // DHT-announce so multi-source catch-up can find this node.
    if (announcer_) announcer_(block.hash());

    // Best-effort .blk dump for the operator. Bucket by height/1000 so
    // no directory holds more than ~1000 files (NTFS degrades around 10M
    // files in a flat folder). Layout: blocks/00000123/00123456.blk
    try {
        const uint32_t h = chain.tip().height;
        std::ostringstream sub;
        sub << std::setw(8) << std::setfill('0') << (h / 1000);
        fs::path blocks_dir = fs::path(cfg.data_dir) / "blocks" / sub.str();
        fs::create_directories(blocks_dir);
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << h << ".blk";
        fs::path file_path = blocks_dir / fname.str();
        auto block_bytes = block.serialize();
        std::ofstream f(file_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(block_bytes.data()),
                block_bytes.size());
    } catch (...) {
        // Non-fatal — block is durable in LevelDB even if the .blk write fails.
    }

    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = now_ms_c();
    }
    return true;
}

// ---- Heartbeat loop ------------------------------------------------

void CandidateManager::heartbeat_loop(Chain& chain, Database& db,
                                       net::NetworkManager& network,
                                       const net::NodeConfig& cfg,
                                       const crypto::KeyPair& keypair) {
    // Wakes on three signals:
    //   * a player just queued a song registration (enqueue_registration
    //     pokes us so the song lands in the next block — and we keep
    //     draining back-to-back while the queue stays non-empty)
    //   * the 30-second poll (slow-path check for the empty heartbeat
    //     interval — only relevant when nothing is queued)
    //   * stop()
    //
    // On wake: if there are pending registrations we mint song blocks
    // until the queue drains, with no sleep between them. Otherwise we
    // mint an empty heartbeat block iff HEARTBEAT_INTERVAL_MS has
    // elapsed since the last block.
    bool minted_last = false;
    while (true) {
        // Wait for work unless the previous pass just minted a block — then loop
        // straight back to drain the next mature item with zero delay, so a
        // matured album lands back-to-back. The predicate wakes ONLY on stop or
        // an explicit wake/enqueue; the periodic timeout re-checks the maturity
        // gate (a buffered item becomes eligible purely by the passage of time,
        // with no new signal). We deliberately do NOT wake merely because the
        // buffer is non-empty — an immature item (still inside the propagation
        // window) must never spin the loop.
        {
            std::unique_lock<std::mutex> lk(producer_mu_);
            if (!minted_last) {
                heartbeat_cv_.wait_for(lk, std::chrono::seconds(30), [this] {
                    return !running_ || wake_requested_;
                });
            }
            wake_requested_ = false;
            if (!running_) return;
        }
        minted_last = false;

        const uint64_t now = now_ms_c();   // ONLY used for the maturity gate

        // ---- Deterministic song selection (invariants I3 + I6) -----------
        //
        // Next song = the LOWEST content_hash among songs that are (a) mature —
        // past the propagation window, so every node already holds them — and
        // (b) not already on chain (the ONE shared song_on_chain verdict).
        // pending_songs_ is a std::map, so it iterates content_hash ascending
        // and the first qualifying entry is that minimum on every node.
        // content_hash is a unique 32-byte key ⇒ total order ⇒ every node
        // selects the SAME song for a given tip. `now` only decides WHEN this
        // node mints, never WHAT bytes it mints.
        std::optional<PendingRegistration> reg;   // the selected song (a copy)
        {
            std::lock_guard<std::mutex> lk(regs_mutex_);
            for (auto it = pending_songs_.begin(); it != pending_songs_.end(); ) {
                PendingRegistration& r = it->second;
                if (r.retries >= 3) { ++it; continue; }   // gave up (bug-fix #8)
                const uint64_t age = now >= r.submit_ms ? now - r.submit_ms : 0;
                if (age < PROPAGATION_WINDOW_MS) { ++it; continue; }   // buffer
                if (song_on_chain(db, r.content_hash, r.compressed_fingerprint)) {
                    // Already landed (ours or a peer's) — lazily prune the
                    // in-memory buffer + any stale sp: row and move on.
                    db.del_pending_song(it->first);
                    it = pending_songs_.erase(it);
                    continue;
                }
                reg = r;   // copy the selected registration for the build
                break;
            }
        }

        auto all_pending = db.get_all_pending_txs();

        if (!all_pending.empty() || reg) {
            std::cout << "[producer] tick: regs=" << (reg ? 1 : 0)
                      << " pending_txs=" << all_pending.size() << "\n";
        }

        // ---- Bug fix #7: pre-flight every pending tx -----------------
        //
        // Old behaviour stuffed every pending tx into the block without
        // checking signatures first. apply_transactions() then bailed on
        // the first bad one and the whole block was rejected, leaving
        // the bad tx in the mempool for the next iteration to trip over
        // — chain wedged.
        //
        // Now we deserialize + verify each pending tx in-place. Anything
        // that doesn't pass basic structural / signature checks is
        // dropped from the mempool right here so it can't poison
        // another attempt. The chain still does the full
        // apply-rules check; pre-flight is only the cheap floor.
        // We can't rely on leveldb iteration order here: pending txs are
        // keyed by tx_hash (sha256 of the serialized tx), and that's
        // effectively random. If a single address queues two txs (e.g.
        // the bootstrap path: GRANT nonce=0 then UsernameTx nonce=1),
        // the UsernameTx may sort BEFORE the GRANT in the resulting
        // block; the chain then runs UsernameTx first, sees DB nonce 0,
        // rejects "nonce mismatch (tx=1 expected=0)" and the entire
        // block fails apply_transactions. We hit this on every cold
        // bootstrap.
        //
        // Fix: collect (sender, nonce) for every tx during the verify
        // pass, then sort by (sender, nonce) so consecutive nonces from
        // the same address always appear in the right order.
        struct TxSlot {
            Hash256              hash;
            std::vector<uint8_t> raw;
            Address              sender{};   // zero for MINT (no per-sender nonce)
            uint64_t             nonce = 0;
            // Origin-stamped ms from the pt: side-index (the MC_TX flood value),
            // or 0 for an un-flooded / locally-injected tx. Drives the inclusion
            // boundary + maturity gate below; never written into the block.
            uint64_t             submit_ms = 0;
        };
        std::vector<TxSlot> slots;
        slots.reserve(all_pending.size());
        for (auto& [tx_hash, raw] : all_pending) {
            if (raw.empty()) {
                db.del_pending_tx(tx_hash);
                continue;
            }
            bool ok = false;
            const char* why = "ok";
            TxSlot slot;
            slot.hash      = tx_hash;
            slot.submit_ms = db.get_pending_tx_submit_ms(tx_hash).value_or(0);
            TxType type = static_cast<TxType>(raw[0]);
            switch (type) {
                case TxType::TRANSFER: {
                    TransferTx tx;
                    if (!TransferTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "TRANSFER: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "TRANSFER: verify_signature failed";
                    } else {
                        slot.sender = tx.from_address;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MODERATOR_OP: {
                    ModeratorOpTx tx;
                    if (!ModeratorOpTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "MODERATOR_OP: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "MODERATOR_OP: verify_signature failed";
                    } else if (static_cast<ModOpCode>(tx.op_code) == ModOpCode::GRANT
                               && static_cast<ModLevel>(tx.level) == ModLevel::FOUNDER
                               && std::memcmp(tx.proposer.data(), tx.subject.data(), 20) == 0
                               && founder_is_pinned()
                               && std::memcmp(tx.subject.data(),
                                              PINNED_FOUNDER_ADDRESS.data(), 20) != 0) {
                        // Defense-in-depth: never even stage a founder-bootstrap
                        // self-grant for a non-pinned address. The authoritative
                        // reject is in Chain::apply_moderator_op; this just keeps
                        // a bogus claim out of the local mempool / a mined block.
                        why = "MODERATOR_OP: bootstrap subject is not the pinned founder";
                    } else {
                        slot.sender = tx.proposer;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MODERATOR_PROPOSAL: {
                    ProposalTx tx;
                    if (!ProposalTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "PROPOSAL: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "PROPOSAL: verify_signature failed";
                    } else {
                        slot.sender = tx.proposer;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::USERNAME_REGISTER: {
                    UsernameTx tx;
                    if (!UsernameTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "USERNAME_REGISTER: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "USERNAME_REGISTER: verify_signature failed";
                    } else {
                        slot.sender = tx.owner;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                case TxType::MINT: {
                    MintTx tx;
                    if (!MintTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "MINT: deserialize failed";
                    } else { ok = true; }
                    break;
                }
                case TxType::RELAY_REWARD: {
                    // Founder-signed relay reward. The apply path (chain.cpp)
                    // already handles RELAY_REWARD, but the candidate/mempool
                    // gate had no case for it — so every reward was dropped here
                    // as "unknown TxType" and mini-nodes never actually got
                    // paid (their balance stayed 0). Validate like the others.
                    RelayRewardTx tx;
                    if (!RelayRewardTx::deserialize(raw.data(), raw.size(), tx)) {
                        why = "RELAY_REWARD: deserialize failed";
                    } else if (!tx.verify_signature()) {
                        why = "RELAY_REWARD: verify_signature failed";
                    } else {
                        slot.sender = tx.issuer_address;
                        slot.nonce  = tx.nonce;
                        ok = true;
                    }
                    break;
                }
                default:
                    why = "unknown TxType";
                    ok = false;
            }
            if (!ok) {
                std::cerr << "[chain] dropping malformed mempool tx "
                          << crypto::to_hex(tx_hash).substr(0, 12) << "… ("
                          << static_cast<int>(raw[0]) << " : " << why << ")\n";
                db.del_pending_tx(tx_hash);
                continue;
            }
            slot.raw = std::move(raw);
            slots.push_back(std::move(slot));
        }
        // TOTAL-order sort by (sender, nonce, tx_hash). Same-address txs stay in
        // monotonic nonce order (apply_transactions requires per-sender
        // monotonicity); the tx_hash tiebreak makes the order total, which kills
        // the old MINT tie — every MINT carries sender=0/nonce=0, so (sender,
        // nonce) alone left ≥2 MINTs in unspecified std::sort order and two
        // nodes could emit different merkle roots. tx_hash is unique ⇒ one
        // canonical body on every node (invariant I5).
        std::sort(slots.begin(), slots.end(), [](const TxSlot& a, const TxSlot& b) {
            if (a.sender != b.sender) return a.sender < b.sender;
            if (a.nonce  != b.nonce ) return a.nonce  < b.nonce;
            return a.hash < b.hash;
        });

        // ---- Deterministic tx inclusion + block timestamp (I4 + I5 + I6) ----
        //
        // Both the tx set and the header timestamp must be a pure function of
        // replicated values, never a per-node clock read:
        //   * SONG block: timestamp = the selected song's (replicated)
        //     submit_ms; include every valid tx with submit_ms ≤ that boundary.
        //     Because the song is mature (past the window), any such tx is at
        //     least as old ⇒ also mature ⇒ already on every node ⇒ the set is
        //     identical everywhere, with no clock-skew edge.
        //   * TX-ONLY (heartbeat) block: no song ⇒ no replicated boundary, so
        //     include only MATURE txs and set the timestamp to the MAX included
        //     submit_ms (still a replicated value). A tx within ~(skew+jitter)
        //     of a node's maturity edge is the lone residual — resolved by
        //     fork-choice, never a double-spend (every tx keeps its sig/nonce/
        //     balance check in apply_transactions).
        uint64_t block_ts = 0;
        std::vector<TxSlot> included;
        included.reserve(slots.size());
        if (reg) {
            block_ts = reg->submit_ms;
            for (auto& s : slots)
                if (s.submit_ms <= block_ts) included.push_back(std::move(s));
        } else {
            for (auto& s : slots) {
                const uint64_t age = now >= s.submit_ms ? now - s.submit_ms : 0;
                if (age < PROPAGATION_WINDOW_MS) continue;   // not yet propagated
                included.push_back(std::move(s));
            }
        }

        // ---- Applicability gate (LIVENESS bug-fix) -----------------------
        //
        // The pre-flight above only checks structure + signature, so a well-
        // signed but UNAPPLIABLE tx still slips through — e.g. an attacker floods
        // a fresh zero-balance keypair's TransferTx{nonce=0}. With submit_ms 0 it
        // matures instantly and, being <= every boundary, sorts into EVERY
        // candidate. apply_transactions is all-or-nothing, so that one tx fails
        // the WHOLE block; the block never connects, the identical poisoned
        // candidate rebuilds next tick and fails again — a one-packet, network-
        // wide chain wedge (and innocent songs in the same block burn retries).
        // Gate inclusion on APPLICABILITY: trial-apply the assembled body against
        // the current tip and drop the first tx that won't apply (nonce / balance
        // / authority), re-checking until the body is clean, del_pending_tx'ing
        // each culprit so it can't be re-selected. Every included tx is mature
        // (past the propagation window), so a sequential-nonce sibling is already
        // present and applies alongside it — only genuine poison is dropped. Same
        // trial-apply as the commit-failure backstop below, so they never
        // disagree.
        for (;;) {
            std::vector<std::vector<uint8_t>> raws;
            raws.reserve(included.size());
            for (auto& s : included) raws.push_back(s.raw);
            const int bad = chain.first_unappliable_tx(raws);
            if (bad < 0) break;
            std::cerr << "[chain] dropping unappliable mempool tx "
                      << crypto::to_hex(included[bad].hash).substr(0, 12)
                      << "… (nonce/balance/authority)\n";
            db.del_pending_tx(included[bad].hash);
            included.erase(included.begin() + bad);
        }

        // Header timestamp is a pure function of replicated values (I4). SONG
        // block: the selected song's submit_ms (already set). TX-ONLY block: the
        // MAX included submit_ms — recomputed HERE from the PRUNED set so a tx
        // dropped by the gate can never set the boundary.
        if (!reg) {
            block_ts = 0;
            for (auto& s : included)
                if (s.submit_ms > block_ts) block_ts = s.submit_ms;
        }

        std::vector<std::pair<Hash256, std::vector<uint8_t>>> pending_txs;
        pending_txs.reserve(included.size());
        for (auto& s : included)
            pending_txs.emplace_back(s.hash, std::move(s.raw));

        // Model 1 — two block kinds, both validated by EVERY node:
        //   * song block:      carries a unique-song registration (has a
        //                      chromaprint fingerprint; validated by the
        //                      one-song-once uniqueness check).
        //   * heartbeat block: carries tx data only (founder GRANT, play
        //                      mints, relay rewards, transfers, usernames) and
        //                      NO fingerprint; validated by the tx rules.
        // We mint whenever there is EITHER — a heartbeat block is a normal,
        // fully-validated, propagated block, just without a song. What we do
        // NOT mint is an empty time-based block (no song AND no txs): minting
        // those on every node forked the chain each interval and was the sole
        // reason the old validator_enabled gate existed. With empty heartbeats
        // gone, every node runs the producer safely and the chain converges on
        // the heaviest valid chain. Quiet periods (nothing to carry) simply
        // produce no block — including pre-genesis, until the operator's
        // bootstrap puts the founder GRANT in the mempool, which is when the
        // first block (genesis) gets minted.
        if (!reg && pending_txs.empty()) continue;
        Block block;
        block.header.version          = BLOCK_VERSION;
        block.header.prev_hash        = chain.tip().hash;
        // Replicated timestamp — the song's submit_ms (song block) or the max
        // included tx submit_ms (tx-only). NEVER `now`: the wall clock only
        // gated maturity above, it never reaches the block bytes (invariant I4).
        block.header.timestamp_ms     = block_ts;
        for (auto& [_, tx_data] : pending_txs)
            block.transactions.push_back(tx_data);
        block.header.merkle_root      = Block::compute_merkle_root(block.transactions);

        if (reg) {
            block.has_song = true;
            block.song.audio_format         = reg->audio_format;
            block.song.content_hash         = reg->content_hash;
            block.song.compressed_fingerprint = reg->compressed_fingerprint;
            block.song.duration_ms          = reg->duration_ms;
            block.song.title                = reg->title;
            block.song.artist               = reg->artist;
            block.song.artist_address       = reg->artist_address;
            block.song.genre                = reg->genre;
            block.song.album                = reg->album;
            block.song.year                 = reg->year;
            block.song.track_number         = reg->track_number;
            block.song.royalty_splits       = reg->royalty_splits;
            block.header.content_hash       = reg->content_hash;
            // Bug fix: always recompute fingerprint_hash from the actual
            // compressed bytes we're about to ship. Block::validate
            // checks that header.fingerprint_hash ==
            // sha256(song.compressed_fingerprint); trusting the reg's
            // stored fph used to break the block when a player's
            // claimed hash disagreed with the bytes (or when the
            // compressed format was renormalized somewhere along the
            // path). Recomputing here makes validate a tautology.
            block.header.fingerprint_hash   = crypto::sha256(
                reinterpret_cast<const uint8_t*>(reg->compressed_fingerprint.data()),
                reg->compressed_fingerprint.size());
        }
        // Heartbeat (no song): header.content_hash / fingerprint_hash
        // stay zero — see Block::validate.

        std::string err;
        if (!commit_block(block, chain, db, network, cfg, keypair,
                          pending_txs, err)) {
            std::cerr << "[chain] block commit failed: " << err << "\n";
            {
                std::lock_guard<std::mutex> lk(producer_mu_);
                last_block_at_ms_ = now; // back off so we don't spin
            }
            // ---- Liveness backstop: isolate + evict a poison tx ---------
            //
            // The build-time applicability gate already dropped txs that won't
            // apply, so a failure here is normally a raced tip (prev_hash) or a
            // duplicate song — neither a tx fault. But if the tip advanced under
            // us and a now-included tx became unappliable, isolate exactly that
            // one and evict it (mirroring the song retry/drop) so it can't wedge
            // the chain across the race. If no tx is at fault (idx < 0) fall
            // through to the song handling. Never penalizes the song for a bad
            // tx — the song stays selectable.
            if (!pending_txs.empty()) {
                std::vector<std::vector<uint8_t>> raws;
                raws.reserve(pending_txs.size());
                for (auto& p : pending_txs) raws.push_back(p.second);
                const int bad = chain.first_unappliable_tx(raws);
                if (bad >= 0) {
                    std::cerr << "[chain] evicting unappliable tx after commit "
                                 "failure "
                              << crypto::to_hex(pending_txs[bad].first).substr(0, 12)
                              << "…\n";
                    db.del_pending_tx(pending_txs[bad].first);
                    continue;   // retry without it; the song (if any) is untouched
                }
            }
            // ---- Bug fix #8 / #26 ---------------------------------
            //
            // A song block that fails to connect is usually a duplicate
            // (another node registered the same song first, so song_on_chain
            // is now true) or carries a bad tx. Bump the selected song's retry
            // counter IN the buffer (map lookup by content_hash) and drop it
            // once it is on chain or has burned 3 attempts, so one bad
            // submission can't wedge the chain forever. It stays selectable
            // (lowest content_hash first) until then; the selection scan skips
            // retries >= 3. The `reg` we hold is a copy, so the counter must
            // live on the map entry, not on it.
            if (reg) {
                const bool dup = song_on_chain(db, reg->content_hash,
                                               reg->compressed_fingerprint);
                std::lock_guard<std::mutex> rlk(regs_mutex_);
                auto it = pending_songs_.find(reg->content_hash);
                if (it != pending_songs_.end()) {
                    it->second.retries++;
                    if (dup || it->second.retries >= 3) {
                        if (dup)
                            std::cerr << "[chain] dropping duplicate song reg "
                                      << crypto::to_hex(reg->content_hash).substr(0, 12)
                                      << "…\n";
                        else
                            std::cerr << "[chain] dropping reg after "
                                      << static_cast<unsigned>(it->second.retries)
                                      << " retries\n";
                        db.del_pending_song(it->first);
                        pending_songs_.erase(it);
                    }
                }
            }
            continue;
        }

        // Success — the block connected. Drop the minted song from the buffer
        // (its sp: row already fell inside connect_block's batch) and set
        // minted_last so the loop goes straight back to drain the next mature
        // item with no wait (back-to-back album mints).
        if (reg) {
            std::lock_guard<std::mutex> rlk(regs_mutex_);
            pending_songs_.erase(reg->content_hash);
        }
        minted_last = true;

        if (reg) {
            std::cout << "[chain] block " << chain.tip().height
                      << " registered \"" << reg->title << "\" by "
                      << reg->artist << " (ch="
                      << crypto::to_hex(reg->content_hash).substr(0, 12)
                      << ", " << block.transactions.size() << " tx)\n";
        } else {
            std::cout << "[heartbeat] block " << chain.tip().height
                      << " emitted with " << block.transactions.size()
                      << " tx (empty fingerprint)\n";
        }
    }
}

} // namespace mc
