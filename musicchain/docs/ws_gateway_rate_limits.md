# WS mini-gateway per-connection rate-limit spec

Scope: `WsMiniGateway` (see `src/transport/ws_mini_gateway.h` and the
forthcoming `ws_mini_gateway.cpp`) — the unified JSON-envelope +
binary-frame WebSocket surface that browsers connect to directly on the
mini-node, default port 8082.

Companion reading:

- `tools/mini_node.cpp` — the verb dispatcher (`on_rpc_request`) that
  the gateway forwards to via `relay.forward` for anything it doesn't
  serve locally.
- `src/transport/ws_audio_bridge.cpp` — the existing audio-only bridge
  whose `MAX_FRAME_PAYLOAD` (16 MiB) and `MAX_OUTBOUND_BYTES` (16 MiB)
  ceilings are the only per-connection caps that exist today.
- `src/transport/ws_bridge.h:72` — explicit "no auth, no session, no
  rate-limit (yet)" note that this spec retires.

Date: 2026-06-21.

## 1. Audit of existing rate / throttle code

`grep -i 'rate_limit|max_per_sec|throttle|RPS|requests_per|quota|
leaky|token_bucket'` over `tools/mini_node.cpp` and the
`src/transport/` headers returns:

- `tools/mini_node.cpp` — zero matches related to per-peer limits. The
  only "drop" sites are JSON-parse failures and ring-buffer trims for
  the chat history (`kChatRingPerRoom = 1000`) and the event monitor
  (`kEventRows = 22`).
- `src/transport/ws_bridge.h:72` — comment "no auth, no session, no
  rate-limit (yet)" — confirms intent gap.
- `src/transport/ws_audio_bridge.cpp:167` — `MAX_FRAME_PAYLOAD = 16 *
  1024 * 1024` and `:179` `MAX_OUTBOUND_BYTES = 16 * 1024 * 1024`. These
  are per-frame and per-outbox caps, NOT rate limits — a hostile
  browser can still hammer 16 MiB frames as fast as the I/O loop will
  accept them.
- `tools/mini_node.cpp:1638` — `rats_set_max_peers(client, 4096)`.
  Global ceiling on librats-side peers; does not apply to the browser
  WS surface (those connections never become librats peers).
- `LoadMonitor` (`src/net/load_monitor.h`) — `max_bandwidth_bps`
  influences the published `load_score` so players migrate elsewhere
  under load, but it does not gate, drop, or queue a single inbound
  request.

Conclusion: **no per-connection rate limiting exists**. The gateway is
about to expose every verb the mini-node serves — plus, via
`relay.forward`, every verb every full node serves — to anonymous
browser sessions over a plain WebSocket. This document sets the caps
to land before that surface ships.

## 2. Threat model

The gateway is reachable from any browser on the public internet with
no auth, no captcha, no session token. Realistic abuse:

- A browser tab (or a botnet of them) bursts `routes.get` /
  `mininodes.list` thousands of times per second to drive the mini-node
  CPU up and degrade `load_score` so other VPSes pick up the slack
  (free DoS against the cheapest tier).
- A browser opens many parallel `audio.fetch` streams against
  different content hashes to saturate the mini-node's upstream
  bandwidth and starve real players.
- A browser sends `relay.forward` flood at a single full node it picked
  out of `routes.get`, abusing the mini-node as a free reflector to DoS
  the home node.
- A browser sends a single 16 MiB JSON frame per millisecond — within
  the current `MAX_FRAME_PAYLOAD` cap — to exhaust the mini-node's
  socket-buffer + heap.

Anonymity means we cannot ban by identity. The only stable handle is
the upgraded WS connection itself (the `WsMiniConnId`). Per-IP caps
are a secondary defense (NAT shares one IP across many real users, so
per-IP caps must be looser than per-connection caps).

## 3. Headline caps (per WebSocket connection)

The four caps below are the contract. Defaults are conservative; the
gateway should expose each as a constant in `ws_mini_gateway.cpp` and
NOT plumb them as CLI flags until we see real traffic.

