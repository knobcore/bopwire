// On-chain username registration for the web player.
//
// Mirrors WalletFirstLaunchScreen._tryRegisterUsername in the Flutter
// player (musicchain_player/lib/src/screens/wallet_first_launch_screen.dart).
// The preimage layout is locked by UsernameTx::sign_message in
// musicchain/src/core/transaction.cpp; this file MUST byte-for-byte
// reproduce it or the chain's secp256k1 verify will reject the tx:
//
//   u32 LE  MC_CHAIN_ID (19779)              4 bytes
//   u8      name length                      1 byte
//   bytes   name                             N bytes (3..30)
//   bytes   owner address                   20 bytes
//   bytes   owner pubkey (compressed)       33 bytes
//   u64 LE  nonce                            8 bytes
//
// Total preimage length: 66 + name.length.
//
// The wallet's sign() runs sha256 over the preimage inside the WASM
// glue (see mc_web_wallet_sign in wasm/wallet_glue.c), then secp256k1
// signs the digest and returns the 64-byte compact signature as 128
// hex chars. We just ferry that to the home node, which re-derives the
// preimage on its side and verifies.

import type { NodeClient } from './node_client';
import type { Wallet } from './wallet';

/** musicchain mainnet chain id; matches MC_CHAIN_ID in
 *  musicchain/src/core/transaction.cpp. */
const MC_CHAIN_ID = 19779;

/** Username constraints enforced by UsernameTx::deserialize and by the
 *  chain's deeper validators: 3..30 chars, lowercase alphanumeric plus
 *  underscore. The chain also validates this server-side; we re-check
 *  here so the user gets an immediate, local error message. */
const USERNAME_RE = /^[a-z0-9_]{3,30}$/;

export interface RegisterUsernameOpts {
  name: string;
  wallet: Wallet;
  node: NodeClient;
}

export interface RegisterUsernameResult {
  txHash: string;
}

/** Build the sign_message preimage that UsernameTx::sign_message in
 *  src/core/transaction.cpp produces. Exported for unit-test parity
 *  checks against the C++ vector — the bytes MUST match exactly. */
export function buildUsernamePreimage(
  name: string,
  ownerAddress: Uint8Array,
  ownerPubkey: Uint8Array,
  nonce: bigint,
): Uint8Array {
  if (ownerAddress.length !== 20) {
    throw new Error(`owner address must be 20 bytes (got ${ownerAddress.length})`);
  }
  if (ownerPubkey.length !== 33) {
    throw new Error(`owner pubkey must be 33 bytes compressed (got ${ownerPubkey.length})`);
  }
  const nameBytes = new TextEncoder().encode(name);
  if (nameBytes.length > 255) {
    // The u8 length prefix can't represent more — the chain's 30-char
    // cap makes this unreachable in practice, but the guard keeps the
    // function safe for arbitrary callers.
    throw new Error(`name too long for u8 length prefix (${nameBytes.length})`);
  }
  const out = new Uint8Array(4 + 1 + nameBytes.length + 20 + 33 + 8);
  const view = new DataView(out.buffer);
  let off = 0;
  view.setUint32(off, MC_CHAIN_ID, /*littleEndian*/ true);
  off += 4;
  out[off++] = nameBytes.length;
  out.set(nameBytes, off);
  off += nameBytes.length;
  out.set(ownerAddress, off);
  off += 20;
  out.set(ownerPubkey, off);
  off += 33;
  view.setBigUint64(off, nonce, /*littleEndian*/ true);
  off += 8;
  return out;
}

/** Parse a hex string (with or without 0x prefix) into raw bytes.
 *  Throws on odd length or non-hex characters. */
function hexToBytes(hex: string): Uint8Array {
  let s = hex.trim();
  if (s.startsWith('0x') || s.startsWith('0X')) s = s.slice(2);
  if (s.length % 2 !== 0) {
    throw new Error(`odd-length hex string (${s.length})`);
  }
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) {
    const b = Number.parseInt(s.substr(i * 2, 2), 16);
    if (Number.isNaN(b)) {
      throw new Error(`invalid hex at byte ${i}`);
    }
    out[i] = b;
  }
  return out;
}

/** Register `name` on-chain for the address backing `wallet`.
 *
 *  Fetches the wallet nonce, builds the sign_message preimage, signs
 *  it locally via libwally-WASM, and posts the signed envelope to the
 *  home node via `username.register`. Resolves with the chain-assigned
 *  tx hash; rejects on validation failure, RPC error, or signature
 *  rejection by the chain.
 *
 *  This is a "best effort" call from the perspective of the wallet
 *  setup flow — the caller (e.g. WalletSetupScreen) is expected to
 *  swallow exceptions and surface them as a non-blocking warning, the
 *  same way the Flutter player does. */
export async function registerUsername(
  opts: RegisterUsernameOpts,
): Promise<RegisterUsernameResult> {
  const { name, wallet, node } = opts;
  if (!USERNAME_RE.test(name)) {
    throw new Error(
      `username must be 3-30 chars of [a-z0-9_] (got "${name}")`,
    );
  }

  // 1. Fetch the wallet's current nonce. Mirrors the Dart side:
  //    rats.request('wallet.nonce', {address: info.address}).
  const nonceReply = await node.request<{ nonce?: number | string }>(
    'wallet.nonce',
    { address: wallet.address },
  );
  // Server returns it as a number for current chain depths but it's
  // logically a u64 — accept both shapes so we don't overflow at 2^53.
  const rawNonce = nonceReply.nonce ?? 0;
  const nonce =
    typeof rawNonce === 'bigint' ? rawNonce
    : typeof rawNonce === 'string' ? BigInt(rawNonce)
    : BigInt(rawNonce);

  // 2. Build the preimage matching UsernameTx::sign_message exactly.
  const addrBytes = hexToBytes(wallet.address);
  const pkBytes   = hexToBytes(wallet.publicKey);
  const preimage  = buildUsernamePreimage(name, addrBytes, pkBytes, nonce);

  // 3. Sign locally — Wallet.sign hashes + secp256k1-signs, returning
  //    128 hex chars (r||s, 64 bytes compact).
  const signature = wallet.sign(preimage);
  if (signature.length !== 128) {
    throw new Error(
      `wallet.sign returned unexpected length ${signature.length} (want 128)`,
    );
  }

  // 4. Submit. Field names match rats_api.cpp's username.register
  //    handler (name / owner_address / owner_pubkey / nonce / signature).
  //    nonce is serialised as Number for the JSON envelope — same as
  //    Dart's int. If a deployment ever needs >2^53 it'll need a u64
  //    string convention on the wire, but we're nowhere near that.
  const reply = await node.request<{ tx_hash?: string }>(
    'username.register',
    {
      name,
      owner_address: wallet.address,
      owner_pubkey:  wallet.publicKey,
      nonce:         Number(nonce),
      signature,
    },
  );
  const txHash = reply.tx_hash ?? '';
  if (!txHash) {
    throw new Error('username.register reply missing tx_hash');
  }
  return { txHash };
}
