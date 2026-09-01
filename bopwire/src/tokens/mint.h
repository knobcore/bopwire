#pragma once
#include <cstdint>
#include "../core/transaction.h"
#include "../storage/database.h"
#include "ledger.h"

namespace mc {

// ---- PlayProof v3 co-signature activation (CONSENSUS) ----------------
//
// Stage 1 shipped the v3 wire format: the proof carries slots for a LISTENER
// co-signature and a MINI-NODE (relay) co-signature, and check_play verifies
// either one *if the matching pubkey is non-zero*. Nobody signed, so every
// v3 proof on chain up to now carries zero co-signatures and the "no single
// party can forge a play" property did not actually hold — a serving node
// alone could attest a play naming any player_address.
//
// Stage 2 makes the signatures FLOW (session.complete -> session.cosign, and
// the mini-node's proof.cosign verb) and adds this height gate so they can be
// made MANDATORY without invalidating one byte of history:
//
//   height <  COSIGN_ACTIVATION_HEIGHT : verify-if-present. Byte-for-byte the
//       pre-Stage-2 rule, so every block already on chain (v1, v2, and
//       v3-with-zero-cosigs) re-validates exactly as it did before.
//   height >= COSIGN_ACTIVATION_HEIGHT : the listener co-signature becomes
//       MANDATORY, and the mini-node co-signature becomes mandatory for any
//       proof that claims a paid relay lane (mini_node_address != 0). The
//       governing invariant is "no lane pays an address that did not sign".
//
// !! DELIBERATELY NOT ACTIVATED !!  UINT32_MAX means "never". Flipping this to
// a real height is a HARD FORK: every node must be running a build with the
// same value before that height, and every LISTENER must be running a player
// that co-signs, or their plays stop minting (session.complete refuses with
// 426 rather than emitting a mint consensus would reject). Activation is a
// deliberate, scheduled, operator decision — see the Stage 2 report. Until
// then the node-local BOPWIRE_REQUIRE_COSIGN=1 switch lets an operator refuse
// to *originate* un-co-signed mints without touching consensus at all.
static constexpr uint32_t COSIGN_ACTIVATION_HEIGHT = 0xFFFFFFFFu;

// Compute mint outputs for a given play proof.
// Uses current play_count from database to determine reward tier.
// Applies royalty splits to artist share.
std::vector<MintOutput> compute_mint_outputs(const PlayProof& proof,
                                              const SongSection& song,
                                              uint64_t play_count,
                                              const Hash256& serving_node_id,
                                              const Address& serving_node_address);

// Proof-level validity of a single play (shared by the per-play MINT and, later,
// each constituent of a batched settlement):
//   - serving node signature verifies against the "v:" validator registry
//   - session_id not already used on chain
//   - duration >= 30s and heartbeat count plausible
//   - listener / mini-node co-signatures per COSIGN_ACTIVATION_HEIGHT above
// Reads only replicated state plus the block height, so it is identical on
// every node. `height` is the height of the block the proof is being applied
// in (tip+1 for a mempool preflight); it ONLY selects the co-signature rule.
bool check_play(const PlayProof& proof, const Database& db,
                uint32_t height, std::string& error);

// Re-derive the authoritative mint amounts for a play from ON-CHAIN state:
// resolves the serving-node wallet from the "v:" registry (bound to the same
// pubkey check_play verified) and the SongSection from the block store, reads
// the live play_count + total_supply, and returns compute_mint_outputs + the
// burn. Pure function of committed state -> every node recomputes the same
// numbers, so a node can never inflate or redirect a mint.
bool recompute_mint(const PlayProof& proof, const Database& db,
                    std::vector<MintOutput>& outputs, uint64_t& burn,
                    std::string& error);

// Full consensus validation of a MINT tx before it may enter the mempool or a
// block: check_play(proof) AND the declared outputs+burn EQUAL recompute_mint.
// Run identically in the mempool preflight and at block-apply.
bool validate_mint(const MintTx& mint, const Database& db,
                   uint32_t height, std::string& error);

} // namespace mc
