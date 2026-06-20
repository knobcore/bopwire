#include "mint.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"

namespace mc {

std::vector<MintOutput> compute_mint_outputs(const PlayProof& proof,
                                              const SongSection& song,
                                              uint64_t play_count,
                                              const Hash256& /*serving_node_id*/,
                                              const Address& serving_node_address) {
    std::vector<MintOutput> outputs;
    const Address zero_addr{};

    if (play_count < FULL_REWARD_THRESHOLD) {
        // Pre-10k tier:
        //   * Artist share → ESCROW (released by moderator). Routed to a
        //     deterministic escrow_address_for(artist) so the existing
        //     moderator-transfer endpoint can release it on approval —
        //     no chain-format change needed for the in-block MintTx.
        //   * Serving node → 1 token (spendable).
        //   * Discoverer (the listener who triggered this play) → 1 token.
        uint64_t artist_share     = FULL_ARTIST_REWARD;
        uint64_t node_share       = FULL_NODE_REWARD;
        uint64_t discoverer_share = FULL_DISCOVERER_REWARD;

        if (!song.royalty_splits.empty()) {
            // Each royalty-split recipient gets their proportional share
            // routed through their own escrow account.
            for (const auto& rs : song.royalty_splits) {
                uint64_t portion = (artist_share * rs.basis_points) / 10000;
                if (portion > 0)
                    outputs.push_back({crypto::escrow_address_for(rs.address),
                                       portion});
            }
        } else {
            // Songs registered without an artist_address (older player
            // builds that omitted the field) land here with all-zero
            // artist_address; escrow_address_for(zero) is a deterministic
            // "unclaimed" escrow that a moderator can release once the
            // artist is identified.
            outputs.push_back({crypto::escrow_address_for(song.artist_address),
                               artist_share});
        }
        outputs.push_back({serving_node_address, node_share});

        if (discoverer_share > 0 && proof.player_address != zero_addr)
            outputs.push_back({proof.player_address, discoverer_share});

    } else {
        // Post-10k tier: artist and node each get 1 token (no escrow;
        // 10k+ plays have passed the moderator-discovery period). The
        // listener no longer earns a discoverer credit and instead
        // BURNS a dynamic amount that scales with total supply — the
        // caller (post_session_complete) populates burn_amount from
        // compute_burn_rate(total_supply) and apply_mint() debits it
        // from the player wallet plus refuses mints above the 2B cap.
        uint64_t artist_share = FULL_ARTIST_REWARD;
        uint64_t node_share   = FULL_NODE_REWARD;

        if (!song.royalty_splits.empty()) {
            for (const auto& rs : song.royalty_splits) {
                uint64_t portion = (artist_share * rs.basis_points) / 10000;
                if (portion > 0)
                    outputs.push_back({rs.address, portion});
            }
        } else if (song.artist_address != zero_addr) {
            outputs.push_back({song.artist_address, artist_share});
        } else {
            // No artist registered AND we're past 10k plays so escrow is
            // gone: route through the deterministic unclaimed escrow so a
            // late claim still has somewhere to release from.
            outputs.push_back({crypto::escrow_address_for(song.artist_address),
                               artist_share});
        }
        outputs.push_back({serving_node_address, node_share});
    }

    return outputs;
}

bool validate_mint(const MintTx& mint, const Database& db, std::string& error) {
    const auto& proof = mint.proof;

    // Check session not already used
    if (db.is_session_used(proof.session_id)) {
        error = "session_id already used";
        return false;
    }

    // Check minimum duration
    if (proof.total_duration_ms < 30000) {
        error = "play duration under 30 seconds";
        return false;
    }

    // Check heartbeat count plausibility: at least 1 heartbeat per 35 seconds
    uint32_t expected_min = proof.total_duration_ms / 35000;
    if (proof.heartbeat_count < expected_min) {
        error = "insufficient heartbeat count";
        return false;
    }

    // Verify node signature over proof data
    auto sign_msg = proof.sign_message();
    auto hash     = crypto::sha256(sign_msg.data(), sign_msg.size());

    // Look up validator public key from registry
    auto node_entry = db.get("v:" + db.hex(proof.serving_node_id));
    if (!node_entry) {
        error = "serving node not registered";
        return false;
    }
    // node_entry format: 33 bytes pubkey + 4 bytes endpoint length + endpoint string
    if (node_entry->size() < 33) {
        error = "invalid validator entry";
        return false;
    }
    PubKey33 pubkey;
    std::copy(node_entry->begin(), node_entry->begin() + 33, pubkey.begin());

    if (!crypto::verify_ecdsa(hash, proof.node_signature, pubkey)) {
        error = "invalid node signature";
        return false;
    }

    return true;
}

} // namespace mc
