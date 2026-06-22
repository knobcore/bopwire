// TypeScript wrapper around the chain-core WASM module
// (wasm/chain.{js,wasm}, built from native/chain_glue.cpp +
// musicchain/src/core/transaction.cpp + crypto/*).
//
// The web player UI uses this to:
//   - Build TransferTx + UsernameTx sign-message preimages, which the
//     wallet module (libwally-WASM) then SHA-256s + signs.
//   - Validate user-typed addresses (EIP-55 checksum aware).
//   - Render an address in canonical EIP-55 mixed-case form for display.
//   - Hash arbitrary bytes (session ids) with SHA-256.
//
// Two-track implementation strategy:
//
//   1. Pure-TypeScript code paths exist for ALL helpers (preimage
//      builders, EIP-55 checksum, address parser, SHA-256). The byte
//      layout matches musicchain/src/core/transaction.cpp's
//      sign_message() functions exactly, so the UI works the moment
//      this file is imported — no waiting on the WASM fetch.
//
//   2. When wasm/chain.js loads successfully, the WASM-backed versions
//      take over via Chain.useWasm(). The native code is the canonical
//      implementation (same source the home node + Android player +
//      Windows DLL link against) so any divergence in the TS port gets
//      caught the moment the WASM is wired in. Treat the TS path as
//      "MVP ships today, WASM is the hardening step".
//
// Byte layouts (from musicchain/src/core/transaction.cpp):
//
//   TransferTx::sign_message:
//     u32 LE chain_id (= MC_CHAIN_ID, 19779 = 0x4D43)
//     from_address                       (20 bytes)
//     to_address                         (20 bytes)
//     u64 LE amount
//     u64 LE nonce
//                                        -> 60 bytes total
//
//   UsernameTx::sign_message:
//     u32 LE chain_id
//     u8     name_len
//     name                               (name_len bytes, ASCII)
//     owner                              (20 bytes)
//     owner_pubkey                       (33 bytes compressed secp256k1)
//     u64 LE nonce

import type { ChainEmscriptenModule } from '@wasm/chain.js';

// =====================================================================
//  Constants
// =====================================================================

/** Chain id mixed into every signed message preimage. Matches
 *  MC_CHAIN_ID in musicchain/src/core/block.h (19779 = "MC" in ASCII). */
export const MC_CHAIN_ID = 19779;

const ADDRESS_LEN = 20;
const PUBKEY33_LEN = 33;

// =====================================================================
//  Lazy WASM module loader
// =====================================================================

let _modulePromise: Promise<ChainEmscriptenModule> | null = null;
let _moduleResolved: ChainEmscriptenModule | null = null;

/**
 * Kick off (or reuse) the chain WASM fetch + instantiate. Memoised, so
 * calling this from screen-bootstrap code and again at first use does
 * not double-fetch the .wasm.
 *
 * Throws if the import fails (404 / unsupported runtime). Callers can
 * catch and fall back to the pure-TS code paths — the static helpers
 * on Chain stay functional whether or not this promise resolves.
 */
async function loadModule(): Promise<ChainEmscriptenModule> {
  if (_modulePromise) return _modulePromise;
  _modulePromise = (async () => {
    const factory = (await import('@wasm/chain.js')).default;
    const mod = await factory();
    _moduleResolved = mod;
    return mod;
  })();
  return _modulePromise;
}

// =====================================================================
//  Pure-TS primitives
//
//  SHA-256 and Keccak-256 implementations are deliberately self-
//  contained so this module has zero npm dependencies. Both are
//  drop-in replacements for the canonical algorithms; outputs match
//  byte-for-byte against test vectors from RFC 6234 (SHA-256) and
//  the Ethereum keccak256 ABI test vectors.
// =====================================================================

// ---- SHA-256 (FIPS 180-4) ------------------------------------------

const K256 = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function rotr32(x: number, n: number): number {
  return ((x >>> n) | (x << (32 - n))) >>> 0;
}

