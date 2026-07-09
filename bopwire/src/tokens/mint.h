#pragma once
#include <cstdint>
#include "../core/transaction.h"
#include "../storage/database.h"
#include "ledger.h"

namespace mc {

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
// Reads only replicated state, so it is identical on every node.
bool check_play(const PlayProof& proof, const Database& db, std::string& error);

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
bool validate_mint(const MintTx& mint, const Database& db, std::string& error);

} // namespace mc
