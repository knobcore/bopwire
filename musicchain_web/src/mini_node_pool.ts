// Single-active mini-node pool for the browser player.
//
// Earlier history: this file used to dial every known mini-node in
// parallel via the librats-WASM transport. That code was ripped out
// when librats-WASM left the browser bundle (see `main.ts` for the
// background). The browser is now WebSocket-only and talks to ONE
// mini-node at a time via the single `NodeClient` socket.
//
// Why we still need a pool: the player should never wedge if one VPS
// dies. The pool keeps a directory of known mini-node addresses,
// picks the one that's currently least busy, hands its URL to the
// NodeClient, and on disconnect fails over to the next-best entry —
// without forcing every screen to know any of this happened.
//
// Selection rule:
//   * Lowest `load_score` (0..1, where 0 == idle and 1 == saturated;
//     a parallel agent populates it from routes.get / mininodes.list).
//   * Tiebreak on the round-trip time we observed from our own ping
//     against the active connection (lower is better).
//   * Final tiebreak on `last_seen_ms` descending (freshest wins) so
//     the choice is deterministic.
// Entries we've never heard a `load_score` for default to 0 — "assume
// free until told otherwise" — which keeps a fresh seed list dialable
// instead of stuck behind a more-loaded but recently-measured entry.
//
// Failover: the pool subscribes to NodeClient.onDisconnect. When the
// active socket drops, the pool calls `selectBest()` excluding the
// just-failed entry, hot-swaps NodeClient's URL via `setUrl()` (which
// rejects in-flight requests with `switching_url` so screens retry on
// the new endpoint), and lets NodeClient's normal connect() drive the
// reopen. If no other entry exists we leave the URL alone and let
// NodeClient's own auto-reconnect retry the same address — better
// than blackholing the player when there's literally nowhere else to
// go.
//
// Refresh: the periodic tick polls `mininodes.list` on the active
// NodeClient, merges newly-discovered entries into `_known` (and
// updates `load_score` / `last_seen_ms` on existing entries). RTT
// values aren't taken from the reply — we measure those ourselves
// against the currently-active connection via `mini.ping` so they
// correspond to what THIS browser sees, not what the mini-node sees
// about its neighbours.
//
// Persistence: `_known` is serialised to localStorage on every
// change, so a fresh page load keeps the previously-discovered set.
// RTT values aren't persisted — they're observation-time and would
// only mislead the next session.

import type { NodeClient } from './node_client';
import { RpcError } from './node_client';

/** A mini-node address plus the live metadata the pool uses to pick
 *  between entries. `host`/`port` are required; everything else is
 *  optional because a freshly-seeded entry has zero observations and
 *  a `mininodes.list` reply from an old node may omit the new
 *  fields. Callers should treat missing fields as "assume best-case"
 *  (load 0, RTT unknown, last_seen 0). */
export interface MiniNodeAddr {
  host: string;
  port: number;
  /** Server-reported load on the entry, 0..1. Missing => assume 0. */
  load_score?: number;
  /** Round-trip time we measured ourselves, in milliseconds. Only
   *  set for the currently-active entry (and any previously-active
   *  entry we kept a measurement for); fresh seeds have it undefined. */
  rtt_ms?: number;
  /** Wall-clock time (ms epoch) we last heard about this entry —
   *  populated from the `mininodes.list` reply, not by us. Missing =>
   *  0 (so it sorts last on the freshness tiebreak). */
  last_seen_ms?: number;
}

