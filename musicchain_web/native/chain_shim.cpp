/*
 * chain_shim.cpp — minimal reimplementation of the musicchain symbols
 * transaction.cpp links against, but without dragging OpenSSL or
 * block.cpp / chain.cpp / leveldb into the WASM build.
 *
 * What's here:
 *   - mc::write_*le / read_*le / write_bytes / read_bytes / write_string16 /
 *     read_string16    — bit-identical copies of the helpers in block.cpp
 *   - mc::crypto::sha256(…)               — via libwally's wally_sha256
 *   - mc::crypto::address_from_pubkey     — uncompress (secp256k1) +
 *                                            keccak256(X||Y)[12..32]
 *   - mc::crypto::verify_ecdsa            — libsecp256k1 (kept so a
 *                                            future verb can call it)
 *   - mc::crypto::verify_ecdsa_from_address — secp256k1 recovery
 *   - mc::crypto::to_checksum_hex         — EIP-55 mixed case via keccak256
 *   - mc::crypto::parse_address_checksummed — accepts 0x-prefixed/bare,
 *                                              all-one-case bypasses
 *                                              checksum (legacy form),
 *                                              mixed-case verifies it
 *
 * The transaction.cpp TU itself is unchanged — it sees the same C++
 * namespaces it would on the desktop build. The verify_signature()
 * paths inside that TU end up dead-stripped because nothing in the
 * web glue calls them, but emcc still needs the symbols declared so
 * compilation succeeds.
 */
#include "../../musicchain/src/core/block.h"
#include "../../musicchain/src/crypto/hash.h"
#include "../../musicchain/src/crypto/keys.h"
#include "../../musicchain/src/crypto/signature.h"
#include "../../musicchain/src/crypto/keccak256_c.h"

#include <wally_core.h>
#include <wally_crypto.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace mc {

// ---- Serialization helpers (copied verbatim from block.cpp) --------

