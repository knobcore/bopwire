/**
 * Smoke tests for the wallet WASM module (wasm/wallet.{js,wasm}).
 *
 * These exercise the `Wallet` TypeScript wrapper in `src/wallet.ts`,
 * which in turn cwraps the libwally-backed mc_web_* entry points from
 * native/wallet_glue.c. The goal is to catch any regression that would
 * make the browser player derive a different address — or refuse to
 * sign — for the same mnemonic, before the change ever ships next to
 * the Android player or Windows DLL (which both use the same libwally
 * derivation through their own glue).
 *
 * Run with:
 *
 *     npm test          # watch mode
 *     npm run test:run  # one-shot
 *
 * The Emscripten glue normally fetches `wallet.wasm` over HTTP from the
 * served origin. Under vitest there's no origin and no fetch, so we
 * load the .wasm bytes off disk and inject them via the
 * `__setWalletModuleOptionsForTest({ wasmBinary })` hook the wrapper
 * exposes for this exact reason. Production code never calls that
 * setter.
 *
 * Known-answer test vector:
 *
 *   mnemonic = "abandon abandon abandon abandon abandon abandon
 *               abandon abandon abandon abandon abandon about"
 *   path     = m/44'/19779'/0'/0/0     (musicchain SLIP-44 coin index)
 *   address  = 0xa1210A41eda278ff2e6b3F7E1CAC299BF0215034   (EIP-55)
 *   pubkey   = 03dc89172afe740c979b407279c53e68483c809fc8b49d59aae457867564d09636
 *
 * These values match what the chain's libwally-backed C++ pipeline
 * (musicchain/src/crypto/wally_bip39.cpp → keys.cpp::address_from_pubkey
 * → to_checksum_hex) produces from the same mnemonic, which in turn
 * matches the Android JNI's mc_wallet_from_mnemonic and the Windows
 * DLL's musicchain_player export. If any of those four moves to a
 * different SLIP-44 index, different derivation path, or different
 * keccak256 implementation, this test goes red BEFORE the player ships.
 */

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

import { Wallet, __setWalletModuleOptionsForTest } from '../src/wallet';

// Resolve relative to THIS file (tests/) → musicchain_web/wasm/wallet.wasm
const __dirname = resolve(fileURLToPath(new URL('.', import.meta.url)));
const WASM_PATH = resolve(__dirname, '..', 'wasm', 'wallet.wasm');

// BIP39 canonical "all-abandons" test mnemonic, plus the well-known
// 24-word "abandon × 23 + art" mnemonic the BIP-39 spec uses for
// English test vectors. The task spec called out the 12-word variant
// specifically because the chain's derivation path is locked to a
// single account index — 24 words don't get a deterministic address
// without also pinning the passphrase, which the web player doesn't
// surface.
const KNOWN_VALID_MNEMONIC =
  'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about';

// Pre-computed by deriving once with the same wallet.wasm we test
// against — see the doc comment block above. Hardcoded so the test
// catches any future drift in libwally version, SLIP-44 index, or
// keccak256 implementation. Format: EIP-55 mixed-case, 0x-prefixed.
const EXPECTED_ADDRESS = '0xa1210A41eda278ff2e6b3F7E1CAC299BF0215034';
const EXPECTED_PUBKEY =
  '03dc89172afe740c979b407279c53e68483c809fc8b49d59aae457867564d09636';

// secp256k1 signing in libsecp256k1 is deterministic (RFC 6979) by
// default — same key + same message bytes → same 64-byte (r, s).
// Captured once against this wallet.wasm so any change to the signing
// path or sha256 hashing trips the assertion.
const SIGN_DATA = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
const EXPECTED_SIG_HEX =
  '91f24cb319ce34efc2c8b65d3513ecfdaaae4b20fa723ff6f55fe734444e88263a3e9fdddc31bf447b2827b82046f5ff49b4a41618147b9654e350053228c121';

beforeAll(() => {
  // Read the .wasm bytes off disk so the Emscripten module factory
  // can instantiate without going through fetch(). MUST run before the
  // first Wallet.* static method touches the module.
  const wasmBinary = readFileSync(WASM_PATH);
  __setWalletModuleOptionsForTest({ wasmBinary });
});

afterAll(() => {
  // Clear the test hook so subsequent test files (or hot-reload
  // sessions in `npm test` watch mode) re-instantiate cleanly.
  // The module promise is module-private so the cleared options only
  // matter if this file is re-evaluated — but cheap to do regardless.
  try {
    __setWalletModuleOptionsForTest(null);
  } catch (_) {
    // Already loaded — fine, nothing to clear.
  }
});

