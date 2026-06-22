/*
 * wallet_glue.c — extern "C" entry points the web player's TypeScript
 * wrapper calls into via Module.cwrap(). Same shape as the Android JNI
 * library (musicchain_player/android/app/src/main/cpp/musicchain_android.c):
 *
 *   - BIP39 12-word mnemonic generation + validation through libwally
 *   - mc_wallet_from_mnemonic → BIP32 m/44'/19779'/0'/0/0 → 32-byte
 *     secp256k1 priv key, returned as an opaque handle (just the priv
 *     bytes, allocated on the WASM heap)
 *   - mc_wallet_get_address: keccak256(uncompressed_pubkey[1..65])[12..32]
 *     then EIP-55 mixed-case checksum, returned as "0x" + 40 hex.
 *   - mc_wallet_get_pubkey: 33-byte compressed pubkey as 66 hex
 *   - mc_wallet_sign: secp256k1 ECDSA over sha256(data)
 *
 * All output strings are malloc'd inside this module's heap; the
 * caller frees via Module._free(). All addresses produced here MUST
 * match the ones the Windows DLL and Android .so produce for the same
 * mnemonic — same libwally backend, same derivation path, same
 * keccak256 implementation (vendored at musicchain/src/crypto/keccak256_c.c).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip39.h>
#include <wally_bip32.h>
#include <wally_crypto.h>
#include "../../musicchain/src/crypto/keccak256_c.h"

#include <secp256k1.h>

static secp256k1_context* g_ctx     = NULL;
static int                g_wally_ok = 0;

static const char kHex[] = "0123456789abcdef";

static char* mc_dup_hex(const uint8_t* bytes, size_t n) {
    char* out = (char*)malloc(n * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
    return out;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void init_once(void) {
    if (!g_ctx) {
        g_ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    }
    if (!g_wally_ok) {
        if (wally_init(0) == WALLY_OK) g_wally_ok = 1;
    }
}

/* --------------------------------------------------------------------
 * BIP39
 * ------------------------------------------------------------------ */

/** Generates 16 bytes of entropy via the browser's crypto.getRandomValues
 *  (forwarded into the WASM heap by the TypeScript wrapper). Returns a
 *  freshly-malloc'd 12-word mnemonic string the caller must free with
 *  Module._free(). NULL on entropy failure. */
char* mc_web_bip39_generate_12(const uint8_t* entropy16) {
    init_once();
    if (!entropy16) return NULL;
    /* Re-randomise the secp256k1 context against the same entropy so
     * future signs / verifies aren't using the deterministic-seed
     * default. wally accepts the same buffer twice without copying. */
    (void)wally_secp_randomize(entropy16, 16);
    struct words* word_list = NULL;
    if (bip39_get_wordlist(NULL, &word_list) != WALLY_OK) return NULL;
    char* mnemonic = NULL;
    if (bip39_mnemonic_from_bytes(word_list, entropy16, 16, &mnemonic) != WALLY_OK) {
        return NULL;
    }
    size_t n = strlen(mnemonic);
    char* out = (char*)malloc(n + 1);
    if (out) memcpy(out, mnemonic, n + 1);
    wally_free_string(mnemonic);
    return out;
}

/** Returns 1 if the mnemonic is a valid BIP39 phrase, 0 otherwise. */
int mc_web_bip39_validate(const char* mnemonic) {
    init_once();
    if (!mnemonic) return 0;
    struct words* word_list = NULL;
    if (bip39_get_wordlist(NULL, &word_list) != WALLY_OK) return 0;
    return bip39_mnemonic_validate(word_list, mnemonic) == WALLY_OK ? 1 : 0;
}

/* --------------------------------------------------------------------
 * Wallet handle = pointer to 32 bytes of secp256k1 private key
 * ------------------------------------------------------------------ */

typedef struct { uint8_t priv[32]; } WebWallet;

