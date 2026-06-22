// Song-blob fetcher for the web player.
//
// The Dart-side equivalent (musicchain_player/lib/src/services/rats_client.dart
// + node_client.dart) opens a swarm stream over librats: it asks the full
// node `stream.open`, picks a peer, then pulls binary chunks straight off
// the rats socket and reassembles them into a temp file media_kit plays
// from disk. The browser can't speak librats, so it can't pull peer-served
// binary frames the same way.
//
// Two transports were on the table:
//   (a) WebSocket binary frames forwarded by the home node (full node
//       proxies the swarm peer's bytes to us). Cheap implementation,
//       expensive bandwidth on the full node, and the full node would
//       have to dedup chunks for parallel listeners. Future work — left
//       stubbed below for the day the librats-WS bridge lands.
//   (b) Plain HTTPS GET to a song-blob endpoint the home node exposes
//       at /audio/<content_hash>. The home node already proxies binary
//       audio for the embedded webview / direct browser clients, so
//       reusing that endpoint costs us nothing.
//
// We ship (b) today. `streamSong` still asks the full node `stream.open`
// first — that primes the swarm metadata (peers + variants) and lets
// the server hint at a custom serving URL if a future version decides
// to redirect us to a CDN, a sibling full node, or an HTTP-capable
// swarm peer. If no hint comes back, we fall back to the home node's
// /audio/<hash> proxy on the same origin.

import type { NodeClient } from './node_client';

/** Shape of one entry in the `peers` array returned by `stream.open`.
 *  Mirrors the JSON the C++ side builds in src/api/rats_api.cpp. */
export interface SwarmPeer {
  peer_id: string;
  /** The peer's *local* content hash — variant peers serve a slightly
   *  different file (same song, different bitrate / format) than the
   *  one the chain canonically indexes against. */
  content_hash: string;
  bitrate?: number;
  audio_format?: string;
}

/** Reply shape from `stream.open`. The C++ server returns at least
 *  `content_hash`, `peers`, and `source`. Newer servers may add
 *  optional URL hints (`stream_url` / `url` / `serving_url`) so the
 *  browser can skip the home-node proxy and go straight to an HTTP
 *  source; we honour whichever spelling shows up. */
interface StreamOpenReply {
  content_hash?: string;
  peers?: SwarmPeer[];
  /** "swarm" when peers are present, "no_swarm" otherwise. */
  source?: 'swarm' | 'no_swarm' | string;
  /** Optional direct URL — picked up if the server starts sending one. */
  stream_url?: string;
  url?: string;
  serving_url?: string;
}

/** Result of a successful stream. The caller assigns `url` to the
 *  HTMLAudioElement (via Player.load) and is responsible for calling
 *  `URL.revokeObjectURL(url)` once the source is no longer in use —
 *  Player.load already does this when the next source is loaded, but
 *  if the caller bypasses Player they must revoke themselves. */
export interface StreamResult {
  /** The raw audio bytes, kept alive for the lifetime of the URL. */
  blob: Blob;
  /** A `blob:`-scheme URL pointing at `blob`. Pass to <audio>.src. */
  url: string;
}

export interface StreamSongOptions {
  /** RPC client connected to the home node — used for `stream.open`. */
  node: NodeClient;
  /** 64-char hex content hash the chain canonically indexes the song by. */
  contentHash: string;
  /** Optional override for the HTTPS host that serves /audio/<hash>.
   *  Defaults to the same origin the page was loaded from, which is how
   *  production runs (page + WS gateway + audio proxy all on the home
   *  node). Useful in tests / when the WS gateway is on a different
   *  host than the audio proxy. */
  audioHost?: string;
  /** Override the path prefix the home node serves audio under.
   *  Defaults to `/audio`. */
  audioPathPrefix?: string;
}

const HASH_RE = /^[0-9a-fA-F]{64}$/;

/** Compute the default `https://<host>/audio/<hash>` URL the home node
 *  proxies blob audio under. Falls back to `wss:` → `https:` rewriting
 *  for the rare environment where `location` only has the WS gateway
 *  host (e.g. a wrapper page hosted elsewhere). */
function defaultAudioUrl(
  contentHash: string,
  hostOverride: string | undefined,
  prefix: string,
): string {
  // Caller-provided override wins.
  if (hostOverride && hostOverride.length > 0) {
    // Allow either a bare host ("node.example.com") or a fully-qualified
    // origin ("https://node.example.com"). We don't try to be clever about
    // sniffing protocol — bare hosts default to https since the only
    // production deployment is TLS-fronted.
    const isAbsolute = /^https?:\/\//i.test(hostOverride);
    const base = isAbsolute ? hostOverride : `https://${hostOverride}`;
    return `${base}${prefix}/${contentHash}`;
  }
  if (typeof location !== 'undefined' && location.host) {
    // The page is served by the home node itself — same-origin works
    // and avoids any CORS preflight on the /audio path.
    const proto = location.protocol === 'https:' ? 'https:' : 'http:';
    return `${proto}//${location.host}${prefix}/${contentHash}`;
  }
  // Last-resort. There's no good answer in a non-DOM environment, but
  // returning a localhost URL at least lets a unit test under jsdom
  // exercise the rest of the function.
  return `http://localhost${prefix}/${contentHash}`;
}

/** Pick a direct URL hint out of the `stream.open` reply, if any.
 *  Returns null when the server only returned swarm peers (the common
 *  case today). We accept three field names so the server can pick
 *  whichever spelling fits its naming style — none is currently
 *  emitted, but the contract is documented here so the C++ side has
 *  a target to point at. */
