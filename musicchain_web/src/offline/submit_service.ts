// Bundles the offline play-log into a signed payload and POSTs it to
// the home node via `offline.play_proof.submit`.
//
// Mirrors musicchain_player/lib/src/services/offline_play_log/
// offline_submit_service.dart, minus the periodic timer + network
// transition kick — the web shell drives the call directly from its
// reconnect handler (NodeClient.onConnect). The wire shape is identical
// so the home node's existing verb handler accepts both clients.
//
// Bundle format is documented in docs/offline_play_proof.md. Signing
// uses the player's wallet key via Wallet.sign (ECDSA over secp256k1,
// same key that signs token transfers). Web has no easy access to
// BSSID / cell-id / battery / screen-on intervals, so those signal
// arrays go out empty and the home node's bot heuristics fall back to
// the remaining signals (heartbeats, monotonic-vs-wall drift, session
// concurrency, wallet age vs play volume).

import type { NodeClient } from '../node_client';
import type { Wallet } from '../wallet';
import {
  type CapturedSession,
  heartbeatCapture,
} from './heartbeat_capture';

/** Result of a submit attempt. Callers (the reconnect handler) can
 *  surface these counts in the UI without re-querying the chain. */
export interface SubmitOutcome {
  attempted:    number; // how many sessions we tried to ship
  shipped:      number; // sessions in the bundle the server accepted
  rejected:     number; // sessions the server explicitly rejected
  flagged:      number; // bot-pattern flags the server returned
}

/** Reply shape from offline.play_proof.submit. The home node returns
 *  partial-success granularity: accepted ids, rejected ids w/ reason,
 *  and any flagged-pattern names that fired against the bundle. */
interface SubmitReply {
  accepted?:         string[];
  rejected?:         Array<{ session_id: string; reason: string }>;
  flagged_patterns?: string[];
}

const DEVICE_ID_KEY = 'mc_device_id_hex';
const SUBMIT_RPC    = 'offline.play_proof.submit';
const SUBMIT_TIMEOUT_MS = 60_000;

/** Stable per-install id, hashed so we never leak a hardware id.
 *  Persisted in localStorage after the first generation. Matches the
 *  Dart `_deviceIdHex` shape (32 random bytes, hex-encoded). */
function deviceIdHex(): string {
  const cached = localStorage.getItem(DEVICE_ID_KEY);
  if (cached && /^[0-9a-f]{64}$/i.test(cached)) return cached.toLowerCase();
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  let hex = '';
  for (const b of bytes) hex += b.toString(16).padStart(2, '0');
  localStorage.setItem(DEVICE_ID_KEY, hex);
  return hex;
}

/** Hex-encode a 32-byte random nonce. */
function nonceHex(): string {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  let hex = '';
  for (const b of bytes) hex += b.toString(16).padStart(2, '0');
  return hex;
}

/** Canonical JSON: sort object keys ascending, no whitespace, preserve
 *  array order, JSON-encode primitives. This is what the home node
 *  re-derives when it verifies the signature; the two sides must
 *  agree byte-for-byte. */
function canonicalJson(v: unknown): string {
  if (v === null || v === undefined) return 'null';
  if (typeof v === 'number') {
    // JSON.stringify handles Infinity/NaN by emitting null, which is
    // wrong for canonical form but never reached here — all numeric
    // fields are integer ms / ints.
    return JSON.stringify(v);
  }
  if (typeof v === 'string' || typeof v === 'boolean') return JSON.stringify(v);
  if (Array.isArray(v)) {
    const parts: string[] = [];
    for (const el of v) parts.push(canonicalJson(el));
    return `[${parts.join(',')}]`;
  }
  if (typeof v === 'object') {
    const obj = v as Record<string, unknown>;
    const keys = Object.keys(obj).sort();
    const parts: string[] = [];
    for (const k of keys) parts.push(`${JSON.stringify(k)}:${canonicalJson(obj[k])}`);
    return `{${parts.join(',')}}`;
  }
  // Fallback (e.g. bigint, function) — caller should never reach here.
  return JSON.stringify(v);
}

/** Convert a CapturedSession into the wire shape expected by the
 *  offline.play_proof.submit verb. Mirrors CapturedSession.toJson()
 *  on the Dart side. */
function sessionToJson(s: CapturedSession): Record<string, unknown> {
  return {
    session_id:           s.sessionId,
    content_hash:         s.contentHash,
    block_hash:           s.blockHash,
    started_wall_ms:      s.startedWallMs,
    started_monotonic_ms: s.startedMonotonicMs,
    ended_wall_ms:        s.endedWallMs,
    ended_monotonic_ms:   s.endedMonotonicMs,
    song_duration_ms:     s.songDurationMs,
    heartbeats: s.heartbeats.map((h) => ({
      wall_ms:      h.wallMs,
      monotonic_ms: h.monotonicMs,
      position_ms:  h.positionMs,
    })),
  };
}

