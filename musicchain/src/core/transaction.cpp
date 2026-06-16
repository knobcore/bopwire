#include "transaction.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include <cstring>

namespace mc {

// ---- TransferTx -----------------------------------------------------

std::vector<uint8_t> TransferTx::sign_message() const {
    std::vector<uint8_t> msg;
    msg.insert(msg.end(), from_address.begin(), from_address.end());
    msg.insert(msg.end(), to_address.begin(), to_address.end());
    write_u64le(msg, amount);
    write_u64le(msg, nonce);
    return msg;
}

std::vector<uint8_t> TransferTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::TRANSFER));
    write_bytes(buf, from_address.data(), 20);
    write_bytes(buf, to_address.data(), 20);
    write_u64le(buf, amount);
    write_u64le(buf, nonce);
    write_bytes(buf, signature.data(), 64);
    return buf;
}

bool TransferTx::deserialize(const uint8_t* data, size_t len, TransferTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    // skip type byte
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::TRANSFER)) return false;
    if (!read_bytes(p, end, out.from_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.to_address.data(), 20)) return false;
    if (!read_u64le(p, end, out.amount)) return false;
    if (!read_u64le(p, end, out.nonce)) return false;
    if (!read_bytes(p, end, out.signature.data(), 64)) return false;
    return true;
}

Hash256 TransferTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool TransferTx::verify_signature() const {
    auto msg  = sign_message();
    auto hash = crypto::sha256(msg.data(), msg.size());
    // Derive pubkey from from_address for verification
    // For simplicity the from_address is derived as last 20 bytes of sha256(pubkey)
    // Full ECDSA verification is in crypto::verify_ecdsa
    return crypto::verify_ecdsa_from_address(hash, signature, from_address);
}

// ---- PlayProof ------------------------------------------------------

std::vector<uint8_t> PlayProof::sign_message() const {
    std::vector<uint8_t> msg;
    write_bytes(msg, session_id.data(), 32);
    write_bytes(msg, content_hash.data(), 32);
    write_bytes(msg, block_hash.data(), 32);
    write_bytes(msg, artist_address.data(), 20);
    write_bytes(msg, player_address.data(), 20);
    write_bytes(msg, serving_node_id.data(), 32);
    write_u64le(msg, play_start_timestamp);
    write_u64le(msg, play_end_timestamp);
    write_u32le(msg, total_duration_ms);
    write_u16le(msg, heartbeat_count);
    return msg;
}

std::vector<uint8_t> PlayProof::serialize() const {
    std::vector<uint8_t> buf;
    write_bytes(buf, session_id.data(), 32);
    write_bytes(buf, content_hash.data(), 32);
    write_bytes(buf, block_hash.data(), 32);
    write_bytes(buf, artist_address.data(), 20);
    write_bytes(buf, player_address.data(), 20);
    write_bytes(buf, serving_node_id.data(), 32);
    write_u64le(buf, play_start_timestamp);
    write_u64le(buf, play_end_timestamp);
    write_u32le(buf, total_duration_ms);
    write_u16le(buf, heartbeat_count);
    write_bytes(buf, node_signature.data(), 64);
    return buf;
}

bool PlayProof::deserialize(const uint8_t* data, size_t len, PlayProof& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (!read_bytes(p, end, out.session_id.data(), 32)) return false;
    if (!read_bytes(p, end, out.content_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.block_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.artist_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.player_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.serving_node_id.data(), 32)) return false;
    if (!read_u64le(p, end, out.play_start_timestamp)) return false;
    if (!read_u64le(p, end, out.play_end_timestamp)) return false;
    if (!read_u32le(p, end, out.total_duration_ms)) return false;
    if (!read_u16le(p, end, out.heartbeat_count)) return false;
    if (!read_bytes(p, end, out.node_signature.data(), 64)) return false;
    return true;
}

// ---- MintTx ---------------------------------------------------------

std::vector<uint8_t> MintTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::MINT));
    auto proof_bytes = proof.serialize();
    write_u32le(buf, static_cast<uint32_t>(proof_bytes.size()));
    buf.insert(buf.end(), proof_bytes.begin(), proof_bytes.end());
    write_u32le(buf, static_cast<uint32_t>(outputs.size()));
    for (const auto& o : outputs) {
        write_bytes(buf, o.recipient.data(), 20);
        write_u64le(buf, o.amount);
    }
    write_u64le(buf, burn_amount);
    return buf;
}

