#include "mint.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include "../crypto/keys.h"   // address_from_pubkey (H6 serving-node wallet bind)

namespace mc {

std::vector<MintOutput> compute_mint_outputs(const PlayProof& proof,
                                              const SongSection& song,
                                              uint64_t play_count,
                                              const Hash256& /*serving_node_id*/,
                                              const Address& serving_node_address) {
    std::vector<MintOutput> outputs;
    const Address zero_addr{};

    // BUG FIX: validate that royalty_splits basis_points sum to <=
    // 10000 (100%). The previous code took whatever the song
    // declared and minted artist_share * bp / 10000 per entry; a
    // malformed registration with 11000 bp total would mint 110% of
    // the artist share, breaking the supply curve. Sums summing to
    // less than 100% used to silently lose tokens — the remainder
    // never got credited anywhere — so we route the unused share to
    // the zero-artist escrow. Empty splits is the normal
    // single-artist case.
    uint64_t splits_bp_sum = 0;
    for (const auto& rs : song.royalty_splits) splits_bp_sum += rs.basis_points;
    const bool valid_splits = !song.royalty_splits.empty()
                            && splits_bp_sum <= 10000;
    const uint64_t leftover_bp = valid_splits ? (10000 - splits_bp_sum) : 0;

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

        if (valid_splits) {
            // Each royalty-split recipient gets their proportional
            // share routed through their own escrow account.
            for (const auto& rs : song.royalty_splits) {
                if (rs.address == zero_addr) continue;
                uint64_t portion = (artist_share * rs.basis_points) / 10000;
                if (portion > 0)
                    outputs.push_back({crypto::escrow_address_for(rs.address),
                                       portion});
            }
            // Any sub-100% remainder routes to the unclaimed escrow.
            if (leftover_bp > 0) {
                const uint64_t portion = (artist_share * leftover_bp) / 10000;
                if (portion > 0)
                    outputs.push_back(
                        {crypto::escrow_address_for(zero_addr), portion});
            }
        } else {
            // Songs registered without an artist_address (older
            // player builds that omitted the field) land here with
            // all-zero artist_address; escrow_address_for(zero) is a
            // deterministic "unclaimed" escrow that a moderator can
            // release once the artist is identified.
            outputs.push_back({crypto::escrow_address_for(song.artist_address),
                               artist_share});
        }
        if (serving_node_address != zero_addr)
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

        if (valid_splits) {
            for (const auto& rs : song.royalty_splits) {
                if (rs.address == zero_addr) continue;
                uint64_t portion = (artist_share * rs.basis_points) / 10000;
                if (portion > 0)
                    outputs.push_back({rs.address, portion});
            }
            if (leftover_bp > 0) {
                const uint64_t portion = (artist_share * leftover_bp) / 10000;
                if (portion > 0)
                    outputs.push_back(
                        {crypto::escrow_address_for(zero_addr), portion});
            }
        } else if (song.artist_address != zero_addr) {
            outputs.push_back({song.artist_address, artist_share});
        } else {
            // No artist registered AND we're past 10k plays so the
            // escrow is gone: route through the deterministic
            // unclaimed escrow so a late claim still has somewhere
            // to release from.
            outputs.push_back({crypto::escrow_address_for(song.artist_address),
                               artist_share});
        }
        if (serving_node_address != zero_addr)
            outputs.push_back({serving_node_address, node_share});
    }

    // Per-stream lanes (PlayProof v2) — apply to BOTH tiers. Legacy (v1) proofs
    // carry zero addresses here, so these are skipped and old plays mint exactly
    // as before.
    //   * Seeder: the player that uploaded the bytes. Skipped when it equals the
    //     listener (proof.player_address) so a player can't self-seed and
    //     double-dip with the discoverer lane.
    //   * Mini-node: the relay that carried the stream — a flat per-stream credit
    //     that replaces the old per-byte RelayRewardTx.
    if (proof.seeder_address != zero_addr &&
        proof.seeder_address != proof.player_address)
        outputs.push_back({proof.seeder_address, FULL_SEEDER_REWARD});
    if (proof.mini_node_address != zero_addr)
        outputs.push_back({proof.mini_node_address, FULL_MININODE_REWARD});

