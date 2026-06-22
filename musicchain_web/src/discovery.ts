// Node discovery for the browser player.
//
// Mirrors musicchain_player/lib/src/services/librats_discovery.dart:
//   * Open a WebSocket to a seed URL (the VPS mini-node's WS bridge, or a
//     hardcoded fallback the user configured).
//   * Send the `routes.get` RPC the mini-node serves and get back its
//     current routing table (full nodes that have published a routes
//     record, with reachability + load + last_seen_ms).
//   * Score the candidates the same way the Dart player does
//     (load + busy penalty + staleness, with a fallback 50 ms ping) and
//     pick the best home node.
//   * Fire the `autoNode` event with the picked WebSocket URL so
//     screens / NodeClient consumers can rewire onto the chosen node.
//
// Fallback path: if the seed URL is itself a home node (older deploys, or
// the user explicitly pointed `mc.node_url` at one), `routes.get` comes
// back `unknown_type`. We catch that, treat the seed as the picked URL,
// and emit the same `autoNode` event so callers don't need a separate
// code path for "no mini-node available yet".
//
// Until the WS bridge on the VPS mini-node actually ships, the realistic
// production path is the fallback: `mc.node_url` points at a single home
// node and we just publish that URL. `mc.mini_url`, when set, is tried
// first so the player can flip over to mini-node-driven discovery the
// moment the bridge is live without a code change.

/** localStorage key for the user-pinned home-node WebSocket URL. Used by
 *  main.ts to seed the initial NodeClient — kept here as a constant so
 *  any future settings.ts screen can reuse the same key without drift. */
export const NODE_URL_KEY = 'mc.node_url';

/** localStorage key for the mini-node WebSocket seed URL. When present,
 *  start() prefers this over the seed argument so a user-configured
 *  override always wins. */
export const MINI_URL_KEY = 'mc.mini_url';

/** One row in the mini-node's routes.get reply. Field names mirror the
 *  JSON the C++ mini-node emits (see mini_node.cpp::routes_json). Every
 *  field is optional because routes published by older nodes / mini-
 *  nodes mid-upgrade may be missing newer keys; callers must tolerate
 *  undefined and use sensible defaults. */
export interface RouteRecord {
  node_id?: string;
  rats_peer_id?: string;
  ipv4?: string;
  ipv6?: string;
  api_port?: number;
  api_url?: string;
  public_address?: string;
  reachability?: 'direct' | 'relay' | 'unknown' | string;
  load_score?: number;
  cpu_load?: number;
  net_bps?: number;
  is_busy?: boolean;
  last_seen_ms?: number;
}

/** Payload of the `autoNode` CustomEvent fired when discovery picks (or
 *  re-picks) a home node. `pickedUrl` is always a ws:// or wss:// URL
 *  the NodeClient can connect to directly. `source` distinguishes a
 *  mini-node-driven pick from the fallback where the seed itself was a
 *  home node. `record` is the underlying route row when available, so
 *  UI can show reachability / load. */
export interface AutoNodeDetail {
  pickedUrl: string;
  source: 'mini' | 'direct-fallback';
  record?: RouteRecord;
  routes: RouteRecord[];
}

/** Minimum WebSocket subset NodeDiscovery needs — typed loosely so the
 *  unit tests in tests/ can swap in a stub without pulling DOM types. */
interface MinimalSocket {
  readyState: number;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: Event) => void) | null;
  onmessage: ((ev: MessageEvent) => void) | null;
  onclose: ((ev: CloseEvent) => void) | null;
  onerror: ((ev: Event) => void) | null;
}

/** Per-instance options. The socketFactory hook is only exercised by
 *  tests; production code uses the global WebSocket. */
export interface NodeDiscoveryOptions {
  /** Override the WebSocket constructor (tests inject a fake). */
  socketFactory?: (url: string) => MinimalSocket;
  /** Max time to wait for a routes.get reply before declaring fallback,
   *  default 4 s. */
  routesTimeoutMs?: number;
  /** Override the wall clock — same hook the scoring used to weigh
   *  staleness. Tests pin this for deterministic ordering. */
  now?: () => number;
}

const ROUTES_TIMEOUT_DEFAULT_MS = 4_000;

// Default ping the Dart player assumes when per-peer latency isn't yet
// plumbed through. Keep these in lockstep with librats_discovery.dart so
// "best" means the same thing across Flutter and the web.
const FALLBACK_PING_MS = 50;

