# Mini-node WebSocket gateway — wire protocol

Normative spec for the mini-node's browser-facing WebSocket surface on
TCP/8082. This is the single endpoint a web player (or any non-librats
client) needs to drive routes, mini-node discovery, STUN-equivalent
reflection, audio fetch, and arbitrary chain RPCs.

Status: NORMATIVE for protocol version `0x01`. The implementation lives
in `src/transport/ws_audio_bridge.cpp` plus the mini-node verb
dispatcher in `tools/mini_node.cpp`. Where this doc and the source
disagree, the doc wins for new clients; existing clients are expected
to interoperate during transition (see §6 Versioning).

------------------------------------------------------------------------

## 1. Connection

- URL: `wss://<vps>:8082/`
  - Path is always `/`. Query string is ignored.
  - `wss://` (TLS) is the canonical scheme. Plain `ws://` is accepted on
    the same port for development / loopback bring-up but MUST NOT be
    used in production.
- Subprotocol negotiation:
  - The client MAY send any `Sec-WebSocket-Protocol` value, including
    none.
  - The server echoes the first offered subprotocol verbatim into its
    101 response, or omits the header if the client sent none. The
    server does not interpret the value. Future protocol selection MUST
    use the version byte (§6), not the subprotocol field.
- Authentication: **none at the WebSocket layer**. The gateway accepts
  any TCP client that completes the WebSocket handshake. Authority for
  every action lives in the chain-signed body of the verb (founder
  signatures, KYC tokens, payment receipts, etc.). Mini-nodes do not
  hold or verify long-lived bearer credentials.
- Origin policy: the server does not enforce `Origin`. Browsers see CORS
  through the WebSocket handshake's own rules; non-browser clients are
  unrestricted.
- Idle timeout: the server sends WebSocket PINGs every 30 s and closes
  the connection if no PONG arrives within 30 s of the PING. Clients
  SHOULD reply to PINGs and MAY send their own.

------------------------------------------------------------------------

## 2. Frame types

The gateway uses both TEXT and BINARY frames on the same connection.
Control frames (PING/PONG/CLOSE) follow RFC 6455 and are not redefined
here.

### 2.1 TEXT — JSON envelopes

All TEXT frames are UTF-8 JSON objects. There are exactly two envelope
shapes.

**Request (client → server, or server → client for server-initiated
verbs like `stream.close`):**

```json
{
  "req_id": "<opaque string, unique per origin>",
  "type":   "<verb>",
  "body":   { ... verb-specific ... }
}
```

- `req_id` MUST be present and unique within the lifetime of one
  connection. It is the only join key the peer uses for replies. UUIDs,
  monotonic counters as strings, and content-addressed keys are all
  fine.
- `type` is the verb name (see §3).
- `body` MAY be omitted when the verb takes no arguments; it then
  defaults to `{}`.

**Reply (server → client, or client → server for relayed forwards):**

```json
{
  "req_id": "<same id as the request>",
  "status": "ok" | "complete" | "<short error token>",
  "body":   { ... },          // present on success / progress
  "error":  "<human readable>" // present on failure
}
```

- Exactly one of `body` or `error` is present, never both.
- `status` is `"ok"` for normal success, `"complete"` for the final
  envelope of a streamed reply (today only `audio.fetch`), or a
  lowercase snake_case token on failure (e.g. `bad_request`,
  `not_matched`, `open_timeout`, `peer_unreachable`, `internal`).
- Unknown reply fields MUST be ignored by clients.

Both sides MUST tolerate envelopes that arrive split across multiple
WebSocket continuation frames. Implementations re-assemble per RFC 6455
before parsing.

### 2.2 BINARY — audio chunks

BINARY frames carry raw audio bytes for an in-flight `audio.fetch`
stream. Layout:

```
+--------+--------+--------+--------+----------------------------+
|  stream_id (uint32, big-endian)   |  raw audio payload  ...    |
+--------+--------+--------+--------+----------------------------+
   byte 0   byte 1   byte 2   byte 3   bytes 4 .. N-1
```

- `stream_id` is the 32-bit value the server returned in the `body` of
  the `audio.fetch` reply (`{ "stream_id": <n>, ... }`).