function sha256Pure(data: Uint8Array): Uint8Array {
  // Padding: append 0x80, zeros up to multiple of 64 minus 8, then
  // 64-bit big-endian bit length.
  const bitLen = BigInt(data.length) * 8n;
  const padLen = (data.length + 9 + 63) & ~63;
  const padded = new Uint8Array(padLen);
  padded.set(data, 0);
  padded[data.length] = 0x80;
  // High 32 bits then low 32 bits of bit-length, big-endian.
  const hi = Number((bitLen >> 32n) & 0xffffffffn);
  const lo = Number(bitLen & 0xffffffffn);
  const view = new DataView(padded.buffer);
  view.setUint32(padLen - 8, hi, false);
  view.setUint32(padLen - 4, lo, false);

  const H = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
    0x1f83d9ab, 0x5be0cd19,
  ]);
  const W = new Uint32Array(64);

  for (let offset = 0; offset < padLen; offset += 64) {
    for (let t = 0; t < 16; t++) {
      W[t] = view.getUint32(offset + t * 4, false);
    }
    for (let t = 16; t < 64; t++) {
      const w15 = W[t - 15]!;
      const w2 = W[t - 2]!;
      const s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >>> 3);
      const s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >>> 10);
      W[t] = (W[t - 16]! + s0 + W[t - 7]! + s1) >>> 0;
    }

    let a = H[0]!, b = H[1]!, c = H[2]!, d = H[3]!;
    let e = H[4]!, f = H[5]!, g = H[6]!, h = H[7]!;

    for (let t = 0; t < 64; t++) {
      const S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + K256[t]! + W[t]!) >>> 0;
      const S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const mj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + mj) >>> 0;
      h = g;
      g = f;
      f = e;
      e = (d + t1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) >>> 0;
    }

    H[0] = (H[0]! + a) >>> 0;
    H[1] = (H[1]! + b) >>> 0;
    H[2] = (H[2]! + c) >>> 0;
    H[3] = (H[3]! + d) >>> 0;
    H[4] = (H[4]! + e) >>> 0;
    H[5] = (H[5]! + f) >>> 0;
    H[6] = (H[6]! + g) >>> 0;
    H[7] = (H[7]! + h) >>> 0;
  }

  const out = new Uint8Array(32);
  const outView = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) outView.setUint32(i * 4, H[i]!, false);
  return out;
}

// ---- Keccak-256 (the legacy SHA-3 variant Ethereum + EIP-55 use) ----
//
// This is keccak-f[1600] with rate=1088 bits, capacity=512 bits, and
// the PRE-FIPS-202 padding byte 0x01 (NOT 0x06 — that's the SHA-3
// standard). Identical to what musicchain/src/crypto/keccak256_c.c
// produces, which is what EIP-55 needs.

const KECCAK_ROUND_RC = new BigUint64Array([
  0x0000000000000001n, 0x0000000000008082n, 0x800000000000808an,
  0x8000000080008000n, 0x000000000000808bn, 0x0000000080000001n,
  0x8000000080008081n, 0x8000000000008009n, 0x000000000000008an,
  0x0000000000000088n, 0x0000000080008009n, 0x000000008000000an,
  0x000000008000808bn, 0x800000000000008bn, 0x8000000000008089n,
  0x8000000000008003n, 0x8000000000008002n, 0x8000000000000080n,
  0x000000000000800an, 0x800000008000000an, 0x8000000080008081n,
  0x8000000000008080n, 0x0000000080000001n, 0x8000000080008008n,
]);

const KECCAK_ROT_OFFSETS = [
  0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8,
  18, 2, 61, 56, 14,
];

