// WebSocket-based RPC client for the browser player.
//
// As of 2026-06-21 the browser connects to a MINI-NODE WebSocket gateway
// (the lightweight relay deployed alongside the VPS). The mini-node then
// forwards verbs to home nodes over librats via `relay.forward`. The
// home node's own ws_bridge (port 9090) is now legacy / optional — full
// nodes may still expose it for local dev but the production browser
// path goes through the mini-node.
//
// The Dart-side equivalent lives at musicchain_player/lib/src/services/
// node_client.dart + rats_client.dart. Those wire envelopes onto librats
// typed messages; the browser can't speak librats, so the mini-node
// exposes a WebSocket gateway that carries the same JSON envelopes:
//
//   request:  { req_id, type, body }
//   reply:    { req_id, status, body? , error? }
//
// One JSON document per WebSocket text frame. `status == "ok"` resolves
// the matching pending request with `body`; anything else rejects with
// `error` (or `status`).
//
// Server-pushed envelopes that DON'T match a pending req_id are routed
// to the `onIncomingRequest` callback so callers (e.g. the live now-
// playing widget) can react without polling.

export type ReqId = string;

/** Listener invoked when the home node pushes us an envelope whose
 *  `req_id` doesn't match anything we're awaiting. The payload's
 *  `type` is the verb name; `body` is the JSON body. */
export type IncomingHandler = (msg: {
  req_id: string;
  type: string;
  body: unknown;
}) => void;

/** Listener invoked for raw binary WebSocket frames received over the
 *  gateway. The mini-node uses these to carry chunked payloads for verbs
 *  like `audio.fetch`: the verb's text reply contains the `stream_id` and
 *  total size, then bytes arrive as one or more binary frames whose first
 *  four bytes are the stream_id in big-endian (matching the C++ side's
 *  `htonl` framing). AudioBridge installs this to demux chunks by
 *  stream_id; everything else can leave the callback null. */
export type BinaryFrameHandler = (frame: ArrayBuffer) => void;

/** Listener invoked when a text envelope arrives carrying a `req_id`
 *  whose request has already resolved. Long-running streaming verbs
 *  (audio.fetch, future bulk transfers) send a follow-up
 *  `status: "complete"` envelope after the initial `status: "ok"` reply
 *  has been delivered; this hook lets the streaming layer catch that
 *  final envelope without re-using the single-shot pending-request slot. */
export type UnmatchedReplyHandler = (env: {
  req_id: string;
  status: string;
  body: unknown;
  error?: string;
}) => void;

export interface NodeClientOptions {
  url?: string;
  /** Max time to wait for any single reply, default 15s. */
  defaultTimeoutMs?: number;
  /** Reconnect on unexpected close. Defaults to true. */
  autoReconnect?: boolean;
}

/** Error thrown by `request` when the server replies non-ok or the
 *  client is torn down before a reply arrives. */
export class RpcError extends Error {
  constructor(public readonly status: string, message: string) {
    super(message);
    this.name = 'RpcError';
  }
}

