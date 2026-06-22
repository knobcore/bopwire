/*
 * chain_glue.cpp — extern "C" verbs the web player's TypeScript layer
 * calls into via Module.cwrap() to build, sign, and validate chain-
 * level data without going to the home node.
 *
 * Mirrors the way wallet_glue.c re-exports libwally + secp256k1 for
 * key derivation, but exposes the chain-core verbs the Flutter player
 * and Android JNI surface natively:
 *
 *   mc_web_chain_transfer_sign_message    — preimage bytes the wallet
 *                                            then SHA256s + signs
 *   mc_web_chain_username_sign_message    — same shape for username
 *                                            registration
 *   mc_web_chain_to_checksum_hex          — EIP-55 mixed-case display
 *   mc_web_chain_parse_address            — validate user input,
 *                                            reject bad checksum
 *   mc_web_chain_sha256_hex               — 32-byte hash of arbitrary
 *                                            bytes (session ids etc.)
 *   mc_web_chain_validate_hex_hash        — 1 if 64-char hex, else 0
 *
 * Outputs are malloc'd null-terminated hex strings — caller frees with
 * Module._free(). NULL returns mean "bad input or internal error";
 * the TypeScript wrapper turns those into thrown errors.
 *
 * Why this lives in C++ rather than C like wallet_glue: we call into
 * the mc::TransferTx / mc::UsernameTx structs from musicchain's core,
 * which are C++ types with std::vector member fields. Wrapping those
 * here costs nothing — the extern "C" boundary keeps Module.cwrap
 * happy.
 */
#include "../../musicchain/src/core/block.h"
#include "../../musicchain/src/core/transaction.h"
#include "../../musicchain/src/crypto/hash.h"
#include "../../musicchain/src/crypto/keys.h"

#include <wally_core.h>

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