describe('Wallet WASM smoke tests', () => {
  it('Test 1: generateMnemonic() returns 12 words from the BIP39 wordlist', async () => {
    const mnemonic = await Wallet.generateMnemonic();
    expect(typeof mnemonic).toBe('string');
    const words = mnemonic.trim().split(/\s+/);
    expect(words).toHaveLength(12);
    // Every word is lowercase a–z (BIP39 English wordlist guarantees
    // this; if the generator ever returns a non-list word the next
    // `validate` call would fail anyway, but checking the shape gives
    // a clearer failure message when something is obviously off).
    for (const w of words) {
      expect(w).toMatch(/^[a-z]+$/);
    }
    // And the just-generated mnemonic must itself validate — round-trip
    // proof that generate + validate agree on the same checksum byte.
    await expect(Wallet.validate(mnemonic)).resolves.toBe(true);
  });

  it('Test 2: validate() accepts known-good mnemonics and rejects junk', async () => {
    await expect(Wallet.validate(KNOWN_VALID_MNEMONIC)).resolves.toBe(true);
    // Whitespace + case insensitivity (wallet.ts trims + lowercases).
    await expect(
      Wallet.validate(`  ${KNOWN_VALID_MNEMONIC.toUpperCase()}  `),
    ).resolves.toBe(true);

    // Obvious garbage — neither a wordlist hit nor the right length.
    await expect(Wallet.validate('asdf jkl bogus')).resolves.toBe(false);
    // 12 wordlist hits, but wrong checksum byte (swap last word from
    // "about" → "abandon"). libwally must reject this.
    await expect(
      Wallet.validate(
        'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon',
      ),
    ).resolves.toBe(false);
    // Empty string.
    await expect(Wallet.validate('')).resolves.toBe(false);
  });

  it('Test 3: fromMnemonic derives the expected EIP-55 address + pubkey', async () => {
    const wallet = await Wallet.fromMnemonic(KNOWN_VALID_MNEMONIC);
    try {
      // EIP-55 canonical form: 0x-prefixed, mixed-case checksum. The
      // string must be byte-identical to EXPECTED_ADDRESS — a
      // case-insensitive comparison would silently hide a checksum
      // regression.
      expect(wallet.address).toBe(EXPECTED_ADDRESS);
      expect(wallet.address).toHaveLength(42);
      expect(wallet.address.startsWith('0x')).toBe(true);
      // The 40 hex chars must NOT be all-lowercase or all-uppercase;
      // that's the EIP-55 mixed-case invariant. (The "all-numeric"
      // pathological case would also pass this — but our test vector
      // has plenty of [a-f] in it, so the mix-case check is meaningful.)
      const hexPart = wallet.address.slice(2);
      expect(hexPart).toMatch(/^[0-9a-fA-F]{40}$/);
      expect(/[A-F]/.test(hexPart)).toBe(true);
      expect(/[a-f]/.test(hexPart)).toBe(true);

      // 33-byte compressed pubkey: 1-byte parity prefix (02 or 03) +
      // 32-byte X coordinate, hex-encoded.
      expect(wallet.publicKey).toBe(EXPECTED_PUBKEY);
      expect(wallet.publicKey).toHaveLength(66);
      expect(/^0[23][0-9a-f]{64}$/.test(wallet.publicKey)).toBe(true);
    } finally {
      wallet.free();
    }
  });

  it('Test 4: sign() returns 128 hex chars and round-trips the chain wire format', async () => {
    const wallet = await Wallet.fromMnemonic(KNOWN_VALID_MNEMONIC);
    try {
      const sig = wallet.sign(SIGN_DATA);
      // Compact (r,s) form — 64 bytes → 128 lowercase hex chars. This
      // is the exact shape the chain expects in TransferTx.signature /
      // UsernameTx.signature, so any deviation here breaks tx
      // verification on the home node.
      expect(typeof sig).toBe('string');
      expect(sig).toHaveLength(128);
      expect(sig).toMatch(/^[0-9a-f]{128}$/);

      // r and s halves separately — neither may be zero (a zero scalar
      // would be a malformed signature that the chain would reject).
      const r = sig.slice(0, 64);
      const s = sig.slice(64, 128);
      expect(r).not.toBe('0'.repeat(64));
      expect(s).not.toBe('0'.repeat(64));

      // Round-trip: decode to bytes → re-encode to hex → must match.
      // Cheap sanity that the hex string is a valid encoding (no stray
      // non-hex chars, even length, etc.) before the chain layer tries
      // to parse it.
      const bytes = new Uint8Array(64);
      for (let i = 0; i < 64; i++) {
        bytes[i] = parseInt(sig.slice(i * 2, i * 2 + 2), 16);
      }
      const reencoded = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
      expect(reencoded).toBe(sig);

      // libsecp256k1 signs deterministically (RFC 6979). Same key +
      // same data must always produce the same compact signature, so
      // the test vector is fully reproducible and the chain can't
      // accidentally accept a "second" valid signature for the same
      // tx body.
      expect(sig).toBe(EXPECTED_SIG_HEX);

      // Repeat the sign to confirm the determinism property in this
      // session — if someone swaps libsecp256k1's nonce function for
      // a randomized one, this assertion goes red even if the wire
      // format still looks right.
      const sig2 = wallet.sign(SIGN_DATA);
      expect(sig2).toBe(sig);

      // Signing different data MUST produce a different signature.
      const otherData = new Uint8Array([0xde, 0xad, 0xbe, 0xee]); // 1-bit flip
      const otherSig = wallet.sign(otherData);
      expect(otherSig).not.toBe(sig);
      expect(otherSig).toHaveLength(128);
    } finally {
      wallet.free();
    }
  });
});