export interface MiniNodePoolOptions {
  /** Bootstrap addresses the pool starts with — typically the VPS
   *  the bundle is hosted from plus any hardcoded fallbacks. Merged
   *  with whatever's already in localStorage. */
  seeds?: MiniNodeAddr[];
  /** localStorage key the pool persists known addresses under.
   *  Defaults to `mc.known_mini_nodes` so it stays aligned with the
   *  `mc.*` keys main / settings / queue / playlists use. */
  storageKey?: string;
  /** How often to re-poll `mininodes.list` on the active socket.
   *  Defaults to 60 s — matches the old pool's tick. */
  refreshIntervalMs?: number;
  /** How often to send a `mini.ping` so we keep our own RTT estimate
   *  for the active entry fresh. Defaults to 15 s. */
  pingIntervalMs?: number;
  /** Per-request timeout for `mininodes.list` and `mini.ping`.
   *  Defaults to 6 s — same number the Dart side uses. */
  requestTimeoutMs?: number;
  /** Path component appended to ws://host:port/ when forming the
   *  WebSocket URL for an entry. Defaults to `/musicchain` — the
   *  same path the home-node gateway accepts. */
  wsPath?: string;
  /** Console-log selection + failover events when true. */
  debug?: boolean;
}

const DEFAULT_STORAGE_KEY = 'mc.known_mini_nodes';
const DEFAULT_REFRESH_MS = 60_000;
const DEFAULT_PING_MS = 15_000;
const DEFAULT_REQUEST_TIMEOUT_MS = 6_000;
const DEFAULT_WS_PATH = '/musicchain';

function keyOf(a: MiniNodeAddr): string {
  return `${a.host}:${a.port}`;
}

function parseHostPort(addr: string): { host: string; port: number } | null {
  // Use lastIndexOf so IPv6 literals (`[::1]:8080` style or raw colon
  // forms) don't get split on the wrong colon. Mirrors the librats
  // discovery agent's IPv6 fix.
  const colon = addr.lastIndexOf(':');
  if (colon <= 0) return null;
  const host = addr.substring(0, colon).trim();
  const port = Number(addr.substring(colon + 1));
  if (!host) return null;
  if (!Number.isFinite(port) || port <= 0 || port > 65535) return null;
  return { host, port };
}

/** Compose the WebSocket URL for an entry. ws:// for plain hosts;
 *  wss:// when the page itself is served over https so a mixed-content
 *  block doesn't quietly kill the dial. */
function wsUrlFor(addr: MiniNodeAddr, path: string): string {
  const isHttps = typeof location !== 'undefined' && location.protocol === 'https:';
  const proto = isHttps ? 'wss:' : 'ws:';
  // IPv6 literals must be bracketed in URLs. parseHostPort hands us a
  // bare host (no brackets); add them back when needed.
  const needsBrackets = addr.host.includes(':') && !addr.host.startsWith('[');
  const host = needsBrackets ? `[${addr.host}]` : addr.host;
  return `${proto}//${host}:${addr.port}${path}`;
}

/** Multi-mini-node directory + active-connection switcher. Owns no
 *  socket of its own — operates on the NodeClient handed to `start()`. */
export class MiniNodePool {
  private readonly _known = new Map<string, MiniNodeAddr>();
  private readonly _storageKey: string;
  private readonly _refreshMs: number;
  private readonly _pingMs: number;
  private readonly _requestTimeoutMs: number;
  private readonly _wsPath: string;
  private readonly _debug: boolean;

  private _node: NodeClient | null = null;
  private _activeKey: string | null = null;
  private _refreshTimer: ReturnType<typeof setInterval> | null = null;
  private _pingTimer: ReturnType<typeof setInterval> | null = null;
  private _started = false;

  // Saved-off NodeClient callbacks so `stop()` can restore them. The
  // pool composes with existing handlers rather than overwriting,
  // because callers (main.ts, screens) may have wired their own
  // onConnect / onDisconnect hooks before handing us the client.
  private _prevOnDisconnect: NodeClient['onDisconnect'] = null;
  private _prevOnConnect: NodeClient['onConnect'] = null;

  constructor(opts: MiniNodePoolOptions = {}) {
    this._storageKey = opts.storageKey ?? DEFAULT_STORAGE_KEY;
    this._refreshMs = opts.refreshIntervalMs ?? DEFAULT_REFRESH_MS;
    this._pingMs = opts.pingIntervalMs ?? DEFAULT_PING_MS;
    this._requestTimeoutMs = opts.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    this._wsPath = opts.wsPath ?? DEFAULT_WS_PATH;
    this._debug = opts.debug ?? false;

    // Seeds come first so a fresh install always has the hardcoded
    // bootstrap entry; storage layers on top so a returning user keeps
    // whatever was discovered last session.
    for (const a of opts.seeds ?? []) {
      this._known.set(keyOf(a), this._sanitise(a));
    }
    this._loadFromStorage();
  }