interface Pending {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

const DEFAULT_TIMEOUT_MS = 15_000;
const RECONNECT_INITIAL_MS = 1_000;
const RECONNECT_CAP_MS = 30_000;

function defaultUrl(): string {
  // wss://<page-host>/musicchain by default — the mini-node serves the
  // web bundle and the gateway from the same origin, so the browser's
  // location.host gives us the right thing in production. In dev the
  // Vite proxy on /musicchain forwards upstream to the mini-node WS
  // (default ws://85.239.238.226:8082, override with MUSICCHAIN_MINI_WS).
  if (typeof location !== 'undefined' && location.host) {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/musicchain`;
  }
  return 'wss://localhost/musicchain';
}

function newReqId(): string {
  // crypto.randomUUID is supported in every browser this project targets
  // (Chromium, Firefox, Safari 15.4+, Edge). Falls back to a manual v4
  // build for the rare environment that doesn't expose it.
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

export class NodeClient {
  private url: string;
  private readonly defaultTimeoutMs: number;
  private readonly autoReconnect: boolean;

  private ws: WebSocket | null = null;
  private readonly pending = new Map<ReqId, Pending>();

  // Reconnect bookkeeping.
  private reconnectAttempt = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private connecting: Promise<void> | null = null;
  private closedByUser = false;

  // Event handlers (set as fields; multiple handlers can be wired by
  // composing callbacks externally — we keep the surface tiny).
  onConnect: (() => void) | null = null;
  onDisconnect: ((reason: string) => void) | null = null;
  onError: ((err: Event | Error) => void) | null = null;
  onIncomingRequest: IncomingHandler | null = null;
  /** Fires after `setUrl()` has actually swapped the URL (no-op calls
   *  don't trigger it). Receives the previous URL and the new one so a
   *  pool watcher can log / surface the failover decision. The
   *  underlying disconnect + redial still drives onDisconnect / onConnect
   *  in addition to this. */
  onUrlChange: ((prev: string, next: string) => void) | null = null;
  /** Streaming demux hook — fires once per binary frame, with the raw
   *  ArrayBuffer. Installed by AudioBridge to route bytes to the
   *  matching stream_id. For listener-style fan-out (multiple consumers
   *  of the same frame) use `addBinaryListener` — both the field and the
   *  list-registered callbacks fire per frame. */
  onBinaryFrame: BinaryFrameHandler | null = null;
  /** Streaming end-of-transfer hook — fires when a text envelope with a
   *  `req_id` we no longer have pending arrives carrying a `status`
   *  (e.g. `"complete"`). AudioBridge uses this to detect end-of-stream
   *  without race-stealing the initial `status: "ok"` reply. */
  onUnmatchedReply: UnmatchedReplyHandler | null = null;
  private readonly binaryListeners: Array<(bytes: Uint8Array) => void> = [];

  constructor(opts: NodeClientOptions = {}) {
    this.url = opts.url ?? defaultUrl();
    this.defaultTimeoutMs = opts.defaultTimeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.autoReconnect = opts.autoReconnect ?? true;
  }

  /** True while the underlying WebSocket is in OPEN state. */
  get isConnected(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  /** Current WebSocket URL the client is dialing / connected to. */
  getUrl(): string {
    return this.url;
  }

  /** Hot-swap the WebSocket URL. If `newUrl` matches the current URL
   *  this is a no-op (no callbacks fire, no socket churn). Otherwise:
   *    1. Cancel any pending reconnect timer.
   *    2. Reject every in-flight request with
   *       `RpcError('switching_url', …)` so callers retry against the
   *       new endpoint instead of timing out on the dead socket.
   *    3. Close the current socket cleanly (code 1000).
   *    4. Update `this.url`, fire `onUrlChange`, and — if the caller
   *       was previously connected or mid-connect (i.e. hadn't called
   *       `disconnect()`) — dial the new URL. The next `onConnect`
   *       fires on the new socket as normal.
   *
   *  Intended for MiniNodePool-style failover: "this VPS just got
   *  blacklisted, switch to that one." Setting the same URL is a
   *  deliberate no-op so a pool can re-broadcast its current target
   *  without churning the socket. */
  setUrl(newUrl: string): void {
    if (newUrl === this.url) return;
    const prevUrl = this.url;

    // 1. Cancel any pending reconnect — we're about to take over the
    //    dial sequence ourselves.
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.reconnectAttempt = 0;

    // 2. Reject in-flight requests with switching_url so the upstream
    //    RatsRpcError surfaces a distinct status (not the generic
    //    'reconnecting' / 'closed') and callers know it's safe to retry.
    this.failAllPending('switching_url');

    // 3. Tear down the current socket. We flip closedByUser before the
    //    close so the OLD socket's onclose doesn't kick off the
    //    auto-reconnect loop against the now-stale URL — we issue the
    //    dial ourselves below.
    const ws = this.ws;
    const wasClosedByUser = this.closedByUser;
    const wasLive = ws !== null || this.connecting !== null;
    this.ws = null;
    this.connecting = null;
    this.closedByUser = true;
    if (ws && ws.readyState !== WebSocket.CLOSED) {
      try { ws.close(1000, 'switching url'); } catch (_) { /* swallow */ }
    }

    // 4. Update URL and notify watchers before redialing so anyone
    //    reading getUrl() from the handler sees the new value.
    this.url = newUrl;
    try { this.onUrlChange?.(prevUrl, newUrl); } catch (_) { /* swallow */ }

    // Honour the caller's prior intent: if they hadn't explicitly
    // disconnected, dial the new URL now. If they had, stay down —
    // setUrl shouldn't resurrect a deliberately-closed client.
    if (!wasClosedByUser && wasLive) {
      this.closedByUser = false;
      this.connect().catch(() => {
        // connect()'s onclose drives scheduleReconnect on failure; no
        // extra work needed here.
      });
    } else {
      this.closedByUser = wasClosedByUser;
    }
  }

  /** Open the WebSocket; resolves once the connection is OPEN, rejects
   *  if the very first connection attempt errors out before opening. */
  connect(): Promise<void> {
    this.closedByUser = false;
    if (this.connecting) return this.connecting;
    if (this.isConnected) return Promise.resolve();

    this.connecting = new Promise<void>((resolve, reject) => {
      let settled = false;
      let ws: WebSocket;
      try {
        ws = new WebSocket(this.url);
      } catch (e) {
        this.connecting = null;
        reject(e instanceof Error ? e : new Error(String(e)));
        return;
      }
      // Mini-node streams binary chunks (audio.fetch and friends) as
      // ArrayBuffer; the default `blob` mode would force every chunk
      // through an async FileReader before AudioBridge could touch the
      // bytes. ArrayBuffer is universally supported in browsers + the
      // `ws` polyfill used by the Node smokes.
      ws.binaryType = 'arraybuffer';
      this.ws = ws;

      ws.onopen = () => {
        this.reconnectAttempt = 0;
        if (!settled) {
          settled = true;
          this.connecting = null;
          resolve();
        }
        try { this.onConnect?.(); } catch (_) { /* swallow */ }
      };

      ws.onmessage = (ev: MessageEvent) => this.handleMessage(ev);

      ws.onerror = (ev: Event) => {
        try { this.onError?.(ev); } catch (_) { /* swallow */ }
        // Reject the initial connect() promise if we never opened — the
        // close handler will fire next and drive the reconnect loop.
        if (!settled) {
          settled = true;
          this.connecting = null;
          reject(new Error(`websocket error before open: ${this.url}`));
        }
      };

      ws.onclose = (ev: CloseEvent) => {
        const reason = ev.reason || `close code ${ev.code}`;
        this.ws = null;
        this.connecting = null;
        // Fail every in-flight request — they can't be served by a dead
        // socket. We rebuild on reconnect; callers must retry themselves.
        this.failAllPending(this.closedByUser ? 'closed' : 'reconnecting');
        try { this.onDisconnect?.(reason); } catch (_) { /* swallow */ }
        if (!this.closedByUser && this.autoReconnect) {
          this.scheduleReconnect();
        }
        if (!settled) {
          settled = true;
          reject(new Error(`websocket closed before open: ${reason}`));
        }
      };
    });
    return this.connecting;
  }

  /** Close the socket and cancel any pending reconnect. After this
   *  returns, all in-flight requests have been rejected with `'closed'`. */
  disconnect(): void {
    this.closedByUser = true;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const ws = this.ws;
    this.ws = null;
    this.failAllPending('closed');
    if (ws && ws.readyState !== WebSocket.CLOSED) {
      try { ws.close(1000, 'client disconnect'); } catch (_) { /* swallow */ }
    }
  }

  /** Register a listener for incoming binary WebSocket frames. The
   *  returned thunk removes the listener. Listeners run in registration
   *  order, after `onBinaryFrame` if it's also set. Throws from any one
   *  listener are routed to `onError` and don't stop the rest. */
  addBinaryListener(fn: (bytes: Uint8Array) => void): () => void {
    this.binaryListeners.push(fn);
    return () => {
      const i = this.binaryListeners.indexOf(fn);
      if (i >= 0) this.binaryListeners.splice(i, 1);
    };
  }

  /** Send a request envelope and await the matching reply.
   *  Rejects with `RpcError` on non-ok status, timeout, or socket close. */
  async request<T = unknown>(
    type: string,
    body: unknown,
    timeoutMs?: number,
  ): Promise<T> {
    if (!this.isConnected) {
      // Best-effort: try to connect first. If the caller wanted strict
      // fail-fast they can check isConnected themselves.
      try {
        await this.connect();
      } catch (e) {
        throw new RpcError(
          'send_failed',
          `websocket not connected: ${e instanceof Error ? e.message : String(e)}`,
        );
      }
    }
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      throw new RpcError('send_failed', 'websocket not open');
    }

    const reqId = newReqId();
    const envelope = { req_id: reqId, type, body };
    const timeout = timeoutMs ?? this.defaultTimeoutMs;

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        const p = this.pending.get(reqId);
        if (p) {
          this.pending.delete(reqId);
          p.reject(new RpcError('timeout', `no reply for ${type} in ${timeout}ms`));
        }
      }, timeout);
      this.pending.set(reqId, {
        resolve: resolve as (v: unknown) => void,
        reject,
        timer,
      });

      try {
        ws.send(JSON.stringify(envelope));
      } catch (e) {
        clearTimeout(timer);
        this.pending.delete(reqId);
        reject(new RpcError(
          'send_failed',
          e instanceof Error ? e.message : String(e),
        ));
      }
    });
  }

  // -- Internals -------------------------------------------------------

  private handleMessage(ev: MessageEvent): void {
    if (typeof ev.data !== 'string') {
      // Binary frame — route to AudioBridge via onBinaryFrame (ArrayBuffer)
      // and to any list-registered listeners (Uint8Array). When nothing's
      // listening we surface as an error so a misconfigured deployment
      // doesn't silently drop bytes.
      const buf =
        ev.data instanceof ArrayBuffer
          ? ev.data
          : ev.data instanceof Uint8Array
            ? ev.data.buffer.slice(
                ev.data.byteOffset,
                ev.data.byteOffset + ev.data.byteLength,
              ) as ArrayBuffer
            : null;
      if (!buf) {
        try { this.onError?.(new Error('binary frame in unsupported form')); }
        catch (_) { /* swallow */ }
        return;
      }
      const hasField = this.onBinaryFrame !== null;
      const hasListeners = this.binaryListeners.length > 0;
      if (!hasField && !hasListeners) {
        try { this.onError?.(new Error('unexpected binary websocket frame')); }
        catch (_) { /* swallow */ }
        return;
      }
      if (this.onBinaryFrame) {
        try { this.onBinaryFrame(buf); }
        catch (e) {
          try { this.onError?.(e instanceof Error ? e : new Error(String(e))); }
          catch (_) { /* swallow */ }
        }
      }
      if (hasListeners) {
        // Snapshot the list so a listener that calls `addBinaryListener`
        // or its returned remover doesn't mutate the array we're iterating.
        const snapshot = this.binaryListeners.slice();
        const view = new Uint8Array(buf);
        for (const fn of snapshot) {
          try { fn(view); }
          catch (e) {
            try { this.onError?.(e instanceof Error ? e : new Error(String(e))); }
            catch (_) { /* swallow */ }
          }
        }
      }
      return;
    }
    let env: { req_id?: string; type?: string; status?: string; body?: unknown; error?: string };
    try {
      env = JSON.parse(ev.data) as typeof env;
    } catch (e) {
      try { this.onError?.(e instanceof Error ? e : new Error(String(e))); }
      catch (_) { /* swallow */ }
      return;
    }
    const reqId = env.req_id ?? '';
    const pending = reqId ? this.pending.get(reqId) : undefined;
    if (pending) {
      this.pending.delete(reqId);
      clearTimeout(pending.timer);
      const status = env.status ?? '';
      if (status === 'ok') {
        // `body` may be absent for verbs that don't return data; surface
        // an empty object rather than undefined so callers can destructure.
        pending.resolve(env.body ?? {});
      } else {
        pending.reject(new RpcError(
          status || 'error',
          env.error || `non-ok status: ${status || '(missing)'}`,
        ));
      }
      return;
    }

    // No matching req_id. Two cases:
    //   - Envelope has a `status` field => it's a follow-up reply for a
    //     streaming verb that already resolved (e.g. audio.fetch's
    //     `status:"complete"` after the initial `status:"ok"`). Route to
    //     onUnmatchedReply so the streaming layer can drive its state
    //     machine without race-stealing the first reply.
    //   - Envelope has a `type` field => server-pushed request/event;
    //     hand to onIncomingRequest as before.
    // If both happen to be present we prefer the streaming hook because
    // the standard server-push envelopes don't carry a status.
    if (env.status !== undefined && this.onUnmatchedReply) {
      try {
        this.onUnmatchedReply({
          req_id: reqId,
          status: env.status,
          body: env.body ?? {},
          ...(env.error !== undefined ? { error: env.error } : {}),
        });
      } catch (e) {
        try { this.onError?.(e instanceof Error ? e : new Error(String(e))); }
        catch (_) { /* swallow */ }
      }
      return;
    }
    if (this.onIncomingRequest) {
      try {
        this.onIncomingRequest({
          req_id: reqId,
          type: env.type ?? '',
          body: env.body ?? {},
        });
      } catch (e) {
        try { this.onError?.(e instanceof Error ? e : new Error(String(e))); }
        catch (_) { /* swallow */ }
      }
    }
  }

  private failAllPending(status: string): void {
    if (this.pending.size === 0) return;
    const snapshot = Array.from(this.pending.entries());
    this.pending.clear();
    for (const [, p] of snapshot) {
      clearTimeout(p.timer);
      try {
        p.reject(new RpcError(status, `request aborted: ${status}`));
      } catch (_) { /* swallow */ }
    }
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) return;
    // 1s, 2s, 4s, 8s, 16s, 30s, 30s, … (cap 30s).
    const delay = Math.min(
      RECONNECT_INITIAL_MS * Math.pow(2, this.reconnectAttempt),
      RECONNECT_CAP_MS,
    );
    this.reconnectAttempt += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (this.closedByUser) return;
      // Fire and forget — the connect promise wires up onopen/onclose
      // which drive the next retry on failure.
      this.connect().catch(() => { /* scheduleReconnect will fire from onclose */ });
    }, delay);
  }
}