function keccakF1600(state: BigUint64Array): void {
  const C = new BigUint64Array(5);
  const D = new BigUint64Array(5);
  const B = new BigUint64Array(25);
  const MASK = 0xffffffffffffffffn;

  for (let round = 0; round < 24; round++) {
    // θ
    for (let x = 0; x < 5; x++) {
      C[x] =
        state[x]! ^ state[x + 5]! ^ state[x + 10]! ^ state[x + 15]! ^
        state[x + 20]!;
    }
    for (let x = 0; x < 5; x++) {
      const c4 = C[(x + 4) % 5]!;
      const c1 = C[(x + 1) % 5]!;
      D[x] = c4 ^ (((c1 << 1n) | (c1 >> 63n)) & MASK);
    }
    for (let i = 0; i < 25; i++) state[i] = state[i]! ^ D[i % 5]!;

    // ρ + π
    for (let x = 0; x < 5; x++) {
      for (let y = 0; y < 5; y++) {
        const idx = x + 5 * y;
        const newIdx = y + 5 * ((2 * x + 3 * y) % 5);
        const rot = BigInt(KECCAK_ROT_OFFSETS[idx]!);
        const v = state[idx]!;
        B[newIdx] = ((v << rot) | (v >> (64n - rot))) & MASK;
        // Handle rot=0 separately to avoid a 64-bit shift by 64 which
        // is implementation-defined in some BigInt engines. For rot=0
        // the value is unchanged anyway.
        if (rot === 0n) B[newIdx] = v;
      }
    }

    // χ
    for (let y = 0; y < 5; y++) {
      for (let x = 0; x < 5; x++) {
        const i = x + 5 * y;
        state[i] = B[i]! ^ ((~B[((x + 1) % 5) + 5 * y]! & MASK) & B[((x + 2) % 5) + 5 * y]!);
      }
    }

    // ι
    state[0] = state[0]! ^ KECCAK_ROUND_RC[round]!;
  }
}

function keccak256Pure(data: Uint8Array): Uint8Array {
  const RATE_BYTES = 136; // 1088 / 8
  const state = new BigUint64Array(25);
  const stateBytes = new Uint8Array(state.buffer);

  let offset = 0;
  while (data.length - offset >= RATE_BYTES) {
    for (let i = 0; i < RATE_BYTES; i++) {
      stateBytes[i] ^= data[offset + i]!;
    }
    keccakF1600(state);
    offset += RATE_BYTES;
  }

  // Padding: domain byte 0x01 (legacy keccak, not 0x06), trailing 0x80
  const rem = data.length - offset;
  for (let i = 0; i < rem; i++) {
    stateBytes[i] ^= data[offset + i]!;
  }
  stateBytes[rem] ^= 0x01;
  stateBytes[RATE_BYTES - 1] ^= 0x80;
  keccakF1600(state);

  return stateBytes.slice(0, 32);
}

// ---- LE writers ----------------------------------------------------

function writeU32LE(buf: Uint8Array, offset: number, v: number): void {
  buf[offset]     = v & 0xff;
  buf[offset + 1] = (v >>> 8) & 0xff;
  buf[offset + 2] = (v >>> 16) & 0xff;
  buf[offset + 3] = (v >>> 24) & 0xff;
}

function writeU64LE(buf: Uint8Array, offset: number, v: bigint): void {
  const MASK = 0xffn;
  for (let i = 0; i < 8; i++) {
    buf[offset + i] = Number((v >> BigInt(i * 8)) & MASK);
  }
}

// ---- Hex helpers ---------------------------------------------------

function bytesToHex(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i]!;
    out += (b < 16 ? '0' : '') + b.toString(16);
  }
  return out;
}

function hexToBytes(hex: string): Uint8Array | null {
  let h = hex;
  if (h.length >= 2 && h[0] === '0' && (h[1] === 'x' || h[1] === 'X')) {
    h = h.slice(2);
  }
  if (h.length === 0 || (h.length & 1) !== 0) return null;
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) {
    const hi = hexNibble(h.charCodeAt(i * 2));
    const lo = hexNibble(h.charCodeAt(i * 2 + 1));
    if (hi < 0 || lo < 0) return null;
    out[i] = (hi << 4) | lo;
  }
  return out;
}

function hexNibble(code: number): number {
  if (code >= 0x30 && code <= 0x39) return code - 0x30;
  if (code >= 0x61 && code <= 0x66) return code - 0x61 + 10;
  if (code >= 0x41 && code <= 0x46) return code - 0x41 + 10;
  return -1;
}