  /** Snapshot of known entries. Fresh array each call so iteration is
   *  safe against the refresh / ping ticks. */
  get known(): MiniNodeAddr[] {
    return Array.from(this._known.values());
  }

  /** Pick the best-scoring entry — lowest load_score, tiebreak on
   *  rtt_ms, final tiebreak on freshness. Entries listed in `exclude`
   *  are skipped (used by failover to avoid picking the one that just
   *  failed). Returns null if no entry qualifies. */
  selectBest(exclude: Iterable<string> = []): MiniNodeAddr | null {
    const banned = new Set(exclude);
    let best: MiniNodeAddr | null = null;
    let bestLoad = Number.POSITIVE_INFINITY;
    let bestRtt = Number.POSITIVE_INFINITY;
    let bestSeen = Number.NEGATIVE_INFINITY;
    for (const [k, addr] of this._known.entries()) {
      if (banned.has(k)) continue;
      const load = typeof addr.load_score === 'number'
        ? Math.max(0, Math.min(1, addr.load_score))
        : 0;
      // Unmeasured RTT sorts after measured RTT — we don't want to
      // flip away from a known-fast entry just because a never-pinged
      // peer reported the same load.
      const rtt = typeof addr.rtt_ms === 'number' && addr.rtt_ms >= 0
        ? addr.rtt_ms
        : Number.POSITIVE_INFINITY;
      const seen = typeof addr.last_seen_ms === 'number' ? addr.last_seen_ms : 0;
      if (load < bestLoad
          || (load === bestLoad && rtt < bestRtt)
          || (load === bestLoad && rtt === bestRtt && seen > bestSeen)) {
        best = addr;
        bestLoad = load;
        bestRtt = rtt;
        bestSeen = seen;
      }
    }
    return best;
  }

  /** WebSocket URL of the currently-selected mini-node, or empty
   *  string before `start()` has run (or if no entry was selectable). */
  activeUrl(): string {
    if (!this._activeKey) return '';
    const addr = this._known.get(this._activeKey);
    return addr ? wsUrlFor(addr, this._wsPath) : '';
  }

  /** Bind to a NodeClient, pick the best entry, point the client at
   *  it, and arm the refresh + ping ticks. Idempotent — calling twice
   *  is a no-op. The caller still owns the NodeClient (we don't call
   *  connect or disconnect on it ourselves; setUrl drives the dial). */
  start(node: NodeClient): void {
    if (this._started) return;
    this._started = true;
    this._node = node;

    // Compose onto whatever handlers the caller already wired so we
    // don't clobber them. We swap back in stop().
    this._prevOnDisconnect = node.onDisconnect;
    this._prevOnConnect = node.onConnect;

    node.onDisconnect = (reason: string) => {
      try { this._prevOnDisconnect?.(reason); } catch (_) { /* swallow */ }
      this._handleDisconnect(reason);
    };
    node.onConnect = () => {
      try { this._prevOnConnect?.(); } catch (_) { /* swallow */ }
      // Refresh the directory once the new connection is live so a
      // failover redial immediately learns about any neighbours the
      // new active mini-node knows about (and the previous one
      // didn't). Fire-and-forget — the ping timer doesn't need this.
      void this._refresh();
    };

    // Pick + apply initial selection. If the caller already pointed
    // the NodeClient at a URL that matches our best pick, setUrl is
    // a no-op and no socket churn happens.
    const pick = this.selectBest();
    if (pick) {
      this._activeKey = keyOf(pick);
      const url = wsUrlFor(pick, this._wsPath);
      if (this._debug) {
        // eslint-disable-next-line no-console
        console.info('[mini-pool] initial pick', url,
                     '(load=', pick.load_score ?? 0, ')');
      }
      node.setUrl(url);
    } else if (this._debug) {
      // eslint-disable-next-line no-console
      console.info('[mini-pool] no known entries — leaving NodeClient URL alone');
    }

    this._refreshTimer = setInterval(() => { void this._refresh(); },
                                     this._refreshMs);
    this._pingTimer = setInterval(() => { void this._pingActive(); },
                                  this._pingMs);
  }