/** Try to turn an `api_url` ("http://1.2.3.4:9334") into a WebSocket URL
 *  the home node's WS gateway accepts ("ws://1.2.3.4:9334/musicchain").
 *  Returns null when we can't make sense of the input — the caller falls
 *  back to the next-best candidate. The home node serves the bundle and
 *  the WS gateway from the same origin under /musicchain (see main.ts),
 *  so a same-port + path rewrite is correct. Exported so MiniNodePool
 *  can reuse the same normalisation when ranking mini-node candidates
 *  from `routes.get` results. */
export function apiUrlToWsUrl(apiUrl: string | undefined): string | null {
  if (!apiUrl) return null;
  const trimmed = apiUrl.trim();
  if (!trimmed) return null;
  try {
    const u = new URL(trimmed);
    if (u.protocol === 'http:')  u.protocol = 'ws:';
    else if (u.protocol === 'https:') u.protocol = 'wss:';
    else if (u.protocol !== 'ws:' && u.protocol !== 'wss:') return null;
    // The home node accepts the chain RPC envelope on /musicchain. Keep
    // any explicit pathname the caller already supplied — that way an
    // operator who put the gateway behind a reverse proxy at a custom
    // path still works.
    if (u.pathname === '' || u.pathname === '/') u.pathname = '/musicchain';
    return u.toString();
  } catch {
    return null;
  }
}

/** Score a route the same way the Dart player does in
 *  librats_discovery.dart::refresh. Lower = better. Exported for tests. */
export function scoreRoute(r: RouteRecord, nowMs: number): number {
  const loadScore = typeof r.load_score === 'number' ? r.load_score : 0;
  const isBusy = r.is_busy === true;
  const seenMs = typeof r.last_seen_ms === 'number' ? r.last_seen_ms : 0;
  const ageMs = nowMs - seenMs;
  const staleness = ageMs <= 120_000
    ? 1.0
    : Math.min(10.0, Math.max(1.0, 1.0 + (ageMs - 120_000) / 200_000));
  const clampedLoad = Math.min(1.0, Math.max(0.0, loadScore));
  const loadPart = 1.0 + 2.0 * clampedLoad;
  const busyPart = isBusy ? 4.0 : 1.0;
  return FALLBACK_PING_MS * loadPart * busyPart * staleness;
}

/** Sort copy of `routes` by the same precedence the Dart player uses:
 *  score ascending, tiebreak on freshness descending. */
export function rankRoutes(routes: RouteRecord[], nowMs: number): RouteRecord[] {
  const out = routes.slice();
  out.sort((a, b) => {
    const cmp = scoreRoute(a, nowMs) - scoreRoute(b, nowMs);
    if (cmp !== 0) return cmp;
    const fa = typeof a.last_seen_ms === 'number' ? a.last_seen_ms : 0;
    const fb = typeof b.last_seen_ms === 'number' ? b.last_seen_ms : 0;
    return fb - fa;
  });
  return out;
}

/** Discovery driver. Construct one, wire an `autoNode` listener, then
 *  call `start(seedUrl)`. The picked URL is the .detail.pickedUrl of the
 *  CustomEvent. Call `refresh()` to re-pick on demand. */
export class NodeDiscovery extends EventTarget {
  private readonly socketFactory: (url: string) => MinimalSocket;
  private readonly routesTimeoutMs: number;
  private readonly now: () => number;

  /** The seed URL the most recent `start()` call locked onto. Refresh
   *  reuses this; the user override (`mc.mini_url`) is checked on every
   *  start() but not on refresh(), so a setting change requires a fresh
   *  start(). Kept package-internal — settings UIs should call start()
   *  with the new value after persisting. */
  private seedNodeUrl = '';

  /** Most recent route list, populated on every successful refresh. */
  routes: RouteRecord[] = [];

  /** Most recently picked URL. Updated in lockstep with the `autoNode`
   *  event so consumers that subscribe after the first pick can read it
   *  directly instead of buffering events. Empty string until first
   *  pick. */
  pickedUrl = '';

  /** Set when the last attempt failed; cleared on the next success.
   *  Caller-visible via the `error` getter so the settings screen can
   *  show "VPS error: …" the same way Dart does. */
  lastError = '';

  /** Whether a routes.get is currently in flight. */
  isRefreshing = false;

  constructor(opts: NodeDiscoveryOptions = {}) {
    super();
    this.socketFactory = opts.socketFactory
      ?? ((url) => new WebSocket(url) as unknown as MinimalSocket);
    this.routesTimeoutMs = opts.routesTimeoutMs ?? ROUTES_TIMEOUT_DEFAULT_MS;
    this.now = opts.now ?? (() => Date.now());
  }

