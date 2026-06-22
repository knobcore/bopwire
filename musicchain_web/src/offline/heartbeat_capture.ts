// Persistent heartbeat log for offline play-proof bundles.
//
// While the player is offline the existing online flow can't reach
// `session.heartbeat` on the home node, so its beats vanish. We mirror
// every beat into an IndexedDB store here so the OfflineSubmitService
// can gather them into a signed bundle at reconnect time.
//
// The on-disk shape mirrors the wire format from
// docs/offline_play_proof.md: one row per heartbeat, plus a sessions
// store for session-level start/end + content_hash. The Dart side
// (musicchain_player/lib/src/services/offline_play_log/heartbeat_capture.dart)
// uses sqflite and the same schema with a few extra tables for network
// transitions and sensor capture; the browser doesn't expose those
// signals (no BSSID / cell-id / battery API access in vanilla web) so
// we only persist sessions + heartbeats and ship the bundle with empty
// sensor arrays. Same wire shape, fewer signals.
//
// Storage choice: IndexedDB — already available without any extra
// dependency, persists across crash/tab close/reload, survives uninstall
// only on explicit user action (browser data clear). Matches what the
// Dart sqflite layer guarantees.

/** One heartbeat captured locally. Wire shape matches the
 *  `heartbeats[]` element from docs/offline_play_proof.md. */
export interface CapturedHeartbeat {
  sessionId:   string;
  contentHash: string;
  positionMs:  number;
  wallMs:      number;
  monotonicMs: number;
}

/** One session bundled at submit time. */
export interface CapturedSession {
  sessionId:          string;
  contentHash:        string;
  blockHash:          string;
  startedWallMs:      number;
  startedMonotonicMs: number;
  endedWallMs:        number;
  endedMonotonicMs:   number;
  songDurationMs:     number;
  playerAddress:      string;
  heartbeats:         CapturedHeartbeat[];
}

// IDB record shapes (private). These carry the `submitted` flag and
// the lowercased `playerAddressLower` we use for case-insensitive
// lookups — see `unsubmittedSessions` for why.
interface SessionRow {
  session_id:             string;
  content_hash:           string;
  block_hash:             string;
  player_address:         string;
  player_address_lower:   string;
  started_wall_ms:        number;
  started_monotonic_ms:   number;
  ended_wall_ms:          number | null;
  ended_monotonic_ms:     number | null;
  song_duration_ms:       number;
  submitted:              0 | 1;
}

interface HeartbeatRow {
  id?:           number; // auto-incremented
  session_id:    string;
  content_hash:  string;
  position_ms:   number;
  wall_ms:       number;
  monotonic_ms:  number;
  submitted:     0 | 1;
}

const DB_NAME    = 'mc_offline_play_log';
const DB_VERSION = 1;
const STORE_SESS = 'sessions';
const STORE_BEAT = 'heartbeats';

/** Promisify an IDBRequest. */
function reqPromise<T>(req: IDBRequest<T>): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror   = () => reject(req.error ?? new Error('IDB request failed'));
  });
}

/** Promisify a single-transaction commit. */
function txPromise(tx: IDBTransaction): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror    = () => reject(tx.error  ?? new Error('IDB tx failed'));
    tx.onabort    = () => reject(tx.error  ?? new Error('IDB tx aborted'));
  });
}

export class HeartbeatCapture {
  private static _instance: HeartbeatCapture | null = null;
  static get instance(): HeartbeatCapture {
    return (this._instance ??= new HeartbeatCapture());
  }

  private _db: IDBDatabase | null = null;
  private _initPromise: Promise<void> | null = null;
  // Monotonic-ms origin shared with the rest of the offline log layer
  // — single source so heartbeat capture and (future) transition
  // capture agree on a clock origin. We use performance.now() which is
  // a monotonic high-resolution clock per the spec.
  private readonly _mono0 = (typeof performance !== 'undefined' && performance.now)
    ? performance.now()
    : Date.now();

  /** Monotonic milliseconds since this HeartbeatCapture was first
   *  instantiated. Used in lieu of a wall clock for the on-disk
   *  `monotonic_ms` column. */
  monotonicMs(): number {
    const now = (typeof performance !== 'undefined' && performance.now)
      ? performance.now()
      : Date.now();
    return Math.floor(now - this._mono0);
  }

  /** Wall clock milliseconds. Wrapped so tests can override. */
  wallMs(): number {
    return Date.now();
  }