/** Derive a wallet from (mnemonic, passphrase). passphrase may be NULL
 *  or empty. Returns an opaque handle that callers pass to the other
 *  mc_web_wallet_* functions, or NULL on failure. Free with
 *  mc_web_wallet_free(). */
void* mc_web_wallet_from_mnemonic(const char* mnemonic, const char* passphrase) {
    init_once();
    if (!mnemonic) return NULL;
    unsigned char seed[BIP39_SEED_LEN_512];
    size_t seed_written = 0;
    if (bip39_mnemonic_to_seed(mnemonic, passphrase ? passphrase : "",
                                seed, sizeof(seed),
                                &seed_written) != WALLY_OK) return NULL;
    if (seed_written != BIP39_SEED_LEN_512) return NULL;

    struct ext_key master, child;
    if (bip32_key_from_seed(seed, seed_written,
                             BIP32_VER_MAIN_PRIVATE, 0, &master) != WALLY_OK) {
        return NULL;
    }
    /* m / 44' / 19779' / 0' / 0 / 0 — same SLIP-44 coin index the home
     * node + desktop DLL + Android JNI use. */
    uint32_t path[5] = {
        44u    | BIP32_INITIAL_HARDENED_CHILD,
        19779u | BIP32_INITIAL_HARDENED_CHILD,
        0u     | BIP32_INITIAL_HARDENED_CHILD,
        0u,
        0u,
    };
    if (bip32_key_from_parent_path(&master, path, 5,
                                    BIP32_FLAG_KEY_PRIVATE,
                                    &child) != WALLY_OK) return NULL;

    WebWallet* w = (WebWallet*)malloc(sizeof(WebWallet));
    if (!w) return NULL;
    /* child.priv_key[0] is the BIP32 prefix byte (0x00); priv_key[1..33]
     * is the actual secp256k1 scalar. NO sha256-rehash — the rehash bug
     * was killed across the chain rewrite. */
    memcpy(w->priv, child.priv_key + 1, 32);
    if (!secp256k1_ec_seckey_verify(g_ctx, w->priv)) { free(w); return NULL; }
    return w;
}

void mc_web_wallet_free(void* wallet) {
    if (wallet) free(wallet);
}

/* --------------------------------------------------------------------
 * Address derivation + EIP-55 checksum
 *
 * keccak256(uncompressed_pubkey[1..65])[12..32] → 20 bytes, then mixed-
 * case-checksum via keccak256 of the lowercase hex. Identical to
 * Android's derive_address_keccak / eip55_checksum, and identical to
 * the C++ to_checksum_hex in musicchain/src/crypto/keys.cpp.
 * ------------------------------------------------------------------ */

static void derive_addr20(const uint8_t priv[32], uint8_t addr20_out[20]) {
    secp256k1_pubkey pub;
    uint8_t uncompressed[65];
    size_t  outlen = 65;
    uint8_t hash[32];
    secp256k1_ec_pubkey_create(g_ctx, &pub, priv);
    secp256k1_ec_pubkey_serialize(g_ctx, uncompressed, &outlen, &pub,
                                  SECP256K1_EC_UNCOMPRESSED);
    mc_keccak256(uncompressed + 1, 64, hash);
    memcpy(addr20_out, hash + 12, 20);
}

/** In-place EIP-55 checksum. Caller passes a 40-char buffer containing
 *  lowercase hex; this writes the mixed-case version back to the same
 *  buffer. Exported so JS can use it on already-derived addresses
 *  (e.g. the home node returns plain hex for backward-compat and the
 *  web UI runs the checksum at display time). */