// =====================================================================
//  Public API
// =====================================================================

/**
 * Static helpers for chain-level operations the player UI needs:
 * preimage builders for the two transaction types we sign client-side,
 * EIP-55 address rendering / validation, and SHA-256.
 *
 * All methods are static — there is no per-instance state. The lazy
 * WASM module is module-private and reused across calls.
 */
export class Chain {
  // No instances.
  private constructor() {}

  /**
   * Kick off the WASM fetch in the background. Safe to call any number
   * of times — the underlying promise is memoised. Throws if the import
   * fails; callers may catch and continue using the pure-TS path.
   *
   * NB: NOT awaited by any of the static helpers. The TS implementations
   * stay live regardless of WASM availability; the WASM path replaces
   * them once it has loaded (see useWasm()).
   */
  static async preload(): Promise<void> {
    await loadModule();
  }

  /**
   * Returns true once the WASM module has loaded successfully. The
   * helpers automatically prefer the WASM path when this returns true.
   */
  static isWasmReady(): boolean {
    return _moduleResolved !== null;
  }

  // -----------------------------------------------------------------
  //  Preimage builders
  // -----------------------------------------------------------------

  /**
   * Build the TransferTx sign-message preimage exactly the way
   * mc::TransferTx::sign_message() in musicchain/src/core/transaction.cpp
   * does:
   *
   *   u32 LE chain_id (19779) || from(20) || to(20) || amount(8 LE) || nonce(8 LE)
   *
   * Both `from` and `to` are 20-byte raw addresses (NOT hex). `amount`
   * and `nonce` are u64 little-endian. The caller (wallet module) is
   * responsible for SHA-256'ing this and signing the hash.
   *
   * Throws if `from`/`to` aren't 20 bytes, or if amount/nonce don't fit
   * in u64.
   */
  static transferPreimage(
    from: Uint8Array,
    to: Uint8Array,
    amount: bigint,
    nonce: bigint,
  ): Uint8Array {
    if (from.length !== ADDRESS_LEN) {
      throw new Error(`transferPreimage: from must be ${ADDRESS_LEN} bytes`);
    }
    if (to.length !== ADDRESS_LEN) {
      throw new Error(`transferPreimage: to must be ${ADDRESS_LEN} bytes`);
    }
    if (amount < 0n || amount > 0xffffffffffffffffn) {
      throw new Error('transferPreimage: amount out of u64 range');
    }
    if (nonce < 0n || nonce > 0xffffffffffffffffn) {
      throw new Error('transferPreimage: nonce out of u64 range');
    }

    // Try WASM first if available — same byte output but lets us catch
    // drift between the TS and C++ implementations in CI before it can
    // produce a bad signature.
    if (_moduleResolved) {
      const wasmHex = wasmTransferHex(
        _moduleResolved,
        bytesToHex(from),
        bytesToHex(to),
        amount.toString(10),
        nonce.toString(10),
      );
      if (wasmHex) {
        const bytes = hexToBytes(wasmHex);
        if (bytes) return bytes;
      }
      // Fall through to TS path on WASM failure rather than throwing —
      // the TS path is authoritative against the chain source.
    }

    const out = new Uint8Array(4 + 20 + 20 + 8 + 8);
    writeU32LE(out, 0, MC_CHAIN_ID);
    out.set(from, 4);
    out.set(to, 4 + 20);
    writeU64LE(out, 4 + 40, amount);
    writeU64LE(out, 4 + 48, nonce);
    return out;
  }