  /** Open the IDB database; idempotent. Callers don't need to await
   *  this explicitly — every public method does it lazily — but it's
   *  cheap to drive once at boot to warm the cache. */
  init(): Promise<void> {
    if (this._db) return Promise.resolve();
    if (this._initPromise) return this._initPromise;
    this._initPromise = new Promise<void>((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains(STORE_SESS)) {
          const s = db.createObjectStore(STORE_SESS, { keyPath: 'session_id' });
          // Indexed lookup paths: lowercased player address (so
          // mixed-case EIP-55 stored sessions match a lowercased
          // query — mirrors the Dart `LOWER(s.player_address) =
          // LOWER(?)` predicate) and submitted flag.
          s.createIndex('by_player_lower', 'player_address_lower', { unique: false });
          s.createIndex('by_submitted',    'submitted',            { unique: false });
        }
        if (!db.objectStoreNames.contains(STORE_BEAT)) {
          const h = db.createObjectStore(STORE_BEAT, {
            keyPath: 'id',
            autoIncrement: true,
          });
          h.createIndex('by_session',  'session_id', { unique: false });
          h.createIndex('by_submitted', 'submitted', { unique: false });
        }
      };
      req.onsuccess = () => {
        this._db = req.result;
        // If another tab triggers a `versionchange`, close so it can
        // upgrade without our connection blocking it.
        this._db.onversionchange = () => {
          this._db?.close();
          this._db = null;
        };
        resolve();
      };
      req.onerror = () => reject(req.error ?? new Error('IDB open failed'));
    });
    return this._initPromise;
  }

  private async _ensureDb(): Promise<IDBDatabase> {
    if (!this._db) await this.init();
    if (!this._db) throw new Error('HeartbeatCapture: IDB not initialized');
    return this._db;
  }

  /** Opens a session row at session-start time so subsequent heartbeats
   *  have a stable parent. Idempotent on `sessionId` — a second call
   *  with the same id is a no-op (mirrors sqflite's
   *  ConflictAlgorithm.ignore). */
  async openSession(args: {
    sessionId:     string;
    contentHash:   string;
    playerAddress: string;
    blockHash?:    string;
    durationMs:    number;
  }): Promise<void> {
    const db = await this._ensureDb();
    const tx = db.transaction(STORE_SESS, 'readwrite');
    const store = tx.objectStore(STORE_SESS);
    // Check existence first — IDB has no "insert or ignore" verb, but
    // `add()` errors on duplicate keys. We could swallow the error,
    // but a quick existence check is clearer and avoids the
    // misleading abort logging some browsers emit on a rejected add.
    const existing = await reqPromise(store.get(args.sessionId));
    if (existing) return; // idempotent — no overwrite.
    const wall = this.wallMs();
    const mono = this.monotonicMs();
    const row: SessionRow = {
      session_id:           args.sessionId,
      content_hash:         args.contentHash,
      block_hash:           args.blockHash ?? '0'.repeat(64),
      player_address:       args.playerAddress,
      player_address_lower: args.playerAddress.toLowerCase(),
      started_wall_ms:      wall,
      started_monotonic_ms: mono,
      ended_wall_ms:        null,
      ended_monotonic_ms:   null,
      song_duration_ms:     args.durationMs,
      submitted:            0,
    };
    store.put(row);
    await txPromise(tx);
  }

  /** One row per heartbeat tick. Called by the player's heartbeat loop
   *  on every timer tick regardless of whether the online RPC succeeded
   *  — either way we mirror the beat locally. */
  async recordHeartbeat(args: {
    sessionId:   string;
    contentHash: string;
    positionMs:  number;
  }): Promise<void> {
    const db = await this._ensureDb();
    const tx = db.transaction(STORE_BEAT, 'readwrite');
    const row: HeartbeatRow = {
      session_id:   args.sessionId,
      content_hash: args.contentHash,
      position_ms:  args.positionMs,
      wall_ms:      this.wallMs(),
      monotonic_ms: this.monotonicMs(),
      submitted:    0,
    };
    tx.objectStore(STORE_BEAT).add(row);
    await txPromise(tx);
  }

  /** Marks a session as ended. Caller uses this on natural stop / next-
   *  song transitions. If never called we still pick the session up at
   *  submit time using the last heartbeat as the end timestamp. */
  async closeSession(sessionId: string): Promise<void> {
    const db = await this._ensureDb();
    const tx = db.transaction(STORE_SESS, 'readwrite');
    const store = tx.objectStore(STORE_SESS);
    const row = await reqPromise(store.get(sessionId)) as SessionRow | undefined;
    if (!row) {
      // No-op if the session was never opened. Dart's UPDATE … WHERE
      // ended_wall_ms IS NULL has the same effect.
      return;
    }
    if (row.ended_wall_ms !== null) return; // already closed.
    row.ended_wall_ms      = this.wallMs();
    row.ended_monotonic_ms = this.monotonicMs();
    store.put(row);
    await txPromise(tx);
  }

  /** Returns every session with at least one heartbeat that hasn't
   *  been submitted yet, restricted to `playerAddress`. Sessions whose
   *  `ended_*` columns are still null are closed-in-memory using the
   *  latest heartbeat for the same session.
   *
   *  Player addresses are typically lowercased on chain, but openSession
   *  may have stored a mixed-case EIP-55 checksummed form. The lookup
   *  normalizes both sides so the bundle isn't silently dropped on the
   *  case mismatch — exact mirror of the Dart `LOWER(s.player_address)
   *  = LOWER(?)` predicate. */
  async unsubmittedSessions(playerAddress: string): Promise<CapturedSession[]> {
    const db = await this._ensureDb();
    const lower = playerAddress.toLowerCase();
    // Read sessions for this address (any submitted state), then filter
    // out submitted=1. We can't compound-index on (lower, submitted)
    // cheaply without a synthetic key, and the per-player row count is
    // small enough that filtering in JS is fine.
    const sessTx = db.transaction(STORE_SESS, 'readonly');
    const idx = sessTx.objectStore(STORE_SESS).index('by_player_lower');
    const allForPlayer = await reqPromise(idx.getAll(IDBKeyRange.only(lower))) as SessionRow[];
    await txPromise(sessTx);

    const unsub = allForPlayer
      .filter((r) => r.submitted === 0)
      .sort((a, b) => a.started_wall_ms - b.started_wall_ms);

    const out: CapturedSession[] = [];
    for (const row of unsub) {
      // Pull every heartbeat for this session and filter to unsubmitted.
      // by_session is an unordered index, so we sort in JS by wall_ms.
      const beatTx = db.transaction(STORE_BEAT, 'readonly');
      const beatIdx = beatTx.objectStore(STORE_BEAT).index('by_session');
      const beats = await reqPromise(
        beatIdx.getAll(IDBKeyRange.only(row.session_id)),
      ) as HeartbeatRow[];
      await txPromise(beatTx);

      const live = beats
        .filter((b) => b.submitted === 0)
        .sort((a, b) => a.wall_ms - b.wall_ms);
      if (live.length === 0) continue; // session w/o beats — skip.

      const lastBeat = live[live.length - 1]!;
      const endedWall = row.ended_wall_ms      ?? lastBeat.wall_ms;
      const endedMono = row.ended_monotonic_ms ?? lastBeat.monotonic_ms;
      out.push({
        sessionId:          row.session_id,
        contentHash:        row.content_hash,
        blockHash:          row.block_hash,
        playerAddress:      row.player_address,
        startedWallMs:      row.started_wall_ms,
        startedMonotonicMs: row.started_monotonic_ms,
        endedWallMs:        endedWall,
        endedMonotonicMs:   endedMono,
        songDurationMs:     row.song_duration_ms,
        heartbeats: live.map((b) => ({
          sessionId:   row.session_id,
          contentHash: b.content_hash,
          positionMs:  b.position_ms,
          wallMs:      b.wall_ms,
          monotonicMs: b.monotonic_ms,
        })),
      });
    }
    return out;
  }

  /** Mark every row that participated in the bundle as submitted so
   *  the next reconnect doesn't double-submit. Caller passes the same
   *  session ids it just shipped, plus the wall-ms cutoff that was
   *  captured *before* the RPC went out — anything inserted after that
   *  instant is for the NEXT bundle and must stay unsubmitted.
   *
   *  The cutoff matters because heartbeats keep streaming during the
   *  RPC; a naive "mark everything submitted=0 → submitted=1" on the
   *  named sessions would silently include beats captured mid-RPC,
   *  losing them from the next bundle. We mirror the Dart fix here. */
  async markSubmitted(sessionIds: readonly string[], cutoffWallMs: number): Promise<void> {
    if (sessionIds.length === 0) return;
    const ids = new Set(sessionIds);
    const db = await this._ensureDb();
    // Single rw tx covering both stores so a mid-flight crash doesn't
    // leave the sessions store marked while the heartbeats store says
    // pending (or vice versa).
    const tx = db.transaction([STORE_SESS, STORE_BEAT], 'readwrite');
    const sessStore = tx.objectStore(STORE_SESS);
    const beatStore = tx.objectStore(STORE_BEAT);

    // Flip session rows. We update only rows whose session_id is in
    // the caller-supplied set and which are currently unsubmitted.
    for (const sid of ids) {
      const row = await reqPromise(sessStore.get(sid)) as SessionRow | undefined;
      if (!row || row.submitted === 1) continue;
      row.submitted = 1;
      sessStore.put(row);
    }

    // Flip heartbeats. We iterate by_session for each id to avoid a
    // full-store scan; combined with the cutoff predicate this only
    // touches rows that were actually shipped in the bundle.
    for (const sid of ids) {
      const beatIdx = beatStore.index('by_session');
      const beats = await reqPromise(
        beatIdx.getAll(IDBKeyRange.only(sid)),
      ) as HeartbeatRow[];
      for (const b of beats) {
        if (b.submitted === 1) continue;
        if (b.wall_ms > cutoffWallMs) continue; // captured mid-RPC.
        b.submitted = 1;
        beatStore.put(b);
      }
    }

    await txPromise(tx);
  }
}

export const heartbeatCapture = HeartbeatCapture.instance;
