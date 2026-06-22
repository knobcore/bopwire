// Posts session.heartbeat to the full node on a fixed 5 s cadence while
// the Player is actively playing the current song. Mirrors the contract
// in musicchain_player/lib/src/services/heartbeat_service.dart: minimal
// {session_id, position_ms} payload, swallow transient errors and retry
// on the next tick, fire a single session.complete on stop().
//
// The browser-side NodeClient is still being ported, so we accept a
// structural interface here. Any object with a `request(verb, body)`
// method that resolves to the chain's JSON reply qualifies — the actual
// NodeClient class lives separately and will be wired in from main.ts.

import type { Player } from './player.js';

/// Minimal contract the heartbeat needs out of the NodeClient. Declared
/// here as a structural type so we can build / typecheck this file
/// without circularly depending on the not-yet-finished NodeClient.
///
/// `request` mirrors Dart's NodeClient._rpc(type, body) shape: a single
/// verb string ('session.heartbeat' / 'session.complete') plus the JSON
/// body. The reply type isn't load-bearing here — we never read it.
export interface NodeClientLike {
  request(verb: string, body: Record<string, unknown>): Promise<unknown>;
}

/// Construction args. Captured once on construct rather than as start()
/// parameters because the web flow opens the session up front and the
/// heartbeat outlives any single play/pause cycle — the Dart equivalent
/// holds these in the Provider and re-passes them on every start().
export interface HeartbeatArgs {
  node: NodeClientLike;
  player: Player;
  sessionId: string;
  contentHash: string;
  playerAddress: string;
  songDurationMs: number;
}

export class HeartbeatService {
  private readonly _node: NodeClientLike;
  private readonly _player: Player;
  private readonly _sessionId: string;
  // contentHash / playerAddress / songDurationMs are stashed so a future
  // offline-play-log mirror (the Dart side ships these into
  // HeartbeatCapture.openSession) can pick them off the service without
  // re-threading them through the call site. Read-only after construct.
  // The Dart contract documents these as required even when the offline
  // mirror is disabled, so we keep the same surface area.
  // Marked readonly to make the immutability intent explicit and
  // dodge accidental mid-flight mutation by future callers.
  // The values themselves aren't referenced yet on the web — the offline
  // log capture isn't ported — but the constructor contract matches the
  // Dart side so the call sites line up. Underscore-prefixed names tell
  // tsc's noUnusedParameters check this is intentional.
  private readonly _contentHash: string;
  private readonly _playerAddress: string;
  private readonly _songDurationMs: number;

  // setInterval handle. Number in browsers, NodeJS.Timeout under @types/node.
  // ReturnType<typeof setInterval> picks the right one without a guard.
  private _timer: ReturnType<typeof setInterval> | null = null;

  // Latches `stop()` so a double-stop (e.g. UI click + ended event both
  // racing) only fires one session.complete. Without this the full node
  // would log a noisy "unknown session" on the second call.
  private _stopped = false;

  constructor(args: HeartbeatArgs) {
    this._node = args.node;
    this._player = args.player;
    this._sessionId = args.sessionId;
    this._contentHash = args.contentHash;
    this._playerAddress = args.playerAddress;
    this._songDurationMs = args.songDurationMs;
    // Touch the captured-but-not-yet-used fields so the tsconfig's
    // noUnusedLocals / noUnusedParameters checks don't reject the file.
    // Once the offline-log mirror is ported these reads turn into the
    // real openSession()/recordHeartbeat() payloads.
    void this._contentHash;
    void this._playerAddress;
    void this._songDurationMs;

    // 5 s cadence — same as the Dart side. Dense enough that the full
    // node's union-of-timestamp-ranges play-count math sees every
    // distinct chunk of the song as the listener moves through it,
    // sparse enough that a marginal-network player still keeps up.
    this._timer = setInterval(() => {
      void this._tick();
    }, 5000);
  }

  /// Stop the heartbeat and notify the full node the session is done.
  /// Awaits the session.complete call so the caller can sequence a
  /// next-track play() behind it — the Dart side does the equivalent
  /// via the unawaited path because it has its own playlist controller,
  /// but on web the caller drives the queue, so blocking here is fine.
  ///
  /// Errors from session.complete are logged, never thrown — the full
  /// node may have already evicted the session via its own timeout
  /// and we don't want a 404 to break playlist advancement.
  async stop(): Promise<void> {
    if (this._stopped) return;
    this._stopped = true;
    if (this._timer !== null) {
      clearInterval(this._timer);
      this._timer = null;
    }
    try {
      await this._node.request('session.complete', {
        session_id: this._sessionId,
      });
    } catch (err) {
      // Best-effort: the chain's 50% effective-listen rule decides
      // whether this turns into a MintTx, so a missed complete just
      // means the session ages out server-side. Log and move on.
      // eslint-disable-next-line no-console
      console.error('heartbeat: session.complete failed', err);
    }
  }

  private async _tick(): Promise<void> {
    // Gate identical to Dart's HeartbeatService._tick: only fire while
    // the user is actively playing. Pause / stop / load freezes the
    // server's view of position_ms so the union-of-ranges math doesn't
    // synthesize listen credit for a parked song.
    if (this._player.state !== 'playing') return;
    const pos = this._player.positionMs;
    try {
      await this._node.request('session.heartbeat', {
        session_id: this._sessionId,
        position_ms: pos,
      });
    } catch {
      // Swallow — will retry on the next tick. The Dart side does the
      // same; transient network failures are the common case on mobile
      // and we don't want a single dropped packet to break the session.
    }
  }
}