- Big-endian byte order. (Network byte order. The wire format is BE
  even though some internal librats hops use LE for the same field;
  the gateway converts on the way out.)
- The payload bytes are the codec-native audio bytes in delivery
  order. There is no per-chunk header, no sequence number, and no EOF
  marker inside the BINARY frame. The end of stream is signalled by a
  TEXT envelope with `status: "complete"` (see `audio.fetch` in §3).
- BINARY frames MUST NOT be sent by the client. The client→server
  direction is TEXT only.

Clients route an incoming BINARY frame by reading the first four bytes,
decoding them as a big-endian uint32, looking up the matching pending
`audio.fetch`, and appending the remaining bytes to that stream's
buffer.

------------------------------------------------------------------------

## 3. Verb catalogue

Verbs answered locally by the mini-node:

| Verb              | Direction      | Streamed | Notes                                |
|-------------------|----------------|----------|--------------------------------------|
| `mini.hello`      | client → mini  | no       | handshake, returns peer id + version |
| `routes.get`      | client → mini  | no       | full route mesh                      |
| `mininodes.list`  | client → mini  | no       | mini-node mesh                       |
| `stun.observe`    | client → mini  | no       | observed `ip:port` of the caller     |
| `audio.fetch`     | client → mini  | yes      | bridge to a swarm peer for audio     |
| `stream.close`    | mini → client  | no       | server-initiated cancel notice       |
| *(anything else)* | client → mini  | varies   | forwarded via `relay.forward`        |

### 3.1 `mini.hello`

First request a client SHOULD send. Serves as the version handshake
(§6).

Request body:

```json
{
  "version":      1,
  "client_id":    "<opaque, optional>",
  "capabilities": ["audio.fetch", "..."]   // optional
}
```

Reply body:

```json
{
  "version":         1,
  "peer_id":         "<40-hex mini-node peer id>",
  "peer_address":    "<observed ip:port of the caller>",
  "route_count":     <n>,
  "supported_verbs": ["routes.get", "mininodes.list", "stun.observe",
                      "audio.fetch", "stream.close"],
  "max_audio_streams": <n>
}
```

If the server cannot satisfy the requested `version`, it responds with
`status: "unsupported_version"` and an `error` string carrying the
highest version it does support, then closes the connection with code
1002.

### 3.2 `routes.get`

Returns the route mesh assembled from full-node route announcements
this mini-node has ingested. Answered locally; never forwarded.

Request body: empty (`{}`).

Reply body:

```json
{
  "routes": [
    {
      "peer_id":        "<40-hex full-node id>",
      "public_address": "<ip:port>",
      "load_score":     <float, 0..1, lower is fresher capacity>,
      "kind":           "full",
      "last_seen_ms":   <epoch ms>
    }
  ],
  "mininodes": [
    {
      "peer_id":        "<40-hex mini-node id>",
      "public_address": "<ip:port>",
      "load_score":     <float>,
      "kind":           "mini",
      "last_seen_ms":   <epoch ms>
    }
  ]
}
```

`load_score` is opaque to the wire format. Clients use it for soft
preference only; it is not a quota.

### 3.3 `mininodes.list`

Returns the mini-node mesh as the mini-node has assembled it from
peer-to-peer `mini.hello` exchanges. Answered locally; never forwarded.

Request body: empty.

Reply body:

```json
{
  "mininodes": [
    {
      "peer_id":        "<40-hex>",
      "public_address": "<ip:port>",
      "load_score":     <float>,
      "last_seen_ms":   <epoch ms>
    }
  ]
}
```

Functionally equivalent to the `mininodes` field of `routes.get`,
exposed separately so clients that only care about VPS reachability can
poll cheaply.

### 3.4 `stun.observe`

Application-layer STUN equivalent. Answered locally from the TCP socket
the WebSocket arrived on; no RFC 5389 framing involved.

Request body: empty.

Reply body:

```json
{
  "observed_address": "<ip:port>"
}
```

`ip` is the peer side of the TCP socket as seen by the OS, `port` is
the source port. Because this surface is TCP, the observed port is only
useful for TCP NAT inference; UDP/QUIC clients SHOULD use the
librats-side `stun.observe` instead.