bool MintTx::deserialize(const uint8_t* data, size_t len, MintTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::MINT)) return false;
    uint32_t proof_len = 0;
    if (!read_u32le(p, end, proof_len)) return false;
    if (static_cast<size_t>(end - p) < proof_len) return false;
    if (!PlayProof::deserialize(p, proof_len, out.proof)) return false;
    p += proof_len;
    uint32_t out_count = 0;
    if (!read_u32le(p, end, out_count)) return false;
    out.outputs.resize(out_count);
    for (auto& o : out.outputs) {
        if (!read_bytes(p, end, o.recipient.data(), 20)) return false;
        if (!read_u64le(p, end, o.amount)) return false;
    }
    out.burn_amount = 0;
    if (p + 8 <= end) read_u64le(p, end, out.burn_amount); // optional field, backward compat
    return true;
}

Hash256 MintTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

// ---- ModeratorOpTx --------------------------------------------------

std::vector<uint8_t> ModeratorOpTx::sign_message() const {
    // Excludes `signature`. Bytes are laid out as in `serialize()` minus
    // the type byte (so the chain wire format is `serialize()` =
    // `type_byte || sign_message() || signature`).
    std::vector<uint8_t> msg;
    msg.push_back(op_code);
    msg.push_back(level);
    write_bytes(msg, subject.data(),         20);
    write_bytes(msg, subject_pubkey.data(),  33);
    write_bytes(msg, proposer.data(),        20);
    write_bytes(msg, proposer_pubkey.data(), 33);
    write_u64le(msg, nonce);
    return msg;
}

std::vector<uint8_t> ModeratorOpTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::MODERATOR_OP));
    auto body = sign_message();
    buf.insert(buf.end(), body.begin(), body.end());
    write_bytes(buf, signature.data(), 64);
    return buf;
}

bool ModeratorOpTx::deserialize(const uint8_t* data, size_t len,
                                 ModeratorOpTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::MODERATOR_OP)) return false;
    if (p >= end) return false; out.op_code = *p++;
    if (p >= end) return false; out.level   = *p++;
    if (!read_bytes(p, end, out.subject.data(),         20)) return false;
    if (!read_bytes(p, end, out.subject_pubkey.data(),  33)) return false;
    if (!read_bytes(p, end, out.proposer.data(),        20)) return false;
    if (!read_bytes(p, end, out.proposer_pubkey.data(), 33)) return false;
    if (!read_u64le(p, end, out.nonce))                      return false;
    if (!read_bytes(p, end, out.signature.data(),       64)) return false;
    return true;
}

Hash256 ModeratorOpTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool ModeratorOpTx::verify_signature() const {
    auto  msg  = sign_message();
    auto  hash = crypto::sha256(msg.data(), msg.size());
    // The proposer's pubkey is carried inline (so the chain doesn't
    // need a separate registry just to verify), but we cross-check
    // that the pubkey actually hashes to the proposer address — that
    // closes the door on a malicious tx that signs against a pubkey
    // that isn't the proposer's.
    Address derived = crypto::address_from_pubkey(proposer_pubkey);
    if (std::memcmp(derived.data(), proposer.data(), 20) != 0) return false;
    return crypto::verify_ecdsa(hash, signature, proposer_pubkey);
}

// ---- Transaction wrapper -------------------------------------------

Transaction Transaction::from_transfer(const TransferTx& tx) {
    Transaction t;
    t.type = TxType::TRANSFER;
    t.raw  = tx.serialize();
    return t;
}

Transaction Transaction::from_mint(const MintTx& tx) {
    Transaction t;
    t.type = TxType::MINT;
    t.raw  = tx.serialize();
    return t;
}

Transaction Transaction::from_moderator_op(const ModeratorOpTx& tx) {
    Transaction t;
    t.type = TxType::MODERATOR_OP;
    t.raw  = tx.serialize();
    return t;
}

bool Transaction::parse_transfer(TransferTx& out) const {
    return TransferTx::deserialize(raw.data(), raw.size(), out);
}

bool Transaction::parse_mint(MintTx& out) const {
    return MintTx::deserialize(raw.data(), raw.size(), out);
}

bool Transaction::parse_moderator_op(ModeratorOpTx& out) const {
    return ModeratorOpTx::deserialize(raw.data(), raw.size(), out);
}

Hash256 Transaction::tx_hash() const {
    return crypto::sha256(raw.data(), raw.size());
}

} // namespace mc