  /** Connect to the seed, ask for routes, pick the best home node, and
   *  emit `autoNode`. Returns once the first pick has fired (or the
   *  fallback, when routes.get isn't supported). On socket or RPC
   *  failure, resolves anyway after emitting `autoNode` with the seed
   *  itself as the pick — better to let the player try the seed than to
   *  reject and leave the UI without a node. */
  async start(seedNodeUrl: string): Promise<void> {
    // User-configured override wins over whatever main.ts passed.
    const miniOverride = this.readLocalStorage(MINI_URL_KEY);
    this.seedNodeUrl = (miniOverride && miniOverride.trim())
      || seedNodeUrl.trim();
    if (!this.seedNodeUrl) {
      // No seed at all — fall back to the user's pinned home node, or to
      // the page-host default. Without this the player has nothing to
      // call and we'd silently never fire `autoNode`.
      const pinned = this.readLocalStorage(NODE_URL_KEY);
      if (pinned && pinned.trim()) {
        this.emitPick(pinned.trim(), 'direct-fallback', undefined, []);
        return;
      }
      const guess = this.guessHomeNodeFromPageOrigin();
      this.emitPick(guess, 'direct-fallback', undefined, []);
      return;
    }
    await this.refresh();
  }

  /** Re-fetch the routing table from the most recent seed URL and
   *  re-pick. Safe to call concurrently — the second call is a no-op
   *  while the first is in flight. */
  async refresh(): Promise<void> {
    if (this.isRefreshing) return;
    if (!this.seedNodeUrl) return;
    this.isRefreshing = true;
    try {
      const routes = await this.fetchRoutes(this.seedNodeUrl);
      this.routes = routes;
      this.lastError = '';
      const pinned = this.readLocalStorage(NODE_URL_KEY);
      if (pinned && pinned.trim()) {
        // User pinned a specific home node — honor that over the
        // mini-node's recommendation. The route record (if any) is
        // still attached so UI can show reachability/load.
        const pinnedUrl = pinned.trim();
        const match = routes.find((r) =>
          apiUrlToWsUrl(r.api_url) === pinnedUrl);
        this.emitPick(pinnedUrl, 'mini', match, routes);
        return;
      }
      const ranked = rankRoutes(routes, this.now());
      // Pick the first ranked candidate that produces a usable WS URL.
      // A route may lack a public_address (mini-node hasn't STUN-probed
      // it yet) — skip it rather than emit an empty URL.
      let picked: { url: string; record: RouteRecord } | null = null;
      for (const r of ranked) {
        const wsUrl = apiUrlToWsUrl(r.api_url);
        if (wsUrl) { picked = { url: wsUrl, record: r }; break; }
      }
      if (picked) {
        this.emitPick(picked.url, 'mini', picked.record, routes);
      } else {
        // Mini-node knew of zero usable home nodes. Fall back to the
        // pinned home node or the page-origin guess so the player has
        // something to try.
        const fallback = this.fallbackHomeNode();
        this.emitPick(fallback, 'direct-fallback', undefined, routes);
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      this.lastError = msg;
      // The seed didn't answer routes.get (either it doesn't support
      // the verb because it's a home node, or the bridge is down).
      // Treat the seed itself as the picked URL — same behavior the
      // task description calls out.
      this.emitPick(this.seedNodeUrl, 'direct-fallback', undefined, []);
    } finally {
      this.isRefreshing = false;
    }
  }

  // -- Internals -------------------------------------------------------

  /** Open a short-lived WebSocket to `seedUrl`, send the routes.get
   *  envelope, and return the parsed peer list. Times out after
   *  routesTimeoutMs. The socket is closed in every exit path. */
  private fetchRoutes(seedUrl: string): Promise<RouteRecord[]> {
    return new Promise<RouteRecord[]>((resolve, reject) => {
      let ws: MinimalSocket;
      try {
        ws = this.socketFactory(seedUrl);
      } catch (e) {
        reject(e instanceof Error ? e : new Error(String(e)));
        return;
      }
      const reqId = newReqId();
      let settled = false;
      const finalize = (cb: () => void) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        try { ws.close(1000, 'discovery done'); } catch { /* swallow */ }
        cb();
      };

      const timer = setTimeout(() => {
        finalize(() =>
          reject(new Error(`routes.get timed out after ${this.routesTimeoutMs}ms`)));
      }, this.routesTimeoutMs);

      ws.onopen = () => {
        try {
          ws.send(JSON.stringify({
            req_id: reqId,
            type: 'routes.get',
            body: {},
          }));
        } catch (e) {
          finalize(() => reject(e instanceof Error ? e : new Error(String(e))));
        }
      };
      ws.onmessage = (ev: MessageEvent) => {
        if (typeof ev.data !== 'string') return;
        let env: {
          req_id?: string;
          status?: string;
          body?: { peers?: unknown };
          error?: string;
        };
        try { env = JSON.parse(ev.data); } catch { return; }
        if (env.req_id !== reqId) return;
        if (env.status !== 'ok') {
          finalize(() => reject(new Error(
            env.error || `routes.get status: ${env.status ?? '(missing)'}`)));
          return;
        }
        const peersRaw = env.body?.peers;
        const peers = Array.isArray(peersRaw) ? (peersRaw as RouteRecord[]) : [];
        finalize(() => resolve(peers));
      };
      ws.onerror = () => {
        finalize(() => reject(new Error(`websocket error talking to ${seedUrl}`)));
      };
      ws.onclose = (ev: CloseEvent) => {
        // If the socket closed before we got our reply, surface that —
        // the timer would otherwise hold the promise for routesTimeoutMs.
        finalize(() => reject(new Error(
          `websocket closed before routes.get reply: ${ev.reason || ev.code}`)));
      };
    });
  }