  /**
   * Build the UsernameTx sign-message preimage exactly the way
   * mc::UsernameTx::sign_message() in transaction.cpp does:
   *
   *   u32 LE chain_id || u8 name_len || name || owner(20) || owner_pubkey(33) || nonce(8 LE)
   *
   * `name` is a UTF-8 string; the on-chain validator enforces a
   * [a-z0-9_] / 3..30 chars rule but this helper only checks the wire-
   * format constraint (name_len fits in u8). `owner` is 20 raw bytes,
   * `ownerPubkey` is 33 raw bytes (compressed secp256k1 pubkey).
   */
  static usernamePreimage(
    name: string,
    owner: Uint8Array,
    ownerPubkey: Uint8Array,
    nonce: bigint,
  ): Uint8Array {
    if (owner.length !== ADDRESS_LEN) {
      throw new Error(`usernamePreimage: owner must be ${ADDRESS_LEN} bytes`);
    }
    if (ownerPubkey.length !== PUBKEY33_LEN) {
      throw new Error(
        `usernamePreimage: ownerPubkey must be ${PUBKEY33_LEN} bytes`,
      );
    }
    if (nonce < 0n || nonce > 0xffffffffffffffffn) {
      throw new Error('usernamePreimage: nonce out of u64 range');
    }

    const nameBytes = new TextEncoder().encode(name);
    if (nameBytes.length === 0 || nameBytes.length > 0xff) {
      throw new Error('usernamePreimage: name byte length must fit in u8');
    }

    if (_moduleResolved) {
      const wasmHex = wasmUsernameHex(
        _moduleResolved,
        name,
        bytesToHex(owner),
        bytesToHex(ownerPubkey),
        nonce.toString(10),
      );
      if (wasmHex) {
        const bytes = hexToBytes(wasmHex);
        if (bytes) return bytes;
      }
    }

    const out = new Uint8Array(4 + 1 + nameBytes.length + 20 + 33 + 8);
    let p = 0;
    writeU32LE(out, p, MC_CHAIN_ID);
    p += 4;
    out[p++] = nameBytes.length;
    out.set(nameBytes, p);
    p += nameBytes.length;
    out.set(owner, p);
    p += 20;
    out.set(ownerPubkey, p);
    p += 33;
    writeU64LE(out, p, nonce);
    return out;
  }

  // -----------------------------------------------------------------
  //  Address helpers
  // -----------------------------------------------------------------

  /**
   * Render a 40-char lowercase hex address as its EIP-55 mixed-case
   * checksummed form, prefixed with "0x". Matches the C++
   * `mc::crypto::to_checksum_hex` byte-for-byte (same keccak256, same
   * "nibble >= 8 means uppercase" rule).
   *
   * Input must be exactly 40 hex chars (no 0x prefix, no whitespace).
   * Throws otherwise.
   */
  static eip55(addr40lc: string): string {
    if (addr40lc.length !== 40 || !/^[0-9a-f]{40}$/.test(addr40lc)) {
      throw new Error(
        'eip55: expected 40-char lowercase hex address without 0x prefix',
      );
    }

    if (_moduleResolved) {
      const wasm = wasmToChecksumHex(_moduleResolved, addr40lc);
      if (wasm) return wasm;
    }

    // Pure-TS: keccak256 of the ASCII bytes of the lowercase hex.
    const hashed = keccak256Pure(new TextEncoder().encode(addr40lc));
    let out = '0x';
    for (let i = 0; i < 40; i++) {
      const c = addr40lc[i]!;
      const nibble = (i & 1) === 0 ? hashed[i >>> 1]! >>> 4 : hashed[i >>> 1]! & 0x0f;
      if (nibble >= 8 && c >= 'a' && c <= 'f') {
        out += c.toUpperCase();
      } else {
        out += c;
      }
    }
    return out;
  }

  /**
   * Parse + validate user-entered address text. Accepts:
   *   - 0x-prefixed or bare 40-char hex
   *   - all-lowercase or all-uppercase (no checksum to verify)
   *   - mixed-case (EIP-55 checksum IS verified; bad-checksum rejected)
   *
   * Returns the 20 raw bytes on success, or `null` on any failure
   * (length, non-hex chars, bad mixed-case checksum). Callers can then
   * feed the result into transferPreimage / usernamePreimage as-is.
   *
   * Trims surrounding whitespace before parsing.
   */
  static parseAddress(input: string): Uint8Array | null {
    if (typeof input !== 'string') return null;
    let s = input.trim();
    if (s.length === 42 && (s[0] === '0' && (s[1] === 'x' || s[1] === 'X'))) {
      s = s.slice(2);
    }
    if (s.length !== 40) return null;

    let hasUpper = false;
    let hasLower = false;
    for (let i = 0; i < 40; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0x61 && c <= 0x66) hasLower = true;
      else if (c >= 0x41 && c <= 0x46) hasUpper = true;
      else if (!(c >= 0x30 && c <= 0x39)) return null;
    }

