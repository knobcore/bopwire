#include "candidate.h"
#include "../core/chain.h"
#include "../storage/database.h"
#include "../network/manager.h"
#include "../network/messages.h"
#include "../audio/fingerprint.h"
#include "../audio/ogg_validator.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include <chrono>
#include <algorithm>
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

// ---- BlockCandidate ------------------------------------------------

bool BlockCandidate::is_expired() const {
    return (now_ms_c() - created_at_ms) > uint64_t(BLOCK_TIMEOUT_SECONDS) * 1000;
}

// ---- CandidateManager: candidate tracking --------------------------

void CandidateManager::add_candidate(const std::string& candidate_hash,
                                      BlockCandidate candidate) {
    std::lock_guard<std::mutex> lk(mutex_);
    candidates_[candidate_hash] = std::move(candidate);
}

bool CandidateManager::add_confirmation(const std::string& candidate_hash,
                                         const Confirmation& conf) {
    bool final = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = candidates_.find(candidate_hash);
        if (it == candidates_.end()) return false;
        auto& cand = it->second;
        for (const auto& c : cand.received_confirmations)
            if (c.validator_id == conf.validator_id) return false;
        cand.received_confirmations.push_back(conf);
        cand.block.header.confirmations = cand.received_confirmations;
        final = cand.is_final();
    }
    if (final) confirm_cv_.notify_all();
    return final;
}

std::optional<BlockCandidate> CandidateManager::get_candidate(
    const std::string& hash) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = candidates_.find(hash);
    if (it == candidates_.end()) return std::nullopt;
    return it->second;
}

void CandidateManager::cleanup_expired() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = candidates_.begin(); it != candidates_.end(); ) {
        if (it->second.is_expired())
            it = candidates_.erase(it);
        else
            ++it;
    }
}

std::vector<std::pair<std::string, BlockCandidate>> CandidateManager::get_all() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return {candidates_.begin(), candidates_.end()};
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
        // Seed the heartbeat clock so we don't immediately fire on boot —
        // give the node a fresh 5-minute window after process start.
        last_block_at_ms_ = now_ms_c();
    }
    heartbeat_thread_ = std::thread([this, &chain, &db, &network, &cfg, &keypair] {
        heartbeat_loop(chain, db, network, cfg, keypair);
    });
}

void CandidateManager::stop() {
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        running_ = false;
    }
    confirm_cv_.notify_all();
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

bool CandidateManager::enqueue_registration(PendingRegistration reg) {
    {
        std::lock_guard<std::mutex> lk(regs_mutex_);
        // De-dup against in-flight queue by BOTH content_hash and
        // fingerprint_hash. Without the fingerprint check, two players
        // submitting the same song in slightly different encodings could
        // race past the content-hash gate and both mint blocks before
        // either one made it into the index.
        std::queue<PendingRegistration> tmp = pending_regs_;
        while (!tmp.empty()) {
            const auto& q = tmp.front();
            if (q.content_hash == reg.content_hash) return false;
            if (q.fingerprint_hash == reg.fingerprint_hash) return false;
            tmp.pop();
        }
        pending_regs_.push(std::move(reg));
    }
    // Nudge the heartbeat loop to flush the next block immediately rather
    // than waiting up to HEARTBEAT_INTERVAL_MS.
    {
        std::lock_guard<std::mutex> lk(producer_mu_);
        last_block_at_ms_ = 0; // force the timer check to fire
    }
    heartbeat_cv_.notify_all();
    return true;
}

