#include "block.h"
#include "../crypto/hash.h"
#include <cstring>
#include <stdexcept>

namespace mc {

// ---- Serialization helpers ------------------------------------------

void write_u16le(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void write_u32le(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
}

void write_u64le(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
}

void write_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

void write_string16(std::vector<uint8_t>& buf, const std::string& s) {
    write_u16le(buf, static_cast<uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

bool read_u16le(const uint8_t*& p, const uint8_t* end, uint16_t& v) {
    if (end - p < 2) return false;
    v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2; return true;
}

bool read_u32le(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(p[i]) << (8*i));
    p += 4; return true;
}

bool read_u64le(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (end - p < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(p[i]) << (8*i));
    p += 8; return true;
}

bool read_bytes(const uint8_t*& p, const uint8_t* end, uint8_t* dst, size_t len) {
    if (static_cast<size_t>(end - p) < len) return false;
    std::memcpy(dst, p, len);
    p += len; return true;
}

bool read_string16(const uint8_t*& p, const uint8_t* end, std::string& s) {
    uint16_t len = 0;
    if (!read_u16le(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;
    s.assign(reinterpret_cast<const char*>(p), len);
    p += len; return true;
}

// ---- BlockHeader ----------------------------------------------------

std::vector<uint8_t> BlockHeader::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(256);
    write_u32le(buf, version);
    write_bytes(buf, prev_hash.data(),        32);
    write_bytes(buf, merkle_root.data(),      32);
    write_bytes(buf, fingerprint_hash.data(), 32);
    write_bytes(buf, content_hash.data(),     32);
    write_u64le(buf, timestamp_ms);
    // v4+: state_root + song_body_hash folded into the block-hash preimage (P4).
    if (version >= 4) {
        write_bytes(buf, state_root.data(),     32);
        write_bytes(buf, song_body_hash.data(), 32);
    }
    return buf;
}

Hash256 BlockHeader::hash() const {
    auto hdr = serialize();
    return crypto::sha256(hdr.data(), hdr.size());
}

// ---- Block serialization --------------------------------------------

// Canonical byte encoding of the song body — the SINGLE source of truth for
// both Block::serialize (below) and Block::compute_song_body_hash, so the bytes
// that go on the wire and the bytes that are hashed into header.song_body_hash
// can never drift apart. Every reward-affecting field (audio_format,
// artist_address, royalty_splits) is included, so hashing these bytes binds the
// whole SongSection to the block id.
static void write_song_body(std::vector<uint8_t>& buf, const SongSection& song) {
    buf.push_back(static_cast<uint8_t>(song.audio_format));
    write_bytes(buf, song.content_hash.data(), 32);
    write_string16(buf, song.compressed_fingerprint);
    write_u32le(buf, song.duration_ms);
    write_string16(buf, song.title);
    write_string16(buf, song.artist);
    write_bytes(buf, song.artist_address.data(), 20);
    write_string16(buf, song.genre);
    write_string16(buf, song.album);
    // ID3-style optional fields — 0 means "not provided".
    write_u16le(buf, song.year);
    write_u16le(buf, song.track_number);
    buf.push_back(static_cast<uint8_t>(song.royalty_splits.size()));
    for (const auto& rs : song.royalty_splits) {
        write_bytes(buf, rs.address.data(), 20);
        write_u16le(buf, rs.basis_points);
    }
}

Hash256 Block::compute_song_body_hash() const {
    if (!has_song) return Hash256{};   // zero on heartbeat (like content_hash)
    std::vector<uint8_t> b;
    write_song_body(b, song);
    return crypto::sha256(b.data(), b.size());
}

std::vector<uint8_t> Block::serialize() const {
    std::vector<uint8_t> buf;

    // Header
    auto hdr = header.serialize();
    buf.insert(buf.end(), hdr.begin(), hdr.end());

    // Optional song record
    buf.push_back(has_song ? 0x01 : 0x00);
    if (has_song) write_song_body(buf, song);

    // Separator
    for (size_t i = 0; i < SEPARATOR_LENGTH; ++i) buf.push_back(SEPARATOR_BYTE);

    // Transaction section
    write_u32le(buf, static_cast<uint32_t>(transactions.size()));
    for (const auto& tx : transactions) {
        write_u32le(buf, static_cast<uint32_t>(tx.size()));
        buf.insert(buf.end(), tx.begin(), tx.end());
    }

    return buf;
}

bool Block::deserialize(const uint8_t* data, size_t len, Block& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;

    // --- Header ---
    if (!read_u32le(p, end, out.header.version)) return false;
    if (!read_bytes(p, end, out.header.prev_hash.data(),        32)) return false;
    if (!read_bytes(p, end, out.header.merkle_root.data(),      32)) return false;
    if (!read_bytes(p, end, out.header.fingerprint_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.header.content_hash.data(),     32)) return false;
    if (!read_u64le(p, end, out.header.timestamp_ms))               return false;
    // v4+: read state_root + song_body_hash (left zero for legacy v3, though the
    // clean-slate fork means every block on the fresh chain is v4).
    if (out.header.version >= 4) {
        if (!read_bytes(p, end, out.header.state_root.data(),     32)) return false;
        if (!read_bytes(p, end, out.header.song_body_hash.data(), 32)) return false;
    }

    // --- Optional song record ---
    if (p >= end) return false;
    out.has_song = (*p++ != 0);
    if (out.has_song) {
        if (p >= end) return false;
        out.song.audio_format = static_cast<AudioFormat>(*p++);
        if (!read_bytes(p, end, out.song.content_hash.data(), 32)) return false;
        if (!read_string16(p, end, out.song.compressed_fingerprint)) return false;
        if (!read_u32le(p, end, out.song.duration_ms))               return false;
        if (!read_string16(p, end, out.song.title))                  return false;
        if (!read_string16(p, end, out.song.artist))                 return false;
        if (!read_bytes(p, end, out.song.artist_address.data(), 20)) return false;
        if (!read_string16(p, end, out.song.genre))                  return false;
        if (!read_string16(p, end, out.song.album))                  return false;
        if (!read_u16le(p, end, out.song.year))                      return false;
        if (!read_u16le(p, end, out.song.track_number))              return false;
        if (p >= end) return false;
        uint8_t rs_count = *p++;
        out.song.royalty_splits.resize(rs_count);
        for (auto& rs : out.song.royalty_splits) {
            if (!read_bytes(p, end, rs.address.data(), 20)) return false;
            if (!read_u16le(p, end, rs.basis_points))      return false;
        }
    }

    // --- Separator ---
    if (static_cast<size_t>(end - p) < SEPARATOR_LENGTH) return false;
    for (size_t i = 0; i < SEPARATOR_LENGTH; ++i) {
        if (p[i] != SEPARATOR_BYTE) return false;
    }
    p += SEPARATOR_LENGTH;

    // --- Transaction section ---
    uint32_t tx_count = 0;
    if (!read_u32le(p, end, tx_count)) return false;
    out.transactions.resize(tx_count);
    for (auto& tx : out.transactions) {
        uint32_t tx_len = 0;
        if (!read_u32le(p, end, tx_len)) return false;
        if (static_cast<size_t>(end - p) < tx_len) return false;
        tx.assign(p, p + tx_len);
        p += tx_len;
    }

    return true;
}

Hash256 Block::compute_merkle_root(const std::vector<std::vector<uint8_t>>& txs) {
    if (txs.empty()) {
        Hash256 zero{};
        return zero;
    }
    std::vector<Hash256> hashes;
    hashes.reserve(txs.size());
    for (const auto& tx : txs)
        hashes.push_back(crypto::sha256(tx.data(), tx.size()));

    while (hashes.size() > 1) {
        if (hashes.size() % 2 != 0)
            hashes.push_back(hashes.back());
        std::vector<Hash256> next;
        next.reserve(hashes.size() / 2);
        for (size_t i = 0; i < hashes.size(); i += 2) {
            std::vector<uint8_t> combined;
            combined.insert(combined.end(), hashes[i].begin(), hashes[i].end());
            combined.insert(combined.end(), hashes[i+1].begin(), hashes[i+1].end());
            next.push_back(crypto::sha256(combined.data(), combined.size()));
        }
        hashes = std::move(next);
    }
    return hashes[0];
}

Hash256 Block::full_hash(const std::vector<uint8_t>& serialized) {
    return crypto::sha256(serialized.data(), serialized.size());
}

bool Block::validate() const {
    // ---- Version pin (clean-slate v4 fork) ---------------------------
    // Every block on this chain is exactly BLOCK_VERSION. Rejecting any other
    // version here — the single choke point run on BOTH connect and rebuild —
    // makes the `version >= 4` conditionals in serialize()/connect_block's
    // state_root gate unconditional in practice and closes the downgrade
    // malleability (set version=3, strip state_root+song_body_hash, and the
    // v4 gates would silently skip).
    if (header.version != BLOCK_VERSION) return false;

    // ---- Phase 5 consensus caps (v4) ---------------------------------
    // Reject an over-cap block before any other work. Enforced identically
    // on connect_block AND rebuild, so an over-cap block is unacceptable
    // network-wide, forever. transactions.size() is O(1); serialize() is
    // O(block) but validate() already implies we're about to apply the
    // whole block, so the extra pass is negligible.
    if (transactions.size() > MAX_TXS_PER_BLOCK) return false;
    if (serialize().size()  > MAX_BLOCK_SIZE)    return false;

    // Heartbeat blocks: no song record, header.fingerprint_hash,
    // header.content_hash and header.song_body_hash must all be zero.
    if (!has_song) {
        Hash256 zero{};
        if (header.fingerprint_hash != zero) return false;
        if (header.content_hash     != zero) return false;
        if (header.song_body_hash   != zero) return false;
    } else {
        // ---- Serialization-round-trip bounds (must precede the hashes) ----
        // Every string16 field encodes its length in a uint16 prefix and the
        // royalty count in a uint8; a field larger than the prefix can hold
        // makes the producer emit bytes no peer can decode (the producer's
        // in-memory validate passes but every peer's deserialize truncates and
        // fails). Rejecting oversize fields HERE means the producer drops the
        // song up front, identically to every peer — no accept/reject split.
        if (song.compressed_fingerprint.size() > 0xFFFF) return false;
        if (song.title.size()  > 0xFFFF) return false;
        if (song.artist.size() > 0xFFFF) return false;
        if (song.genre.size()  > 0xFFFF) return false;
        if (song.album.size()  > 0xFFFF) return false;
        if (song.royalty_splits.size() > 255) return false;

        // Song blocks: header content_hash must match the body's
        // content_hash and the body fingerprint must hash to the header's
        // fingerprint_hash. Audio bytes themselves are not in the block
        // (off-chain content-addressed store), so we don't verify them
        // here — the consumer that fetches them by content_hash will.
        if (header.content_hash != song.content_hash) return false;
        const auto& fp = song.compressed_fingerprint;
        Hash256 fph = crypto::sha256(
            reinterpret_cast<const uint8_t*>(fp.data()), fp.size());
        if (header.fingerprint_hash != fph) return false;
    }
    // Bind the ENTIRE song body (artist_address, royalty_splits, audio_format,
    // metadata) to the block id: a relaying peer that rewrites any song field
    // without recomputing this hash is rejected here, and one that DOES
    // recompute it changes the header bytes -> a different block id -> rejected
    // as "not the block we requested" at ingest. Zero on heartbeat.
    if (header.song_body_hash != compute_song_body_hash()) return false;
    // Verify merkle_root
    if (compute_merkle_root(transactions) != header.merkle_root)
        return false;
    return true;
}

} // namespace mc