### 3.5 `audio.fetch`

The single streamed verb. Pulls audio bytes off a swarm peer (phone,
home node, another mini-node) and pumps them back as BINARY frames,
terminated by a `complete` envelope.

Request body:

```json
{
  "content_hash": "<64-hex SHA-256>",
  "peer_id":      "<40-hex swarm peer id, optional>"
}
```

- `content_hash` is mandatory; the mini-node validates length and
  charset before doing anything else.
- `peer_id` is optional. When present, the mini-node bridges directly
  to that peer. When absent, the mini-node consults its route mesh and
  picks a candidate; clients that want deterministic peer selection
  MUST supply `peer_id`.

Reply sequence:

1. TEXT envelope, status `ok`:

   ```json
   {
     "req_id": "...",
     "status": "ok",
     "body":   { "stream_id": <uint32>, "total_bytes": <n> }
   }
   ```

   The `stream_id` is the value to expect in the leading 4 bytes of
   every BINARY frame for this stream. `total_bytes` MAY be 0 if the
   upstream peer did not pre-announce a size; clients SHOULD then size
   their buffer dynamically.

2. Zero or more BINARY frames, each formatted per §2.2, in delivery
   order. The mini-node guarantees per-stream order; it does not
   interleave chunks from different streams within one frame.

3. Exactly one terminal TEXT envelope:

   ```json
   { "req_id": "...", "status": "complete", "body": { "stream_id": <n>, "bytes": <n> } }
   ```

   `bytes` is the actual number of payload bytes delivered (sum of all
   BINARY payloads excluding the 4-byte stream_id prefix).

Error tokens for the final TEXT envelope: `bad_request`, `not_matched`,
`open_timeout`, `peer_unreachable`, `peer_reset`, `cancelled`,
`internal`. On error there is no `complete` envelope; the error
envelope IS the terminator.

A client MAY have multiple `audio.fetch` requests in flight on the
same WebSocket; the mini-node MUST allocate distinct `stream_id`s and
demultiplex correctly. The mini-node MAY cap concurrency; it advertises
the cap as `max_audio_streams` in the `mini.hello` reply.

### 3.6 `stream.close` (server-initiated)

Sent by the mini-node TO the client (the only server-initiated verb in
this protocol) when an in-flight audio stream is being torn down for
reasons other than normal completion — for example, when the client's
WebSocket has closed and the mini-node is propagating the cancel to the
upstream peer (see §4). Listed here for completeness even though most
clients will never receive one on their own connection (they're already
closing).

Envelope:

```json
{
  "req_id": "<server-generated>",
  "type":   "stream.close",
  "body":   { "stream_id": <uint32> }
}
```

`stream.close` does not expect a reply.

### 3.7 Catch-all — `*` (anything else)

Any `type` not listed above is treated as an opaque chain RPC and
forwarded to a full node via librats `relay.forward`. The mini-node:

1. Picks a full node from its route mesh (lowest `load_score` wins,
   ties broken arbitrarily).
2. Wraps the envelope as the inner payload of a `relay.forward`
   request, preserving the original `req_id`.
3. Awaits the full node's reply over the same librats relay path.
4. Unwraps the reply envelope and writes it back to the client
   verbatim, preserving `req_id`, `status`, and `body`/`error`.

If no suitable full node is currently in the mesh, the mini-node
replies with status `no_route` and `error: "no full node available"`.

If the full node replies but with status indicating it does not know
the verb, that status flows through to the client unchanged. The
mini-node does not interpret unknown verbs and does not maintain an
allow-list; chain-level signatures in the body are the authority.

------------------------------------------------------------------------

## 4. Disconnect handling

When the WebSocket connection closes (clean close, transport reset,
PING timeout, or server-side stop), the mini-node MUST, for every
`audio.fetch` stream still registered against that connection:

1. Remove the stream from the `stream_id → connection` registry so
   late-arriving BINARY chunks are dropped, not queued.