  /** Compute a sensible direct-connection URL when we have no mini-node
   *  to consult. Preference order: pinned `mc.node_url`, then the same
   *  page-origin guess main.ts uses. */
  private fallbackHomeNode(): string {
    const pinned = this.readLocalStorage(NODE_URL_KEY);
    if (pinned && pinned.trim()) return pinned.trim();
    return this.guessHomeNodeFromPageOrigin();
  }

  private guessHomeNodeFromPageOrigin(): string {
    // Identical to main.ts.resolveNodeUrl()'s fallback so the two
    // entrypoints agree. We can't import that function without making
    // main.ts a peer dependency of discovery.ts (circular for tests),
    // so the constant is duplicated and the comment flags it.
    if (typeof location !== 'undefined' && location.host) {
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
      return `${proto}//${location.host}/musicchain`;
    }
    return 'wss://localhost/musicchain';
  }

  private emitPick(
    url: string,
    source: 'mini' | 'direct-fallback',
    record: RouteRecord | undefined,
    routes: RouteRecord[],
  ): void {
    this.pickedUrl = url;
    const detail: AutoNodeDetail = record !== undefined
      ? { pickedUrl: url, source, record, routes }
      : { pickedUrl: url, source, routes };
    this.dispatchEvent(new CustomEvent('autoNode', { detail }));
  }

  private readLocalStorage(key: string): string | null {
    try {
      if (typeof localStorage === 'undefined') return null;
      return localStorage.getItem(key);
    } catch {
      // Safari private mode + tracker blockers can throw on access.
      return null;
    }
  }
}

/** Persist the user's manual home-node override. Pass an empty string to
 *  clear it. Failures (private mode, quota) are swallowed and returned
 *  as `false` so the settings screen can show a non-fatal warning. */
export function setNodeUrl(url: string): boolean {
  return writeLocalStorage(NODE_URL_KEY, url);
}

/** Persist the user's mini-node seed override. Same semantics as
 *  setNodeUrl. */
export function setMiniUrl(url: string): boolean {
  return writeLocalStorage(MINI_URL_KEY, url);
}

/** Read the pinned home-node URL (or empty string). */
export function getNodeUrl(): string {
  return readLocalStorage(NODE_URL_KEY);
}

/** Read the pinned mini-node seed URL (or empty string). */
export function getMiniUrl(): string {
  return readLocalStorage(MINI_URL_KEY);
}

function readLocalStorage(key: string): string {
  try {
    if (typeof localStorage === 'undefined') return '';
    return localStorage.getItem(key) ?? '';
  } catch {
    return '';
  }
}

function writeLocalStorage(key: string, value: string): boolean {
  try {
    if (typeof localStorage === 'undefined') return false;
    if (value === '') localStorage.removeItem(key);
    else              localStorage.setItem(key, value);
    return true;
  } catch {
    return false;
  }
}

function newReqId(): string {
  const c = (globalThis as { crypto?: Crypto }).crypto;
  if (c && typeof c.randomUUID === 'function') return c.randomUUID();
  const bytes = new Uint8Array(16);
  if (c && typeof c.getRandomValues === 'function') {
    c.getRandomValues(bytes);
  } else {
    for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  }
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const h = Array.from(bytes, (b) => b.toString(16).padStart(2, '0'));
  return `${h.slice(0, 4).join('')}-${h.slice(4, 6).join('')}-${h.slice(6, 8).join('')}-${h.slice(8, 10).join('')}-${h.slice(10, 16).join('')}`;
}