extern "C" {

// Hook into chain_shim.cpp without dragging the secp256k1 type into
// the mc::crypto namespace from this TU. shim_init_secp creates the
// context lazily; calling it is idempotent.
void mc_web_chain_shim_init_secp();

static int g_wally_initialised = 0;
static void chain_init_once() {
    if (!g_wally_initialised) {
        if (wally_init(0) == WALLY_OK) g_wally_initialised = 1;
    }
    mc_web_chain_shim_init_secp();
}

// ---- Small helpers -------------------------------------------------

static const char kHexLut[] = "0123456789abcdef";

static char* dup_cstr(const std::string& s) {
    char* p = (char*)std::malloc(s.size() + 1);
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

static char* bytes_to_hex_cstr(const uint8_t* data, size_t len) {
    char* out = (char*)std::malloc(len * 2 + 1);
    if (!out) return nullptr;
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = kHexLut[data[i] >> 4];
        out[2 * i + 1] = kHexLut[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

// Hex → bytes. Accepts optional 0x prefix. Returns true on success.
// Output buffer must hold (hex_len - prefix) / 2 bytes; caller must
// know the expected length up front (we use this for fixed 20/32/33-
// byte fields).
static bool hex_to_fixed(const char* hex, uint8_t* out, size_t expected_len) {
    if (!hex || !out) return false;
    size_t hlen = std::strlen(hex);
    if (hlen >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex  += 2;
        hlen -= 2;
    }
    if (hlen != expected_len * 2) return false;
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < expected_len; ++i) {
        int hi = nyb(hex[2 * i]);
        int lo = nyb(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// strtoull wrapper that rejects empty / non-digit input. Used for the
// amount + nonce string args — passing them as strings rather than
// JS numbers is the only way to preserve full u64 precision through
// the wasm boundary (Number can only represent 2^53 exactly).
static bool parse_u64_str(const char* s, uint64_t* out) {
    if (!s || !*s || !out) return false;
    char* endp = nullptr;
    unsigned long long v = std::strtoull(s, &endp, 10);
    if (!endp || *endp != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

// ---- Verbs ---------------------------------------------------------

/** TransferTx sign-message preimage as hex.
 *
 *  Inputs:
 *    from_hex   — 20-byte source address, 0x-prefixed or bare hex
 *    to_hex     — 20-byte destination
 *    amount_str — internal-unit amount as decimal string (8 decimals,
 *                 so "1.5 MC" = "150000000")
 *    nonce_str  — per-sender nonce as decimal string
 *
 *  Returns malloc'd hex string (104 chars + NUL — chain_id(4) +
 *  from(20) + to(20) + amount(8) + nonce(8) = 60 bytes = 120 hex,
 *  written little-endian per the chain wire format). NULL on bad
 *  inputs. */
char* mc_web_chain_transfer_sign_message(const char* from_hex,
                                          const char* to_hex,
                                          const char* amount_str,
                                          const char* nonce_str) {
    chain_init_once();
    mc::TransferTx tx{};
    if (!hex_to_fixed(from_hex, tx.from_address.data(), 20)) return nullptr;
    if (!hex_to_fixed(to_hex,   tx.to_address.data(),   20)) return nullptr;
    if (!parse_u64_str(amount_str, &tx.amount))             return nullptr;
    if (!parse_u64_str(nonce_str,  &tx.nonce))              return nullptr;
    // signature stays zero — sign_message is the bytes the *signer*
    // hashes, not the wire bytes that include the signature.
    auto msg = tx.sign_message();
    return bytes_to_hex_cstr(msg.data(), msg.size());
}

/** UsernameTx sign-message preimage as hex.
 *
 *  Inputs:
 *    name           — 3..30 ASCII chars, [a-z0-9_]+, must start with
 *                     a letter (chain rules enforce; this verb does
 *                     basic length sanity only)
 *    owner_hex      — 20-byte owner address
 *    owner_pubkey_hex — 33-byte compressed pubkey
 *    nonce_str      — decimal string
 *
 *  Returns malloc'd hex string. NULL on bad inputs. */
char* mc_web_chain_username_sign_message(const char* name,
                                          const char* owner_hex,
                                          const char* owner_pubkey_hex,
                                          const char* nonce_str) {
    chain_init_once();
    if (!name) return nullptr;
    size_t nlen = std::strlen(name);
    if (nlen < 1 || nlen > 255) return nullptr; // wire layout uses u8 length
    mc::UsernameTx tx{};
    tx.name.assign(name, nlen);
    if (!hex_to_fixed(owner_hex,        tx.owner.data(),        20)) return nullptr;
    if (!hex_to_fixed(owner_pubkey_hex, tx.owner_pubkey.data(), 33)) return nullptr;
    if (!parse_u64_str(nonce_str, &tx.nonce))                       return nullptr;
    auto msg = tx.sign_message();
    return bytes_to_hex_cstr(msg.data(), msg.size());
}

/** EIP-55 checksummed display string for a 20-byte address.
 *
 *  Input is the bare or 0x-prefixed hex form (any case — checksum is
 *  recomputed from the underlying bytes). Returns "0x" + 40 mixed-
 *  case hex chars + NUL. NULL if the input isn't 20 hex-encoded bytes. */
char* mc_web_chain_to_checksum_hex(const char* addr_hex) {
    chain_init_once();
    mc::Address a{};
    if (!hex_to_fixed(addr_hex, a.data(), 20)) return nullptr;
    auto s = mc::crypto::to_checksum_hex(a);
    return dup_cstr(s);
}

/** Validate user-entered address. Accepts:
 *    * 0x-prefixed or bare 40-char hex
 *    * all-lowercase or all-uppercase (no checksum verification)
 *    * mixed-case (checksum IS verified — typos rejected)
 *
 *  On success returns malloc'd 40-char lowercase hex (no 0x prefix)
 *  so the caller has the canonical address bytes to feed into other
 *  verbs. NULL on parse error or bad checksum. */
char* mc_web_chain_parse_address(const char* addr_in) {
    chain_init_once();
    if (!addr_in) return nullptr;
    mc::Address a{};
    if (!mc::crypto::parse_address_checksummed(addr_in, a)) return nullptr;
    return bytes_to_hex_cstr(a.data(), 20);
}

/** Plain SHA-256 of arbitrary bytes, returned as 64-char hex.
 *  Used for hashing 32-byte session ids and other per-stream nonces
 *  the player generates client-side. */
char* mc_web_chain_sha256_hex(const uint8_t* data, int len) {
    chain_init_once();
    if (!data || len < 0) return nullptr;
    auto h = mc::crypto::sha256(data, (size_t)len);
    return bytes_to_hex_cstr(h.data(), 32);
}

/** Returns 1 if the input is a 64-char lowercase/uppercase hex
 *  string (optionally 0x-prefixed), else 0. Cheap sanity check the
 *  UI uses before submitting a content_hash to the home node. */
int mc_web_chain_validate_hex_hash(const char* hex) {
    if (!hex) return 0;
    size_t hlen = std::strlen(hex);
    if (hlen >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex  += 2;
        hlen -= 2;
    }
    if (hlen != 64) return 0;
    for (size_t i = 0; i < hlen; ++i) {
        char c = hex[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return 0;
    }
    return 1;
}

} // extern "C"