    return outputs;
}

bool check_play(const PlayProof& proof, const Database& db,
                uint32_t height, std::string& error) {
    // Session not already used on chain (replay/double-credit guard).
    if (db.is_session_used(proof.session_id)) {
        error = "session_id already used";
        return false;
    }
    // Minimum duration.
    if (proof.total_duration_ms < 30000) {
        error = "play duration under 30 seconds";
        return false;
    }
    // Heartbeat count plausibility: at least 1 heartbeat per 35 seconds.
    uint32_t expected_min = proof.total_duration_ms / 35000;
    if (proof.heartbeat_count < expected_min) {
        error = "insufficient heartbeat count";
        return false;
    }
    // ---- v3 three-party co-signature (NO founder v: grant) -------------------
    // The serving-node pubkey travels IN the proof; the identity is bound by
    // serving_node_id == sha256(pubkey). Any node can attest a play — forgery is
    // prevented by the LISTENER + MINI co-signatures, not by a founder whitelist.
    if (proof.version < 3) {
        error = "legacy proof: founder v: grant retired, v3 co-signed proof required";
        return false;
    }
    Hash256 derived_node_id = crypto::sha256(proof.serving_node_pubkey.data(), 33);
    if (std::memcmp(derived_node_id.data(), proof.serving_node_id.data(), 32) != 0) {
        error = "serving_node_id != sha256(serving_node_pubkey)";
        return false;
    }
    auto sign_msg = proof.sign_message();
    auto hash     = crypto::sha256(sign_msg.data(), sign_msg.size());
    if (!crypto::verify_ecdsa(hash, proof.node_signature, proof.serving_node_pubkey)) {
        error = "invalid node signature";
        return false;
    }

    static const PubKey33 kZeroPubkey{};
    static const Address  kZeroAddr{};

    // ---- Stage 2 co-signature gate (see COSIGN_ACTIVATION_HEIGHT) -----------
    // Below the activation height this is EXACTLY the pre-Stage-2 rule
    // ("verify if a pubkey is carried"), so every historical block — v1, v2,
    // and the v3-with-zero-cosigs blocks currently on chain — re-validates
    // byte-for-byte as it always did. At and above it, the presence of the
    // co-signatures becomes mandatory. The governing invariant is:
    //
    //     NO LANE PAYS AN ADDRESS THAT DID NOT SIGN.
    //
    // node lane   -> node_signature        (always mandatory, checked above)
    // listener    -> player_signature      (mandatory once activated)
    // relay lane  -> mini_node_signature   (mandatory once activated, IFF the
    //                proof actually claims a paid mini_node_address; a direct,
    //                un-relayed play carries a zero relay lane, pays nobody for
    //                it, and so needs no relay signature)
    //
    // The SEEDER lane has no signature slot in the v3 wire format, so it is
    // instead established server-side and never from client input — see
    // HttpServer::post_session_complete / RatsApi::resolve_delivery_lanes.
    const bool cosign_active = (height >= COSIGN_ACTIVATION_HEIGHT);

    // Listener co-signature — the EARNER authorizes the mint, so a serving node
    // can't fabricate a play (it lacks the player key).
    //
    // Required only when the proof actually CLAIMS a listener lane, exactly as
    // the relay rule below works. A web listener has no wallet of its own — the
    // gateway plays on their behalf and player_address is zero — so that play
    // pays the artist, the seeder and the relay but NOT a listener. Nobody is
    // paid for the listener lane, so nobody needs to sign for it, and the
    // invariant "no lane pays an address that did not sign" still holds
    // exactly.
    //
    // This is what makes activation possible at all: requiring a listener
    // signature unconditionally would permanently stop every web play from
    // minting, since a browser listener has no key to sign with.
    const bool has_player_pubkey =
        std::memcmp(proof.player_pubkey.data(), kZeroPubkey.data(), 33) != 0;
    const bool claims_player_lane =
        std::memcmp(proof.player_address.data(), kZeroAddr.data(), 20) != 0;
    if (cosign_active && claims_player_lane && !has_player_pubkey) {
        error = "listener co-signature required";
        return false;
    }
    // A proof carrying a listener pubkey but no listener lane would be paying
    // nobody while asserting an identity — reject it rather than let an
    // unpaid-but-signed identity ride along. Gated on activation like every
    // other Stage 2 rule: below the gate this file must stay byte-for-byte the
    // pre-Stage-2 validator, so it cannot introduce a NEW rejection reason for
    // history. (Vacuous on the current chain — no historical proof carries a
    // listener pubkey at all — but the guarantee is what keeps it that way.)
    if (cosign_active && has_player_pubkey && !claims_player_lane) {
        error = "listener pubkey present but no listener lane claimed";
        return false;
    }
    if (has_player_pubkey) {
        if (crypto::address_from_pubkey(proof.player_pubkey) != proof.player_address) {
            error = "player_pubkey does not match player_address";
            return false;
        }
        if (!crypto::verify_ecdsa(hash, proof.player_signature, proof.player_pubkey)) {
            error = "invalid listener signature";
            return false;
        }
    }
    // Mini-node co-signature — the RELAY attests it carried the stream.
    const bool has_mini_pubkey =
        std::memcmp(proof.mini_node_pubkey.data(), kZeroPubkey.data(), 33) != 0;
    const bool claims_mini_lane =
        std::memcmp(proof.mini_node_address.data(), kZeroAddr.data(), 20) != 0;
    if (cosign_active && claims_mini_lane && !has_mini_pubkey) {
        error = "mini-node co-signature required for a paid relay lane";
        return false;
    }
    if (has_mini_pubkey) {
        if (crypto::address_from_pubkey(proof.mini_node_pubkey) != proof.mini_node_address) {
            error = "mini_node_pubkey does not match mini_node_address";
            return false;
        }
        if (!crypto::verify_ecdsa(hash, proof.mini_node_signature, proof.mini_node_pubkey)) {
            error = "invalid mini-node signature";
            return false;
        }
    }
    return true;
}

