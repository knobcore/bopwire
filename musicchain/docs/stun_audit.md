# Mini-node STUN audit

Scope: `C:\Users\lain\blockchain\musicchain\tools\mini_node.cpp` and the
full-node/player code that should consume its STUN-equivalent output.

## What's there today

### `stun.observe` RPC verb

`mini_node.cpp:1063-1087` (handler) and `mini_node.cpp:1303` (error string
listing it as a supported verb).

The mini-node implements a JSON-over-QUIC STUN-equivalent, **not** an
RFC 5389 STUN server:

- Caller sends `{type:"stun.observe", req_id:...}` over the existing librats
  QUIC connection (same UDP/443 the mini-node already terminates).
- Handler calls `rats_get_peer_info_json(g_client, peer_id)` and pulls
  `ip` + `source_port` (NOT `port` — librats overwrites `port` with the
  peer's claimed listen_port, which is the wrong thing to echo back).
- Reply body: `{"observed_address": "<ip>:<source_port>"}`
  (`mini_node.cpp:1085`).
- No external port, no RFC 5389 framing, no XOR-MAPPED-ADDRESS — purely
  an application-layer reflection.

Note: `src/api/rats_api.cpp:255-275` implements the **same verb on the
full node**, but it reads `ip`/`port` (no `source_port` fallback) — fine
for vanilla librats but mismatched with what mini_node exposes.

### Where the mini-node advertises STUN-style observations

1. **`player.announce`** (`mini_node.cpp:1088-1124`) — when a player
   connects and announces, the mini-node observes its `ip:source_port`
   and stores it in `g_players[peer_id].public_address`
   (`mini_node.cpp:1109`). Returned in the reply at `mini_node.cpp:1120`.

2. **`player.locate`** (`mini_node.cpp:1125-1155`) — third parties can
   look up another player's `public_address` here.

3. **`routes.get`** (`mini_node.cpp:382-396` via `routes_json()`) — each
   peer entry includes `public_address`, but the value comes from the
   **route message the full node published itself**
   (`mini_node.cpp:488`, `ingest_route`), not from the mini-node's own
   observation of that node's QUIC source address.

4. **`mini.hello`** reply (`mini_node.cpp:975-979`) — includes
   `peer_address` (the mini-node's view of the caller's address) for
   the caller's benefit, plus `route_count`. So the mini→mini hello path
   does effectively double as a STUN reflection for other mini-nodes.

5. **`mininodes.list`** (`mini_node.cpp:980-1009`) — re-exports
   `g_mininode_addr[pid]` for each peer mini-node; the address comes
   from `peer_address(peer_id)` recorded at `mini.hello` time
   (`mini_node.cpp:959`).

### Do full nodes / swarm peers use `stun.observe` to populate `public_address`?

**No, not anymore.** `src/network/rats_link.cpp:221-234`
(`observe_public_address_via_vps`) used to issue the `stun.observe` RPC,
but it currently calls `rats_discover_public_address(client_,
"stun.l.google.com", 19302, 3000)` — i.e. it talks **vanilla RFC 5389
STUN to Google's public STUN server**. The mini-node's verb is dead
code from the full-node side.

The full node's `public_address` field shipped in `route_publish` JSON
(`rats_link.cpp:164`) and ingested by mini_node at line 488 is therefore
self-reported from the Google STUN result, NOT observed by the mini-node.

### Is there a separate RFC 5389 STUN server on the mini-node?

**No.** The mini-node listens only on librats/QUIC (UDP/443). All STUN
behavior is the application-layer `stun.observe` verb described above.
There is no UDP port that speaks the binary STUN protocol, no
MAPPED-ADDRESS / XOR-MAPPED-ADDRESS encoding, no STUN magic cookie
handling. The verb's name is the only thing "STUN" about it.

## Gaps relative to "browser leaf needs reliable STUN-observed public address from the mini-node"

1. **No RFC 5389 server**. A browser leaf (WebRTC ICE agent) speaks
   binary STUN, not librats JSON RPC. It cannot consume `stun.observe`
   directly. Browsers always require a real STUN URL.

2. **No standalone UDP listener**. Even if we add a STUN codec, the
   mini-node currently shares UDP/443 with msquic; we'd need either
   (a) a STUN demux in front of msquic, or (b) a second UDP port.

3. **Full nodes don't consume the verb**. `rats_link.cpp:226` points
   at `stun.l.google.com:19302`, so the mini-node's careful
   `source_port` extraction is unused. If Google's STUN ever blocks us
   or the path differs from the VPS path (e.g. carrier-grade NAT
   re-maps based on destination), full nodes record an address that's
   not the one the VPS would punch into.

4. **Source-port disagreement between full node and mini-node verbs**.
   `rats_api.cpp:264-272` uses `port` (claimed listen port); only
   `mini_node.cpp:1080` falls back through `source_port` first. A
   browser-style consumer querying either endpoint would see different
   values.

5. **No port-mapping signaling**. RFC 5780 NAT-behavior discovery
   (CHANGE-REQUEST, OTHER-ADDRESS) is absent, so a leaf cannot tell
   whether its NAT is symmetric (the precondition for needing TURN
   instead of ICE). The mini-node's own `probe_reachability`
   (`mini_node.cpp:425-463`) computes a `direct`/`relay` flag for
   full-node routes but does NOT expose it as a STUN attribute or
   include it in any reply consumable by a browser.

6. **No authentication / rate-limiting**. `stun.observe` is open to any
   connected peer. RFC 5389 short-term credentials or a per-peer rate
   limit would be needed before exposing this externally.

7. **No multi-VPS consistency check**. With the multi-VPS topology
   (see `project_musicchain_multi_vps_topology.md`), a leaf that hits
   different mini-nodes via DHT will get different observed addresses
   if its NAT is symmetric — there's no aggregation layer to tell it
   "your mapping is unstable, ask for TURN".

## Proposed next steps (no code yet)

1. **Add an RFC 5389 listener on a separate UDP port** (e.g. 3478) on
   each mini-node, sharing the same process. Browser leaves point at
   `stun:vps.musicchain.example:3478`. Keep `stun.observe` as the
   in-band variant for librats peers.

2. **Unify the source-of-truth**. Make `rats_link.cpp` call
   `mini-node`'s `stun.observe` (the same VPS that will relay our
   traffic) instead of Google. Falls back to Google only if no VPS is
   reachable. This makes `public_address` represent the mapping seen
   from the path that actually matters.

3. **Fix the `port` vs `source_port` mismatch** in
   `src/api/rats_api.cpp:264` so the full-node verb matches mini-node
   semantics (`source_port` first, then `port`).

4. **Expose reachability classification**. Add a `stun.classify` verb
   (or piggyback on the new RFC 5389 server's RFC 5780 extensions)
   that runs the two-mapping test from two different mini-node UDP
   ports and returns `direct` / `port-restricted` / `symmetric`. Lets
   browser leaves decide whether ICE will work or they need to use
   the relay path immediately.

5. **Document the verb** in the player/full-node API reference so
   future browser code knows it exists, even though the binary STUN
   server is the primary consumer.

6. **Cross-mini-node aggregation**. Add `stun.observe_via` that asks
   another mini-node to dial back to the caller's claimed address;
   if it succeeds, the caller is reachable; if it fails on a second
   mini-node, the caller's NAT is likely symmetric. Echoes the existing
   `probe_reachability` logic but driven by the leaf instead of by
   route-ingest.