/** Build, sign, and POST the offline play-proof bundle for any
 *  unsubmitted sessions belonging to `playerAddress`.
 *
 *  Behaviour:
 *  - Returns immediately if there are no unsubmitted sessions.
 *  - Builds the bundle, signs the canonical JSON with the wallet's
 *    ECDSA key, snapshots the wall-ms cutoff BEFORE the RPC, sends,
 *    and on a successful reply marks the shipped sessions submitted
 *    using that cutoff (matches the Dart agent's sensor-tail-race fix
 *    — any heartbeats inserted during the RPC stay unsubmitted for
 *    the next bundle).
 *  - On RPC failure the rows stay marked unsubmitted so the next
 *    reconnect re-ships them; the caller's notification of the
 *    failure is the thrown error.
 *
 *  The home node returns a partial-success reply (accepted / rejected
 *  / flagged_patterns). We mark only the accepted ids submitted; the
 *  rejected ids are also marked so we don't keep re-shipping a bundle
 *  the server already classified as bot — but a rejected session
 *  costs the player a play, which is exactly what the home node is
 *  telling us when it rejects, so the trade is correct. */
export async function submitPending(
  node: NodeClient,
  wallet: Wallet,
  playerAddress: string,
): Promise<SubmitOutcome> {
  const cap = heartbeatCapture;

  const sessions = await cap.unsubmittedSessions(playerAddress);
  if (sessions.length === 0) {
    return { attempted: 0, shipped: 0, rejected: 0, flagged: 0 };
  }

  // Bundle base time = earliest captured signal we're shipping. The
  // home node uses this to normalize the per-session timestamps; we
  // only have sessions/heartbeats here so the min is over session
  // start times.
  let wallBase = Number.MAX_SAFE_INTEGER;
  let monoBase = Number.MAX_SAFE_INTEGER;
  for (const s of sessions) {
    if (s.startedWallMs      < wallBase) wallBase = s.startedWallMs;
    if (s.startedMonotonicMs < monoBase) monoBase = s.startedMonotonicMs;
  }

  const body: Record<string, unknown> = {
    bundle_version:      1,
    player_address:      playerAddress,
    pubkey:              wallet.publicKey,
    bundle_nonce:        nonceHex(),
    created_at_ms:       cap.wallMs(),
    device_id:           deviceIdHex(),
    monotonic_base_ms:   monoBase,
    wall_base_ms:        wallBase,
    sessions:            sessions.map(sessionToJson),
    // Web has no API access to these signals — see file header. Empty
    // arrays match the docs/offline_play_proof.md schema and let the
    // home node still credit sessions when the other signals (the
    // heartbeats themselves) carry sufficient weight.
    network_transitions: [],
    battery_samples:     [],
    screen_intervals:    [],
  };

  // Canonicalize, sign, attach. wallet.sign hashes the bytes (sha256)
  // inside the WASM call and returns the 128-hex r||s ECDSA signature.
  const canonical = new TextEncoder().encode(canonicalJson(body));
  body.signature = wallet.sign(canonical);

  // Snapshot the cutoff BEFORE the RPC. The mark-submitted call below
  // uses this to gate heartbeat updates — anything captured during
  // the RPC window keeps submitted=0 and ships in the next bundle.
  // Without this snapshot, a long RPC (up to 60 s) silently marks
  // beats captured during the wait as submitted even though they
  // weren't in this bundle, losing them forever. Same fix as the Dart
  // agent.
  const cutoffWallMs = cap.wallMs();

  const reply = await node.request<SubmitReply>(
    SUBMIT_RPC,
    body,
    SUBMIT_TIMEOUT_MS,
  );

  // The home node returns either explicit accepted / rejected arrays
  // OR an empty body if every session went through. In the latter
  // case treat the full list as accepted.
  const acceptedSet = new Set<string>(reply.accepted ?? []);
  const rejectedSet = new Set<string>(
    (reply.rejected ?? []).map((r) => r.session_id),
  );
  const flagged = reply.flagged_patterns ?? [];

  let acceptedIds: string[];
  if (reply.accepted === undefined && reply.rejected === undefined) {
    // Server didn't itemize — assume everything we shipped landed.
    acceptedIds = sessions.map((s) => s.sessionId);
  } else {
    acceptedIds = sessions
      .map((s) => s.sessionId)
      .filter((id) => acceptedSet.has(id));
  }

  // Mark both accepted and rejected as submitted so we don't re-ship
  // either: rejected ids would just be re-rejected. The Dart agent
  // does the same — submission is best-effort, server is authoritative.
  const toMarkSubmitted = [
    ...acceptedIds,
    ...sessions
      .map((s) => s.sessionId)
      .filter((id) => rejectedSet.has(id)),
  ];
  await cap.markSubmitted(toMarkSubmitted, cutoffWallMs);

  return {
    attempted: sessions.length,
    shipped:   acceptedIds.length,
    rejected:  rejectedSet.size,
    flagged:   flagged.length,
  };
}