bool recompute_mint(const PlayProof& proof, const Database& db,
                    std::vector<MintOutput>& outputs, uint64_t& burn,
                    std::string& error) {
    // Serving-node wallet is BOUND to the pubkey carried in the proof (v3),
    // whose sha256 == serving_node_id (checked in check_play): the node that
    // signed the proof is the node that earns the node lane — a mint can't
    // redirect the node reward to an arbitrary address. No v: registry lookup.
    const Address serving_node_address =
        crypto::address_from_pubkey(proof.serving_node_pubkey);

    // Authoritative SongSection from the block store (artist_address +
    // royalty_splits). SongMeta/sm: does not carry these, so we read the block.
    auto song = db.get_song_section(proof.content_hash);
    if (!song) {
        error = "song not on chain";
        return false;
    }
    const uint64_t play_count = db.get_play_count(proof.content_hash);
    outputs = compute_mint_outputs(proof, *song, play_count,
                                   proof.serving_node_id, serving_node_address);
    burn = (play_count >= FULL_REWARD_THRESHOLD)
        ? compute_burn_rate(db.get_total_supply())
        : 0;
    return true;
}

bool validate_mint(const MintTx& mint, const Database& db,
                   uint32_t height, std::string& error) {
    if (!check_play(mint.proof, db, height, error)) return false;

    // Forge gate: the declared outputs + burn MUST equal the recomputation from
    // committed state. A registered node can attest that a play happened, but it
    // cannot inflate the amounts or redirect them to other wallets — every
    // number is a pure function of the signed proof + on-chain song/supply,
    // recomputed identically on every validating node.
    std::vector<MintOutput> outs;
    uint64_t                exp_burn = 0;
    if (!recompute_mint(mint.proof, db, outs, exp_burn, error)) return false;

    if (mint.burn_amount != exp_burn) { error = "mint burn mismatch"; return false; }
    if (mint.outputs.size() != outs.size()) { error = "mint output count mismatch"; return false; }
    for (size_t i = 0; i < outs.size(); ++i) {
        if (mint.outputs[i].recipient != outs[i].recipient ||
            mint.outputs[i].amount    != outs[i].amount) {
            error = "mint output mismatch";
            return false;
        }
    }
    return true;
}

} // namespace mc