  /** Manual refresh — call after a connect-state change (fresh page
   *  load, network back online) to widen the pool without waiting for
   *  the periodic tick. */
  async refreshNow(): Promise<void> {
    if (!this._node) return;
    await this._refresh();
  }

  /** Cancel the timers, restore the NodeClient's prior handlers, and
   *  forget the active selection. Doesn't disconnect the client. */
  stop(): void {
    if (this._refreshTimer) {
      clearInterval(this._refreshTimer);
      this._refreshTimer = null;
    }
    if (this._pingTimer) {
      clearInterval(this._pingTimer);
      this._pingTimer = null;
    }
    if (this._node) {
      this._node.onDisconnect = this._prevOnDisconnect;
      this._node.onConnect = this._prevOnConnect;
    }
    this._prevOnDisconnect = null;
    this._prevOnConnect = null;
    this._node = null;
    this._activeKey = null;
    this._started = false;
  }

  /** Manually add an entry — used by Settings when the user pastes a
   *  custom VPS address. New entries are NOT auto-dialed; they enter
   *  the pool and become eligible at the next pick. Returns true if
   *  the entry was actually new. */
  add(addr: MiniNodeAddr): boolean {
    const k = keyOf(addr);
    if (this._known.has(k)) return false;
    this._known.set(k, this._sanitise(addr));
    this._saveToStorage();
    return true;
  }

  // -- internals -------------------------------------------------------

  /** Strip RTT (we measure that ourselves) and clamp load_score so a
   *  malformed reply can't poison the heuristic. host/port are
   *  trusted — the caller is expected to have parsed those already. */
  private _sanitise(addr: MiniNodeAddr): MiniNodeAddr {
    const out: MiniNodeAddr = { host: addr.host, port: addr.port };
    if (typeof addr.load_score === 'number'
        && Number.isFinite(addr.load_score)) {
      out.load_score = Math.max(0, Math.min(1, addr.load_score));
    }
    if (typeof addr.last_seen_ms === 'number'
        && Number.isFinite(addr.last_seen_ms)) {
      out.last_seen_ms = addr.last_seen_ms;
    }
    if (typeof addr.rtt_ms === 'number'
        && Number.isFinite(addr.rtt_ms) && addr.rtt_ms >= 0) {
      out.rtt_ms = addr.rtt_ms;
    }
    return out;
  }

  private _handleDisconnect(reason: string): void {
    const node = this._node;
    if (!node) return;
    const failed = this._activeKey;
    if (this._debug) {
      // eslint-disable-next-line no-console
      console.info('[mini-pool] disconnect', failed, 'reason=', reason);
    }
    // Drop our cached RTT for the failed entry — whatever it was, the
    // socket just died so the measurement no longer reflects reality.
    if (failed) {
      const addr = this._known.get(failed);
      if (addr && addr.rtt_ms !== undefined) {
        const next: MiniNodeAddr = { host: addr.host, port: addr.port };
        if (addr.load_score !== undefined) next.load_score = addr.load_score;
        if (addr.last_seen_ms !== undefined) next.last_seen_ms = addr.last_seen_ms;
        this._known.set(failed, next);
      }
    }

    // Pick again, excluding the just-failed entry so we never fail
    // straight back to the same dead VPS. If selection picks nothing
    // (e.g. single-entry pool), leave the URL alone — NodeClient's
    // own auto-reconnect will retry the existing URL.
    const excl = failed ? [failed] : [];
    const pick = this.selectBest(excl);
    if (!pick) {
      if (this._debug) {
        // eslint-disable-next-line no-console
        console.info('[mini-pool] no failover candidate — leaving URL alone');
      }
      return;
    }
    const newKey = keyOf(pick);
    if (newKey === failed) {
      // Same entry would have won the re-pick; nothing to do.
      return;
    }
    this._activeKey = newKey;
    const url = wsUrlFor(pick, this._wsPath);
    if (this._debug) {
      // eslint-disable-next-line no-console
      console.info('[mini-pool] failover to', url,
                   '(load=', pick.load_score ?? 0, ')');
    }
    // setUrl tears the old (already-closed) socket down cleanly and
    // dials the new URL — NodeClient.onConnect will fire on success
    // and our composed handler kicks the refresh.
    node.setUrl(url);
  }