void write_u16le(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void write_u32le(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void write_u64le(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
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
    for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(p[i]) << (8 * i));
    p += 4; return true;
}

bool read_u64le(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (end - p < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(p[i]) << (8 * i));
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

namespace crypto {

// One global secp256k1 context for sign / verify / recovery. Same
// pattern the wallet_glue uses for BIP32; init_once() in chain_glue.cpp
// wires this up before any verb runs.
static secp256k1_context* g_secp_ctx = nullptr;
static secp256k1_context* shim_get_secp_ctx_impl() {
    if (!g_secp_ctx) {
        g_secp_ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    }
    return g_secp_ctx;
}
} // namespace crypto
} // namespace mc

// extern "C" hook so chain_glue.cpp can prime the context at first-use
// without needing to forward-declare secp256k1_context's type inside
// the mc::crypto namespace.
extern "C" void mc_web_chain_shim_init_secp() {
    (void)mc::crypto::shim_get_secp_ctx_impl();
}

namespace mc {
namespace crypto {
secp256k1_context* shim_get_secp_ctx() { return shim_get_secp_ctx_impl(); }

Hash256 sha256(const uint8_t* data, size_t len) {
    Hash256 out{};
    // wally_sha256 returns WALLY_OK and writes 32 bytes; we already
    // sized out at 32 so a failure path just leaves zeros, which is
    // fine because the WASM build is the only caller and len/data
    // come from already-validated buffers inside the glue.
    (void)wally_sha256(data, len, out.data(), out.size());
    return out;
}

Hash256 sha256(const std::vector<uint8_t>& data) {
    return sha256(data.data(), data.size());
}

Hash256 sha256(const std::string& data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Hash256 sha256d(const uint8_t* data, size_t len) {
    auto first = sha256(data, len);
    return sha256(first.data(), 32);
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char lut[] = "0123456789abcdef";
    std::string out(len * 2, '\0');
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = lut[(data[i] >> 4) & 0xF];
        out[2 * i + 1] = lut[ data[i]       & 0xF];
    }
    return out;
}

std::string to_hex(const Hash256& h) { return to_hex(h.data(), 32); }
std::string to_hex(const std::vector<uint8_t>& v) { return to_hex(v.data(), v.size()); }

std::vector<uint8_t> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nyb(hex[i]), lo = nyb(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool parse_hash256(const std::string& hex, Hash256& out) {
    auto b = from_hex(hex);
    if (b.size() != 32) return false;
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

bool parse_address(const std::string& hex, Address& out) {
    std::string h = hex;
    if (h.size() == 42 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
        h = h.substr(2);
    }
    auto b = from_hex(h);
    if (b.size() != 20) return false;
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

Address escrow_address_for(const Address& artist) {
    std::vector<uint8_t> seed;
    static const char kTag[] = "escrow:";
    seed.insert(seed.end(), kTag, kTag + sizeof(kTag) - 1);
    seed.insert(seed.end(), artist.begin(), artist.end());
    Hash256 h = sha256(seed.data(), seed.size());
    Address out{};
    std::copy(h.begin(), h.begin() + 20, out.begin());
    return out;
}

Address address_from_pubkey(const PubKey33& pubkey) {
    secp256k1_context* ctx = shim_get_secp_ctx();
    secp256k1_pubkey pub;
    // Parse can fail on a malformed pubkey; callers (transaction.cpp
    // verify paths + the recover flow above) always check downstream,
    // so ignore the warn_unused_result attribute here.
    (void)secp256k1_ec_pubkey_parse(ctx, &pub, pubkey.data(), 33);
    uint8_t uncompressed[65];
    size_t  outlen = 65;
    (void)secp256k1_ec_pubkey_serialize(ctx, uncompressed, &outlen, &pub,
                                        SECP256K1_EC_UNCOMPRESSED);
    uint8_t h[32];
    mc_keccak256(uncompressed + 1, 64, h);
    Address out{};
    std::copy(h + 12, h + 32, out.begin());
    return out;
}

EthAddress eth_address_from_pubkey(const PubKey33& pubkey) {
    auto a = address_from_pubkey(pubkey);
    EthAddress out{};
    std::copy(a.begin(), a.end(), out.begin());
    return out;
}

// EIP-55 mixed-case checksum — copied byte-for-byte from keys.cpp so
// addresses displayed by the web player match the chain / Android /
// desktop builds. Uses the vendored keccak256_c rather than the C++
// `keccak256` to avoid dragging keys.cpp's OpenSSL header chain in.
std::string to_checksum_hex(const Address& addr) {
    static const char lut[] = "0123456789abcdef";
    std::string lower(40, '\0');
    for (size_t i = 0; i < 20; ++i) {
        lower[2 * i]     = lut[(addr[i] >> 4) & 0xF];
        lower[2 * i + 1] = lut[ addr[i]       & 0xF];
    }
    uint8_t hash[32];
    mc_keccak256(reinterpret_cast<const uint8_t*>(lower.data()),
                 lower.size(), hash);
    std::string out;
    out.reserve(42);
    out.append("0x");
    for (size_t i = 0; i < 40; ++i) {
        uint8_t hash_nibble = (i % 2 == 0)
                                ? (hash[i / 2] >> 4) & 0xF
                                :  hash[i / 2]       & 0xF;
        char c = lower[i];
        if (hash_nibble >= 8 && c >= 'a' && c <= 'f') c -= 32;
        out.push_back(c);
    }
    return out;
}

bool parse_address_checksummed(const std::string& s, Address& out) {
    std::string h = s;
    if (h.size() == 42 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
        h = h.substr(2);
    }
    if (h.size() != 40) return false;
    bool has_upper = false, has_lower = false;
    for (char c : h) {
        if (c >= 'a' && c <= 'f') has_lower = true;
        else if (c >= 'A' && c <= 'F') has_upper = true;
        else if (!((c >= '0' && c <= '9'))) return false;
    }
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < 20; ++i) {
        int hi = nyb(h[2 * i]);
        int lo = nyb(h[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (has_upper && has_lower) {
        auto canonical = to_checksum_hex(out);
        std::string want = canonical.substr(2);
        if (want != h) return false;
    }
    return true;
}

// ---- ECDSA verify (used by tx verify_signature paths) ----------------
//
// The web verbs we ship today only call sign_message(), not
// verify_signature() — but transaction.cpp's TU references these
// symbols at link time, so we keep working implementations rather
// than stubs. Dead-code elimination drops them from the final wasm
// when nothing actually calls them.

bool verify_ecdsa(const Hash256& hash, const Sig64& sig, const PubKey33& pubkey) {
    secp256k1_context* ctx = shim_get_secp_ctx();
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pubkey.data(), 33)) return false;
    secp256k1_ecdsa_signature s;
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &s, sig.data())) return false;
    return secp256k1_ecdsa_verify(ctx, &s, hash.data(), &pub) == 1;
}

bool verify_ecdsa_from_address(const Hash256& hash, const Sig64& sig,
                                const Address& expected_address) {
    secp256k1_context* ctx = shim_get_secp_ctx();
    // Try both recid 0 and 1; secp256k1 recovery is a search over the
    // possible curve points that match the given (r, s, recid).
    for (int recid = 0; recid < 2; ++recid) {
        secp256k1_ecdsa_recoverable_signature rsig;
        if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
                ctx, &rsig, sig.data(), recid)) continue;
        secp256k1_pubkey pub;
        if (!secp256k1_ecdsa_recover(ctx, &pub, &rsig, hash.data())) continue;
        uint8_t compressed[33];
        size_t  outlen = 33;
        secp256k1_ec_pubkey_serialize(ctx, compressed, &outlen, &pub,
                                      SECP256K1_EC_COMPRESSED);
        PubKey33 pk33;
        std::copy(compressed, compressed + 33, pk33.begin());
        Address derived = address_from_pubkey(pk33);
        if (std::memcmp(derived.data(), expected_address.data(), 20) == 0)
            return true;
    }
    return false;
}

Sig64 sign_ecdsa(const Hash256& hash, const std::vector<uint8_t>& priv_key) {
    Sig64 out{};
    secp256k1_context* ctx = shim_get_secp_ctx();
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_sign(ctx, &sig, hash.data(), priv_key.data(), nullptr, nullptr))
        return out;
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);
    uint8_t compact[64];
    secp256k1_ecdsa_signature_serialize_compact(ctx, compact, &sig);
    std::copy(compact, compact + 64, out.begin());
    return out;
}

Sig64 sign_data(const uint8_t* data, size_t len,
                const std::vector<uint8_t>& priv_key) {
    auto h = sha256(data, len);
    return sign_ecdsa(h, priv_key);
}

bool verify_data(const uint8_t* data, size_t len, const Sig64& sig,
                 const PubKey33& pubkey) {
    auto h = sha256(data, len);
    return verify_ecdsa(h, sig, pubkey);
}

} // namespace crypto
} // namespace mc
