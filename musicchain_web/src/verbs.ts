// Typed wrappers around the home node's chain RPC verbs. Mirrors what
// musicchain_player/lib/src/services/node_client.dart exposes for the
// Flutter player, so the web UI can call the same logical verbs.
//
// Each function takes a NodeClient, builds the body the server expects,
// and casts the reply into a concrete TS shape. Reply field names match
// server.cpp / chain_rpc / wallet handlers (see src/api/rats_api.cpp).

import type { NodeClient } from './node_client';

// -- Reply shapes -----------------------------------------------------

/** Reply from `session.start` — see PlaySession.fromJson in Dart. */
export interface PlaySession {
  /** Hex id the chain assigned to this listening session. */
  session_id: string;
  /** Hash of the block the session was anchored to. */
  block_hash: string;
  /** Echoed back by the wrapper (server doesn't return it). */
  content_hash: string;
}

/** Reply from `session.complete` (mint payout). */
export interface MintResult {
  tx_hash: string;
  /** Decimal-string microMUSIC amount minted to the player. */
  amount: string;
  /** Block height the mint landed in. */
  block_height: number;
}

/** Reply from `wallet.balance` — string to keep full uint256 precision. */
export interface WalletBalance {
  balance: string;
}

/** Reply from `wallet.nonce`. */
export interface WalletNonce {
  nonce: number;
}

/** Reply from `wallet.transfer` / signed-tx submissions. */
export interface TxSubmitResult {
  tx_hash: string;
}

/** One row in `songs.list` / `songs.search` / `songs.get`. Shape mirrors
 *  the chain's Song serialization; fields the server omits arrive as
 *  undefined here and the UI is expected to tolerate that. */
export interface SongRow {
  content_hash: string;
  fingerprint_hash?: string;
  title?: string;
  artist?: string;
  album?: string;
  genre?: string;
  year?: number;
  track_number?: number;
  bitrate?: number;
  duration_ms?: number;
  audio_format?: string;
  uploader_address?: string;
  uploaded_at_ms?: number;
}

/** Inputs for `wallet.transfer`. Wallet-side signing happens outside
 *  this module (libwally-WASM); we just ferry the signed fields. */
export interface SignedTransferTx {
  from_address: string;
  to_address: string;
  /** Decimal microMUSIC amount as a string. */
  amount: string;
  nonce: number;
  signature: string;
}

/** Inputs for `username.register`. Username + signature produced by the
 *  wallet module after the user picks a handle. */
export interface SignedUsernameRegister {
  address: string;
  username: string;
  nonce: number;
  signature: string;
}

// -- Verb wrappers ----------------------------------------------------

/** Start a play session for `contentHash` on behalf of `playerAddress`.
 *  Mirrors NodeClient.startSession in the Dart player. */
export async function startSession(
  client: NodeClient,
  contentHash: string,
  playerAddress: string,
): Promise<PlaySession> {
  const ch = contentHash.trim();
  if (!/^[0-9a-fA-F]{64}$/.test(ch)) {
    throw new Error(`startSession: content_hash must be 64-char hex (got ${ch.length})`);
  }
  const pa = playerAddress.trim();
  const paHex = pa.startsWith('0x') || pa.startsWith('0X') ? pa.slice(2) : pa;
  if (!/^[0-9a-fA-F]{40}$/.test(paHex)) {
    throw new Error('startSession: player_address must be 40-char hex (optionally 0x-prefixed)');
  }
  const reply = await client.request<Partial<PlaySession>>(
    'session.start',
    { content_hash: ch, player_address: pa },
  );
  // Server reply is { session_id, block_hash }; inject content_hash so
  // callers can carry it forward — matches the Dart wrapper exactly.
  return {
    session_id: reply.session_id ?? '',
    block_hash: reply.block_hash ?? '',
    content_hash: reply.content_hash ?? ch,
  };
}

/** Periodic liveness ping inside an active session.
 *  Sent every ~5s by the playing UI. Body is intentionally minimal —
 *  the home node uses its own wall clock and ignores extra fields. */
export async function sendHeartbeat(
  client: NodeClient,
  sessionId: string,
  positionMs: number,
): Promise<void> {
  await client.request(
    'session.heartbeat',
    { session_id: sessionId, position_ms: positionMs },
  );
}

/** Close out a play session and trigger the payout mint. */
export async function completeSession(
  client: NodeClient,
  sessionId: string,
): Promise<MintResult> {
  const reply = await client.request<Partial<MintResult>>(
    'session.complete',
    { session_id: sessionId },
  );
  return {
    tx_hash: reply.tx_hash ?? '',
    amount: reply.amount ?? '0',
    block_height: reply.block_height ?? 0,
  };
}

/** Get the current balance for `address` (decimal microMUSIC string). */
export async function getBalance(
  client: NodeClient,
  address: string,
): Promise<string> {
  const reply = await client.request<WalletBalance>(
    'wallet.balance',
    { address },
  );
  return reply.balance ?? '0';
}

/** List every song the chain currently knows about. The chain may
 *  paginate this in the future; for now the home node returns the
 *  full set. */
export async function listSongs(client: NodeClient): Promise<SongRow[]> {
  const reply = await client.request<SongRow[] | undefined>(
    'songs.list',
    {},
  );
  return Array.isArray(reply) ? reply : [];
}

/** Submit a signed transfer tx (built + signed by the wallet module). */
export async function submitTransfer(
  client: NodeClient,
  signedTx: SignedTransferTx,
): Promise<TxSubmitResult> {
  const reply = await client.request<Partial<TxSubmitResult>>(
    'wallet.transfer',
    {
      from_address: signedTx.from_address,
      to_address: signedTx.to_address,
      amount: signedTx.amount,
      nonce: signedTx.nonce,
      signature: signedTx.signature,
    },
  );
  return { tx_hash: reply.tx_hash ?? '' };
}

/** Register a username for an address. The signature is produced by the
 *  wallet module over (address, username, nonce). */
export async function submitUsernameRegister(
  client: NodeClient,
  signed: SignedUsernameRegister,
): Promise<TxSubmitResult> {
  const reply = await client.request<Partial<TxSubmitResult>>(
    'username.register',
    {
      address: signed.address,
      username: signed.username,
      nonce: signed.nonce,
      signature: signed.signature,
    },
  );
  return { tx_hash: reply.tx_hash ?? '' };
}