  private async _refresh(): Promise<void> {
    const node = this._node;
    if (!node || !node.isConnected) return;
    let reply: unknown;
    try {
      reply = await node.request<unknown>(
        'mininodes.list', {}, this._requestTimeoutMs);
    } catch (err) {
      // Mini-node doesn't implement the verb, or the call timed out.
      // Neither is fatal; we'll try again on the next tick.
      if (this._debug && !(err instanceof RpcError)) {
        // eslint-disable-next-line no-console
        console.warn('[mini-pool] refresh failed', err);
      }
      return;
    }
    // Accept either a bare array or `{peers: […]}` shape — different
    // verb revisions wrap the list differently and we want both.
    const entries = Array.isArray(reply)
      ? reply
      : (reply && typeof reply === 'object'
          && Array.isArray((reply as { peers?: unknown }).peers))
        ? (reply as { peers: unknown[] }).peers
        : [];

    let changed = false;
    for (const raw of entries) {
      if (!raw || typeof raw !== 'object') continue;
      const e = raw as {
        public_address?: unknown;
        host?: unknown;
        port?: unknown;
        load_score?: unknown;
        last_seen_ms?: unknown;
      };
      let host: string | null = null;
      let port: number | null = null;
      if (typeof e.public_address === 'string') {
        const parsed = parseHostPort(e.public_address);
        if (parsed) { host = parsed.host; port = parsed.port; }
      } else if (typeof e.host === 'string' && typeof e.port === 'number') {
        host = e.host;
        port = e.port;
      }
      if (!host || port === null
          || !Number.isFinite(port) || port <= 0 || port > 65535) continue;

      const next: MiniNodeAddr = { host, port };
      if (typeof e.load_score === 'number' && Number.isFinite(e.load_score)) {
        next.load_score = Math.max(0, Math.min(1, e.load_score));
      }
      if (typeof e.last_seen_ms === 'number' && Number.isFinite(e.last_seen_ms)) {
        next.last_seen_ms = e.last_seen_ms;
      }

      const k = keyOf(next);
      const prev = this._known.get(k);
      if (!prev) {
        this._known.set(k, next);
        changed = true;
      } else {
        // Preserve the RTT we've been measuring locally — the reply
        // never carries our observation. Only overwrite load_score /
        // last_seen_ms when the reply actually had them so a partial
        // reply doesn't reset values we already had.
        const merged: MiniNodeAddr = { host: prev.host, port: prev.port };
        if (prev.rtt_ms !== undefined) merged.rtt_ms = prev.rtt_ms;
        if (next.load_score !== undefined) merged.load_score = next.load_score;
        else if (prev.load_score !== undefined) merged.load_score = prev.load_score;
        if (next.last_seen_ms !== undefined) merged.last_seen_ms = next.last_seen_ms;
        else if (prev.last_seen_ms !== undefined) merged.last_seen_ms = prev.last_seen_ms;
        // Only count as changed if a value actually moved — avoids
        // serialising on every quiescent tick.
        if (merged.load_score !== prev.load_score
            || merged.last_seen_ms !== prev.last_seen_ms) {
          changed = true;
        }
        this._known.set(k, merged);
      }
    }
    if (changed) {
      this._saveToStorage();
      if (this._debug) {
        // eslint-disable-next-line no-console
        console.info('[mini-pool] pool size:', this._known.size);
      }
      // If the load_score story for some entry shifted enough to
      // change the best pick (e.g. the currently-active mini-node
      // just reported saturation), let the next disconnect handle
      // the swap. We deliberately don't preempt mid-session — a hot
      // swap on every load update would thrash the audio bridge.
    }
  }