| Cap | Default | Reset window | Action on overage |
| --- | --- | --- | --- |
| **RPC verbs per second** | **50 / sec** | 1 s sliding window | Reply `{"status":"rate_limited","retry_after_ms":<remaining>}`; do NOT close the connection on the first hit |
| **In-flight `audio.fetch` streams** | **1 concurrent** | clears on `complete` or stream close | Reply `{"status":"too_many_streams","limit":1}` to the second stream.open before forwarding it |
| **Aggregate outbound bandwidth** | **10 MB/s** (= 10 * 1024 * 1024 B/s, leaky bucket, 20 MB burst) | continuous | Pause writes; if outbox > `MAX_OUTBOUND_BYTES` (already 16 MiB), close per existing logic |
| **Aggregate inbound bytes** | **2 MB/s** (leaky bucket, 4 MB burst) | continuous | Stop reading from the socket; if peer keeps writing past TCP backpressure, close after 5 s |

Implicit fifth cap: **`MAX_FRAME_PAYLOAD = 256 KiB`** for the gateway
specifically (tighter than the audio bridge's 16 MiB — the gateway's
JSON envelopes are < 1 KiB in practice; only `audio.fetch` needs the
big frames, and audio chunks flow OUTBOUND, never inbound). Inbound
binary frames are already rejected per the gateway's protocol spec.

### Why these numbers

- **50 RPCs/sec/connection** — a real browser-driven player UI calls
  `routes.get` once per refresh, `mininodes.list` once at startup,
  `player.locate` in batches of < 20 when joining a swarm, and
  `chat.history` on tab switches. Steady-state is < 5 RPC/sec. 50/sec
  leaves 10x headroom for a noisy app and still pins CPU per
  connection at a low single-digit percentage.
- **1 in-flight `audio.fetch`** — humans listen to one song at a time.
  Pre-fetching the next track is fine BETWEEN streams (open stream A,
  complete, open stream B). Letting one connection open 20 parallel
  fetches lets it monopolize the mini-node's bandwidth budget and
  pin many full-node sockets via stream.open. If a UI needs
  concurrent prefetch, it can open a second WebSocket — which will
  itself get its own 10 MB/s and its own 50 RPC/sec, but the per-IP
  cap (below) bounds the total.
- **10 MB/s outbound** — at ~256 kbps CBR audio that's roughly 320x
  realtime, enough that even an aggressively scrubbing user
  refetching chunks sees no throttle for normal listening. Lossless
  FLAC at ~1 Mbps still gets 80x realtime headroom. A 10 GB/month VPS
  with 8 connections in steady-state at this cap would saturate egress
  in ~2 minutes, so the per-IP cap below has to bite first.
- **2 MB/s inbound** — JSON envelopes are tiny; the only thing on the
  inbound path is RPC bodies. 2 MB/s is enough to push ~30 KB chat
  messages at the 50 RPC/sec ceiling and still fits an envelope
  carrying a base64-encoded thumbnail.

## 4. Per-IP umbrella (secondary)

Browser tabs can multiply connections trivially, so per-connection
caps are not enough. Add these umbrella caps keyed on the WS upgrade's
source IP (the `:port` is stripped — many tabs share one client port
range over time):

| Cap | Default | Action on overage |
| --- | --- | --- |
| **Concurrent WS connections per IP** | **8** | Reject the 9th upgrade with HTTP 429 before completing the handshake |
| **WS upgrades per IP per minute** | **30** | Reject with HTTP 429; log to the event ring as `ip-burst` |
| **Aggregate egress per IP** | **30 MB/s** (3x the per-connection cap) | Throttle all connections from that IP proportionally |

8 concurrents is generous for a single user (one for the player UI,
one for chat, one for a background pre-fetcher, room to spare for
window reload races) while preventing a single CGNAT'd attacker from
running 200 connections to wave around the per-connection caps.

The umbrella caps tolerate carrier-NAT: ~hundreds of real users
sharing one IP would each get a fair share via the umbrella's
proportional throttle, instead of hitting the hard concurrent-conn
ceiling. If we see CGNAT issues in production, raise the per-IP
concurrent cap to 16-32 and rely on the per-connection caps to keep
each session bounded.

## 5. Special handling per verb