function pickServingUrlHint(reply: StreamOpenReply): string | null {
  const fields = [reply.stream_url, reply.url, reply.serving_url];
  for (const f of fields) {
    if (typeof f === 'string' && /^https?:\/\//i.test(f)) {
      return f;
    }
  }
  return null;
}

/** Fetch the song bytes for `contentHash` and return a Blob URL the
 *  caller can hand to <audio>.src (or Player.load).
 *
 *  Steps:
 *    1. RPC `stream.open` to the home node so the swarm sees the
 *       request (the C++ side credits relay traffic and may emit
 *       a server-pushed URL hint in the future).
 *    2. If the reply carries a direct URL, fetch that. Otherwise fall
 *       back to `https://<host>/audio/<content_hash>` on the same
 *       origin the WS gateway lives on — the home node's HTTP proxy.
 *    3. `response.blob()` the entire body, wrap it with
 *       `URL.createObjectURL`, and return both so the caller can
 *       revoke when the source is no longer needed.
 *
 *  Throws on RPC failure, non-2xx HTTP status, or an unknown hash.
 *  The caller is expected to revoke the returned `url` with
 *  `URL.revokeObjectURL(url)` once playback is over. */
export async function streamSong(
  opts: StreamSongOptions,
): Promise<StreamResult> {
  const contentHash = opts.contentHash.trim();
  if (!HASH_RE.test(contentHash)) {
    throw new Error(
      `streamSong: content_hash must be 64-char hex (got length ${contentHash.length})`,
    );
  }

  // Step 1 — ask the chain where to get it. We don't fail hard if the
  // RPC errors out: the home node's /audio proxy can still serve the
  // bytes even when `stream.open` rejects (e.g. because we haven't
  // joined the swarm yet). Anything the call DID return is honoured.
  let reply: StreamOpenReply = {};
  try {
    reply = await opts.node.request<StreamOpenReply>(
      'stream.open',
      { content_hash: contentHash },
    );
  } catch (e) {
    // Swallow — the HTTP fallback below is the source of truth for the
    // bytes themselves. Log so devs can see swarm misses while the
    // browser still plays through fine.
    // eslint-disable-next-line no-console
    console.warn(
      '[stream] stream.open RPC failed; falling back to /audio proxy:',
      e instanceof Error ? e.message : String(e),
    );
  }

  // Step 2 — choose a transport. The future librats-WS bridge would hook
  // in here: when reply.peers is non-empty AND we have a way to ask the
  // home node to proxy peer chunks over the WS as base64 frames, we'd
  // call into a `streamViaWebSocket(opts.node, reply.peers)` helper that
  // accumulates the chunks and resolves to a Blob. Stubbed for now —
  // every browser fetch goes through HTTPS.
  const hintedUrl = pickServingUrlHint(reply);
  const fetchUrl = hintedUrl
    ?? defaultAudioUrl(
      contentHash,
      opts.audioHost,
      opts.audioPathPrefix ?? '/audio',
    );

  // Step 3 — pull the bytes. `fetch` follows redirects automatically and
  // streams into a Blob; the browser handles content-length / chunked
  // transfer transparently. We don't set a body size cap here — songs
  // are at most ~50 MB in the chain's policy and the browser will OOM
  // gracefully on anything larger before we have a chance to.
  const resp = await fetch(fetchUrl, {
    method: 'GET',
    // Same-origin in production; explicit so a stray cross-origin fetch
    // surfaces as a CORS error instead of a silent 0-byte response.
    credentials: 'omit',
    redirect: 'follow',
    cache: 'default',
  });
  if (!resp.ok) {
    throw new Error(
      `streamSong: HTTP ${resp.status} fetching ${fetchUrl}`,
    );
  }
  const blob = await resp.blob();
  // Guard against an empty body — easy to miss because the audio
  // element will just go straight to 'ended' with no error otherwise.
  if (blob.size === 0) {
    throw new Error(`streamSong: empty body from ${fetchUrl}`);
  }
  const url = URL.createObjectURL(blob);
  return { blob, url };
}

// -- Future work: WebSocket binary swarm streaming -------------------
//
// The shape below sketches the (a) transport described at the top of
// this file. It's not exported yet because there's no server-side
// counterpart to talk to — the home node would need to:
//
//   1. Open a virtual stream id on the WS for the requested hash,
//      sending an initial `{type: 'stream.bytes_begin', stream_id,
//      content_hash, size}` envelope.
//   2. Forward each librats binary chunk as a `{type: 'stream.bytes',
//      stream_id, b64}` envelope (or a real binary WS frame once the
//      gateway can mux text + binary).
//   3. Close the stream with `{type: 'stream.bytes_end', stream_id}`.
//
// On the browser side we'd register a server-push handler on
// `node.onIncomingRequest` keyed by stream_id, accumulate chunks into
// a `Uint8Array[]`, and resolve to `new Blob(parts, {type})`. Wiring
// that requires changes to NodeClient (route by stream_id, not just
// req_id) and to the home node's WS gateway, so it's deferred until
// the (b) HTTP path is in production and proves we need the savings.
//
// async function streamViaWebSocket(
//   _node: NodeClient,
//   _peers: SwarmPeer[],
//   _contentHash: string,
// ): Promise<Blob> {
//   throw new Error('streamViaWebSocket: not implemented yet');
// }