2. Send a `stream.close` request envelope to the upstream swarm peer
   over librats:

   ```json
   {
     "req_id": "<server-generated>",
     "type":   "stream.close",
     "body":   { "stream_id": <uint32> }
   }
   ```

   This is fire-and-forget; the mini-node does not wait for a reply
   and does not retry. If the librats send fails (peer already gone)
   the mini-node logs and moves on.

3. Forget any pending `audio.fetch` reply awaiting demux against that
   stream.

Catch-all (`relay.forward`) requests with no reply yet are NOT
cancelled by the disconnect; the mini-node simply discards the reply
when it eventually arrives, because the `req_id` no longer maps to a
live WS connection. Full nodes are expected to be idempotent enough
that an orphaned reply does no harm.

The mini-node MUST NOT hold the WebSocket close pending upstream
acknowledgement. Close completes locally; cleanup happens in the
background.

------------------------------------------------------------------------

## 5. Errors and status tokens

| Token                | Layer            | Meaning                                            |
|----------------------|------------------|----------------------------------------------------|
| `ok`                 | success          | normal reply                                       |
| `complete`           | streamed success | terminator for a streamed verb (`audio.fetch`)     |
| `bad_request`        | envelope         | missing/invalid `req_id`, `type`, or body fields   |
| `unsupported_version`| handshake        | client requested a protocol version we don't speak |
| `not_matched`        | audio            | no in-flight stream matched the inbound chunk      |
| `open_timeout`       | audio            | upstream peer didn't reply within the open window  |
| `peer_unreachable`   | audio / relay    | named peer is not connected on librats             |
| `peer_reset`         | audio            | upstream peer dropped mid-stream                   |
| `cancelled`          | audio            | client closed before stream completed              |
| `no_route`           | catch-all        | no full node in the route mesh for forwarding      |
| `internal`           | any              | server-side bug; details in `error`                |

The `error` string is human-readable and stable enough to log, but
clients MUST switch on `status` for behaviour, not on `error`.

------------------------------------------------------------------------

## 6. Versioning

The first envelope a client sends on a fresh connection SHOULD be
`mini.hello` (§3.1), carrying an explicit `version` field. To make the
on-wire version unambiguous even in adversarial or proxy-rewritten
JSON, the byte layout is:

- The `body.version` field of `mini.hello` is a single unsigned byte
  (0..255), encoded as a JSON number. This is the **protocol version
  byte**. Current value: `1` (`0x01`).
- The mini-node echoes the version byte it agreed to in its
  `mini.hello` reply `body.version`. Clients MUST trust the server's
  value for the rest of the connection.
- Version `0` is reserved as "unspecified" and is treated by the server
  as version `1` for backward compatibility with clients written
  before this doc landed; future minors MUST NOT use it.
- A client that omits `mini.hello` entirely is presumed to be speaking
  version `1`. Future protocol versions MUST require `mini.hello` and
  refuse silent fallback.

Bumping the version byte indicates a breaking change to any of:

- envelope shape (§2.1),
- BINARY frame layout (§2.2),
- locally-answered verb semantics (§3.1–3.6),
- disconnect handling (§4).

Adding a new verb, adding a new status token, or adding an optional
field is **not** a breaking change and does not bump the version byte.
Clients MUST ignore unknown fields and unknown status tokens (treating
the latter as a generic failure).

When a server receives an envelope whose `mini.hello` advertised a
version it cannot speak, it MUST reply `status: "unsupported_version"`
with `body: { "max_supported": <n> }`, then close the WebSocket with
code 1002 (protocol error).

------------------------------------------------------------------------

## 7. Implementation pointers

- `tools/mini_node.cpp` — verb dispatcher for `mini.hello`,
  `routes.get`, `mininodes.list`, `stun.observe`, `relay.forward`
  (catch-all path).
- `src/transport/ws_audio_bridge.cpp` — `audio.fetch` open/stream/close
  state machine, stream_id registry, BINARY framing.
- `src/transport/ws_bridge.cpp` — sibling JSON-envelope surface on the
  home node; not exposed on port 8082 but shares the envelope shape
  defined here so clients can reuse codecs.

These files are the source of truth for implementation behaviour. This
document is the source of truth for the wire contract.
