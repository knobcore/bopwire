#pragma once
#include "block.h"
#include <cstdint>
#include <vector>

namespace mc {

// Transaction type tags
enum class TxType : uint8_t {
    TRANSFER      = 0x01,
    MINT          = 0x10,
    MODERATOR_OP  = 0x20,
};

// Moderator op codes (sub-type inside MODERATOR_OP).
//
// Deliberately tiny: moderator identity on chain is just (address, level,
// pubkey). No human-readable handles are ever recorded — moderators
// authenticate by raw key material so their pseudonyms can't leak via
// the public chain or the operator UI.
enum class ModOpCode : uint8_t {
    GRANT  = 1,  // raise subject to `level`
    REVOKE = 2,  // strip moderator status from subject
};

// Moderator hierarchy levels.
//
//   FOUNDER (3) is the only level that can grant OP or REVOKE anyone,
//   and is set exactly once via the bootstrap self-grant. There is
//   exactly one FOUNDER per chain. The founder identifies by key
//   material only — there is no on-chain name for them.
//
//   OP (2) can propose hides / escrow releases and votes on proposals.
//   VOICE (1) is observer-only for now — placeholder for future
//   write-but-not-vote roles.
enum class ModLevel : uint8_t {
    NONE    = 0,
    VOICE   = 1,
    OP      = 2,
    FOUNDER = 3,
};

// ---- Transfer Transaction -------------------------------------------

struct TransferTx {
    Address  from_address;
    Address  to_address;
    uint64_t amount;       // internal units (8 decimals)
    uint64_t nonce;
    Sig64    signature;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, TransferTx& out);

    // Message bytes that are signed: from|to|amount|nonce
    std::vector<uint8_t> sign_message() const;
    Hash256 tx_hash() const;
    bool verify_signature() const;
};

// ---- Play Proof (embedded in MintTx) --------------------------------

struct PlayProof {
    Hash256  session_id;
    Hash256  content_hash;
    Hash256  block_hash;
    Address  artist_address;
    Address  player_address;
    Hash256  serving_node_id;
    uint64_t play_start_timestamp;
    uint64_t play_end_timestamp;
    uint32_t total_duration_ms;
    uint16_t heartbeat_count;
    Sig64    node_signature;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, PlayProof& out);

    // Bytes over which node_signature is computed
    std::vector<uint8_t> sign_message() const;
};

// ---- Mint output ---------------------------------------------------

struct MintOutput {
    Address  recipient;
    uint64_t amount;
};

// ---- Mint Transaction ----------------------------------------------

struct MintTx {
    PlayProof               proof;
    std::vector<MintOutput> outputs;     // computed at processing time
    uint64_t                burn_amount = 0; // tokens burned from proof.player_address (post-50k)

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, MintTx& out);
    Hash256 tx_hash() const;
};

// ---- Moderator op transaction ---------------------------------------
//
// One transaction type covers every moderator-system mutation: granting
// or revoking moderator status. The op_code byte distinguishes which
// mutation is being made.
//
// Sign-and-verify covers everything between `op_code` and
// `proposer_pubkey` inclusive. Chain rules in chain.cpp enforce who is
// allowed to issue each op based on the proposer's current level.
//
// Deliberately no alias / handle / nickname field — moderator identity
// on chain is just (address, level, pubkey). Keeping names off the
// wire means a peer with a copy of the chain learns moderation power
// but never learns the operator's chosen handle.

struct ModeratorOpTx {
    uint8_t   op_code         = 0;     // ModOpCode value
    uint8_t   level           = 0;     // ModLevel value (0 for REVOKE)
    Address   subject{};               // who this op acts on
    PubKey33  subject_pubkey{};        // 33-byte compressed pubkey of subject (zero for REVOKE)
    Address   proposer{};              // who is issuing the op (== subject for self-bootstrap)
    PubKey33  proposer_pubkey{};       // proposer's pubkey for signature recovery
    uint64_t  nonce           = 0;     // per-proposer replay protection
    Sig64     signature{};             // ECDSA(sign_message())

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, ModeratorOpTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Generic transaction wrapper -----------------------------------

struct Transaction {
    TxType type;
    std::vector<uint8_t> raw; // raw serialized bytes including type byte

    Hash256 tx_hash() const;

    static Transaction from_transfer(const TransferTx& tx);
    static Transaction from_mint(const MintTx& tx);
    static Transaction from_moderator_op(const ModeratorOpTx& tx);

    bool parse_transfer(TransferTx& out) const;
    bool parse_mint(MintTx& out) const;
    bool parse_moderator_op(ModeratorOpTx& out) const;
};

} // namespace mc