Within the 50 RPC/sec/connection envelope, some verbs cost more than
others. The gateway should treat each RPC verb as costing N tokens out
of a 50-token-per-second bucket:

| Verb | Token cost | Rationale |
| --- | --- | --- |
| `routes.get`, `mininodes.list`, `mini.hello`, `status` | 1 | Cheap reads from in-memory maps |
| `stun.observe`, `player.locate` | 1 | Cheap read with no peer round-trip |
| `player.announce` | 2 | Mutates `g_players` and triggers a `swarm.peer_online` broadcast on first-seen |
| `chat.history`, `chat.list_rooms` | 1 | Cheap read; already paginated by `limit` ≤ 200 |
| `chat.publish` (when added) | 5 | Hits gossipsub, fan-out cost is multiplicative |
| `relay.forward`, `relay.push.forward` | 10 | Uses up a slot in `g_pending_relays` and consumes a full-node's serving budget |
| `ice.connect_request` | 5 | Asks a full node to spend a hole-punch attempt |
| `audio.fetch` (stream.open phase only) | 10 | Gated separately by the in-flight cap; the 10-token RPC cost limits stream-churn |
| Anything else (falls through to `relay.forward`) | 10 | Treat the unknown as a relay |

A connection burning routes.get at 50/sec is bounded to 1 routes.get
per 20 ms. A connection burning relay.forward at the same wall-clock
rate gets bounded to 5/sec — which is plenty for any legitimate UX
while making the mini-node a useless reflector.

## 6. Error envelope shape

All rate-limit rejections share one envelope so the browser client can
implement one retry path:

```json
{
  "req_id": "<echoes caller>",
  "status": "rate_limited",
  "error": "<verb> exceeded <cap>",
  "retry_after_ms": <int>,
  "limit": {
    "kind": "rpc_per_sec | inflight_audio | egress_bps | ingress_bps | ip_concurrent | ip_upgrades_per_min",
    "value": <number>
  }
}
```

`retry_after_ms` is the bucket's projection of when the next token
will be available, NOT a fixed wall-clock delay. The browser should
backoff with jitter — but since this is anonymous traffic, we don't
need to be polite about it; the server-side enforcement is what
actually protects the node.

## 7. Connection-close policy

- Soft offences (single overage): reply `rate_limited`, keep the
  connection.
- Repeated offences (10 consecutive `rate_limited` replies within 5
  seconds, OR a single overage that doubles the cap): close the
  connection with WS close code 1008 (policy violation), reason
  `rate_limit_repeated`. Browser libraries surface this distinctly
  from a network drop.
- Ingress-bytes overage past the 5-second grace: close with 1008
  reason `ingress_overflow`.
- Outbox grows past existing `MAX_OUTBOUND_BYTES` (16 MiB): close per
  current ws_audio_bridge behavior — no change.

We do NOT IP-ban beyond the 30-uploads-per-minute upgrade cap. Banning
anonymous browser IPs hurts shared NAT users and is trivially
circumvented; the per-connection caps are the real enforcement.

## 8. Observability

Push events to the existing `g_events` ring (kind = `rl-drop`,
`rl-close`, `ip-burst`) so the TUI monitor shows hot-spot
connections. Add four atomic counters surfaced on the existing
`status` RPC body:

- `rate_limited_replies_total`
- `connections_closed_for_rate_limit_total`
- `ip_upgrade_rejections_total`
- `audio_fetch_too_many_streams_total`

These let us see whether the caps are biting and tune in a follow-up
PR.

## 9. Non-goals

- No global QPS cap. The mini-node already advertises load via
  `LoadMonitor`; if true load gets high, players migrate to a less
  loaded VPS. Per-connection caps + per-IP umbrella are sufficient.
- No auth-aware tier ("paid users get 200 RPCs/sec"). The gateway is
  defined as anonymous; signed accounts come back through a
  separate authenticated WS surface later, if ever.
- No body-content inspection beyond the existing JSON parse. Anti-spam
  for chat is handled inside the chat module on top of the RPC budget.
- No backoff coordination across mini-nodes in the mesh. A botnet that
  rotates across VPSes gets the per-connection cap at each one
  independently — that's intentional, load distribution is the whole
  point of the multi-VPS topology.