size_t CandidateManager::pending_registration_count() const {
    std::lock_guard<std::mutex> lk(regs_mutex_);
    return pending_regs_.size();
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

    std::string block_hash_hex = crypto::to_hex(block.hash());

    BlockCandidate candidate;
    candidate.block          = block;
    candidate.created_at_ms  = now_ms_c();
    add_candidate(block_hash_hex, candidate);

    bool confirmed = false;
    auto deadline  = std::chrono::steady_clock::now()
                   + std::chrono::seconds(BLOCK_TIMEOUT_SECONDS);

    // Solo mode: self-sign REQUIRED_CONFIRMATIONS times so the block
    // becomes final immediately. Once a real validator set exists this
    // branch goes away and we always wait on confirm_cv_.
    if (network.peer_count() == 0) {
        auto block_hash_bytes = block.hash();
        for (uint32_t i = 0; i < REQUIRED_CONFIRMATIONS; ++i) {
            Confirmation self_conf;
            self_conf.validator_id = cfg.node_id;
            std::copy(keypair.public_key.begin(), keypair.public_key.end(),
                      self_conf.pubkey.begin());
            self_conf.signature = crypto::sign_ecdsa(block_hash_bytes,
                                                      keypair.private_key);
            add_confirmation(block_hash_hex, self_conf);
        }
        confirmed = true;
    } else {
        std::unique_lock<std::mutex> lk(mutex_);
        confirmed = confirm_cv_.wait_until(lk, deadline, [&] {
            auto it = candidates_.find(block_hash_hex);
            return it != candidates_.end() && it->second.is_final();
        });
    }
    if (!confirmed) { err = "Confirmation timeout"; return false; }

    Block final_block = block;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = candidates_.find(block_hash_hex);
        if (it != candidates_.end())
            final_block.header.confirmations = it->second.received_confirmations;
    }

    if (!chain.connect_block(final_block)) {
        err = "Chain connect_block rejected";
        return false;
    }

    block = final_block; // Caller sees the committed form (with confirmations).
    uint32_t height = chain.tip().height;

    try {
        fs::path blocks_dir = fs::path(cfg.data_dir) / "blocks";
        fs::create_directories(blocks_dir);
        std::ostringstream fname;
        fname << std::setw(8) << std::setfill('0') << height << ".blk";
        fs::path file_path = blocks_dir / fname.str();
        auto block_bytes = final_block.serialize();
        std::ofstream f(file_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(block_bytes.data()),
                block_bytes.size());
    } catch (...) {
        // Non-fatal — block is durable in LevelDB even if the .blk write fails.
    }

    for (const auto& [tx_hash, _] : consumed_txs)
        db.del_pending_tx(tx_hash);

    (void)network; // confirmed-block broadcast lives in network/manager.cpp now

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
    while (true) {
        // If the queue has work, skip the wait entirely — we want
        // back-to-back mints so a 14-track album scan doesn't take
        // 7 minutes to show up on chain (one every wake-up tick).
        bool queue_has_pending = false;
        {
            std::lock_guard<std::mutex> lk(regs_mutex_);
            queue_has_pending = !pending_regs_.empty();
        }
        if (!queue_has_pending) {
            std::unique_lock<std::mutex> lk(producer_mu_);
            heartbeat_cv_.wait_for(lk, std::chrono::seconds(30), [this] {
                std::lock_guard<std::mutex> rlk(regs_mutex_);
                return !running_ || !pending_regs_.empty();
            });
            if (!running_) return;
        }

        // Drain one registration if available.
        std::optional<PendingRegistration> reg;
        {
            std::lock_guard<std::mutex> lk(regs_mutex_);
            if (!pending_regs_.empty()) {
                reg = std::move(pending_regs_.front());
                pending_regs_.pop();
            }
        }

        uint64_t last_at;
        {
            std::lock_guard<std::mutex> lk(producer_mu_);
            last_at = last_block_at_ms_;
        }

        const uint64_t now = now_ms_c();
        // No registration AND heartbeat window not yet elapsed → nothing
        // to do this tick.
        if (!reg && (now - last_at < HEARTBEAT_INTERVAL_MS)) continue;

        auto pending_txs = db.get_all_pending_txs();
        Block block;
        block.header.version          = BLOCK_VERSION;
        block.header.prev_hash        = chain.tip().hash;
        block.header.timestamp_ms     = now;
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
            block.header.fingerprint_hash   = reg->fingerprint_hash;
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
            // Re-queue the registration we drained so it isn't lost.
            if (reg) {
                std::lock_guard<std::mutex> rlk(regs_mutex_);
                pending_regs_.push(std::move(*reg));
            }
            continue;
        }

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