void mc_web_eip55_checksum_inplace(char* addr_hex_40) {
    if (!addr_hex_40) return;
    /* Lowercase guard. */
    for (int i = 0; i < 40; ++i) {
        if (addr_hex_40[i] >= 'A' && addr_hex_40[i] <= 'F')
            addr_hex_40[i] = (char)(addr_hex_40[i] + ('a' - 'A'));
    }
    uint8_t hash[32];
    mc_keccak256((const uint8_t*)addr_hex_40, 40, hash);
    for (int i = 0; i < 40; ++i) {
        if (addr_hex_40[i] >= 'a' && addr_hex_40[i] <= 'f') {
            uint8_t nibble = (i & 1) ? (hash[i / 2] & 0x0f)
                                     : (hash[i / 2] >> 4);
            if (nibble >= 8) addr_hex_40[i] = (char)(addr_hex_40[i] - ('a' - 'A'));
        }
    }
}

/** Returns the wallet's EIP-55 checksummed 0x-prefixed address as a
 *  malloc'd 43-char string ("0x" + 40 hex + NUL). NULL on bad input.
 *  Caller frees via Module._free(). */
char* mc_web_wallet_get_address(void* wallet) {
    init_once();
    if (!wallet) return NULL;
    WebWallet* w = (WebWallet*)wallet;
    uint8_t addr[20];
    derive_addr20(w->priv, addr);
    char* out = (char*)malloc(43);
    if (!out) return NULL;
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 20; ++i) {
        out[2 + i * 2]     = kHex[addr[i] >> 4];
        out[2 + i * 2 + 1] = kHex[addr[i] & 0x0f];
    }
    out[42] = '\0';
    mc_web_eip55_checksum_inplace(out + 2);
    return out;
}

/** 33-byte compressed pubkey as a malloc'd 67-char hex string
 *  (66 hex + NUL). Caller frees. */
char* mc_web_wallet_get_pubkey(void* wallet) {
    init_once();
    if (!wallet) return NULL;
    WebWallet* w = (WebWallet*)wallet;
    secp256k1_pubkey pub;
    uint8_t compressed[33];
    size_t outlen = 33;
    secp256k1_ec_pubkey_create(g_ctx, &pub, w->priv);
    secp256k1_ec_pubkey_serialize(g_ctx, compressed, &outlen, &pub,
                                  SECP256K1_EC_COMPRESSED);
    return mc_dup_hex(compressed, 33);
}

/** ECDSA sign sha256(data). Returns 129-char malloc'd hex (128 hex +
 *  NUL) — the compact 64-byte (r,s) form the chain expects. Caller
 *  frees. */
char* mc_web_wallet_sign(void* wallet, const uint8_t* data, int len) {
    init_once();
    if (!wallet || !data || len <= 0) return NULL;
    WebWallet* w = (WebWallet*)wallet;
    uint8_t hash[32];
    if (wally_sha256(data, (size_t)len, hash, sizeof(hash)) != WALLY_OK) return NULL;
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_sign(g_ctx, &sig, hash, w->priv, NULL, NULL)) return NULL;
    secp256k1_ecdsa_signature_normalize(g_ctx, &sig, &sig);
    uint8_t compact[64];
    secp256k1_ecdsa_signature_serialize_compact(g_ctx, compact, &sig);
    return mc_dup_hex(compact, 64);
}

/* Helper used by mc_web_eip55_checksum_inplace's JS callers when they
 * have raw bytes and want hex directly. Not strictly needed for the
 * MVP but cheap to expose. */
char* mc_web_bytes_to_hex(const uint8_t* data, int len) {
    if (!data || len <= 0) return NULL;
    return mc_dup_hex(data, (size_t)len);
}

/* Hex → bytes. Allocates blen bytes on heap, returns pointer; sets
 * *out_blen. Caller frees. Returns NULL on malformed input. */
uint8_t* mc_web_hex_to_bytes(const char* hex, int* out_blen) {
    if (!hex || !out_blen) return NULL;
    size_t hlen = strlen(hex);
    if (hlen >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        hlen -= 2;
    }
    if (hlen & 1) return NULL;
    size_t blen = hlen / 2;
    uint8_t* buf = (uint8_t*)malloc(blen);
    if (!buf) return NULL;
    for (size_t i = 0; i < blen; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { free(buf); return NULL; }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_blen = (int)blen;
    return buf;
}