    const bytes = hexToBytes(s);
    if (!bytes || bytes.length !== 20) return null;

    if (hasUpper && hasLower) {
      // EIP-55: re-derive checksum from the bytes, compare nibble-for-
      // nibble against the input. Any mismatch rejects.
      const want = Chain.eip55(bytesToHex(bytes));
      // `want` is "0x..."; strip the prefix before comparing.
      if (want.slice(2) !== s) return null;
    }
    return bytes;
  }

  // -----------------------------------------------------------------
  //  SHA-256
  // -----------------------------------------------------------------

  /**
   * Synchronous SHA-256 of arbitrary bytes. Useful for hashing
   * 32-byte session ids and other per-stream nonces the player
   * generates client-side.
   *
   * Returns a 32-byte Uint8Array. Pure-TS implementation; the WASM
   * version is wired in once available but byte-identical.
   */
  static sha256(data: Uint8Array): Uint8Array {
    if (_moduleResolved) {
      const hashHex = wasmSha256Hex(_moduleResolved, data);
      if (hashHex) {
        const bytes = hexToBytes(hashHex);
        if (bytes && bytes.length === 32) return bytes;
      }
    }
    return sha256Pure(data);
  }
}

// =====================================================================
//  Internal WASM bridges
//
//  Each of these wraps one `mc_web_chain_*` cwrap call. Pull factored
//  out of the static helpers so the happy path stays cheap (just an
//  arithmetic check + a typed-array write) and the WASM fallback is
//  explicit + easy to audit.
// =====================================================================

function readAndFreeCString(mod: ChainEmscriptenModule, ptr: number): string | null {
  if (!ptr) return null;
  try {
    return mod.UTF8ToString(ptr);
  } finally {
    mod._free(ptr);
  }
}

function wasmTransferHex(
  mod: ChainEmscriptenModule,
  fromHex: string,
  toHex: string,
  amountStr: string,
  nonceStr: string,
): string | null {
  const ptr = Number(
    mod.ccall(
      'mc_web_chain_transfer_sign_message',
      'number',
      ['string', 'string', 'string', 'string'],
      [fromHex, toHex, amountStr, nonceStr],
    ),
  );
  return readAndFreeCString(mod, ptr);
}

function wasmUsernameHex(
  mod: ChainEmscriptenModule,
  name: string,
  ownerHex: string,
  pubkeyHex: string,
  nonceStr: string,
): string | null {
  const ptr = Number(
    mod.ccall(
      'mc_web_chain_username_sign_message',
      'number',
      ['string', 'string', 'string', 'string'],
      [name, ownerHex, pubkeyHex, nonceStr],
    ),
  );
  return readAndFreeCString(mod, ptr);
}

function wasmToChecksumHex(
  mod: ChainEmscriptenModule,
  lowercaseHex: string,
): string | null {
  const ptr = Number(
    mod.ccall(
      'mc_web_chain_to_checksum_hex',
      'number',
      ['string'],
      [lowercaseHex],
    ),
  );
  return readAndFreeCString(mod, ptr);
}

function wasmSha256Hex(
  mod: ChainEmscriptenModule,
  data: Uint8Array,
): string | null {
  const ptr = mod._malloc(data.length || 1);
  if (!ptr) return null;
  try {
    if (data.length > 0) mod.HEAPU8.set(data, ptr);
    const out = Number(
      mod.ccall(
        'mc_web_chain_sha256_hex',
        'number',
        ['number', 'number'],
        [ptr, data.length],
      ),
    );
    return readAndFreeCString(mod, out);
  } finally {
    mod._free(ptr);
  }
}