  /** Send `mini.ping` on the active socket and record the elapsed
   *  time as the active entry's RTT. Best-effort: any error (verb
   *  unknown, timeout, socket flapping) just leaves the previous RTT
   *  in place. */
  private async _pingActive(): Promise<void> {
    const node = this._node;
    if (!node || !node.isConnected) return;
    const key = this._activeKey;
    if (!key) return;
    const addr = this._known.get(key);
    if (!addr) return;
    const start = (typeof performance !== 'undefined'
                   ? performance.now()
                   : Date.now());
    try {
      await node.request<unknown>('mini.ping', {}, this._requestTimeoutMs);
    } catch {
      // Verb may not exist on older mini-nodes — fall through and
      // try again next tick.
      return;
    }
    const elapsed = (typeof performance !== 'undefined'
                     ? performance.now()
                     : Date.now()) - start;
    if (!Number.isFinite(elapsed) || elapsed < 0) return;
    const updated: MiniNodeAddr = { host: addr.host, port: addr.port, rtt_ms: elapsed };
    if (addr.load_score !== undefined) updated.load_score = addr.load_score;
    if (addr.last_seen_ms !== undefined) updated.last_seen_ms = addr.last_seen_ms;
    this._known.set(key, updated);
    // RTT isn't persisted — see top-of-file comment.
  }

  private _loadFromStorage(): void {
    try {
      if (typeof localStorage === 'undefined') return;
      const raw = localStorage.getItem(this._storageKey);
      if (!raw) return;
      const parsed: unknown = JSON.parse(raw);
      if (!Array.isArray(parsed)) return;
      for (const entry of parsed) {
        // Accept both string "host:port" entries (Dart format) and
        // {host, port, …} objects (our preferred shape).
        let candidate: MiniNodeAddr | null = null;
        if (typeof entry === 'string') {
          const parsedHp = parseHostPort(entry);
          if (parsedHp) candidate = { host: parsedHp.host, port: parsedHp.port };
        } else if (entry && typeof entry === 'object') {
          const e = entry as {
            host?: unknown;
            port?: unknown;
            load_score?: unknown;
            last_seen_ms?: unknown;
          };
          if (typeof e.host === 'string' && typeof e.port === 'number'
              && e.port > 0 && e.port <= 65535 && e.host.length > 0) {
            const c: MiniNodeAddr = { host: e.host, port: e.port };
            if (typeof e.load_score === 'number'
                && Number.isFinite(e.load_score)) {
              c.load_score = Math.max(0, Math.min(1, e.load_score));
            }
            if (typeof e.last_seen_ms === 'number'
                && Number.isFinite(e.last_seen_ms)) {
              c.last_seen_ms = e.last_seen_ms;
            }
            candidate = c;
          }
        }
        if (!candidate) continue;
        const k = keyOf(candidate);
        if (!this._known.has(k)) this._known.set(k, candidate);
      }
    } catch {
      // localStorage unavailable / quota / parse failure — fall back
      // to whatever was seeded in the constructor.
    }
  }

  private _saveToStorage(): void {
    try {
      if (typeof localStorage === 'undefined') return;
      const list = Array.from(this._known.values()).map((a) => {
        const out: Record<string, unknown> = { host: a.host, port: a.port };
        if (a.load_score !== undefined) out['load_score'] = a.load_score;
        if (a.last_seen_ms !== undefined) out['last_seen_ms'] = a.last_seen_ms;
        // Deliberately omit rtt_ms — observation-time only.
        return out;
      });
      localStorage.setItem(this._storageKey, JSON.stringify(list));
    } catch {
      // Best effort. Quota errors in private mode are common.
    }
  }
}
