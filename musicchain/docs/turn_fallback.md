# mini-node TURN-like binary-relay fallback

Scope: `tools/mini_node.cpp` as of 2026-06-21. Companion docs:
[`ice_audit.md`](./ice_audit.md). Memory:
[[project-cellular-symmetric-nat]], [[project-musicchain-multi-vps-topology]].

This document is **spec-only**. It audits the relay path that already
exists today and proposes one new RPC verb — `nat.diagnose` — that lets
a player ask the mini-node which mode to use. No code changes are
implied by this file alone.

---

## 1. What is already TURN-like

Two complementary fallback channels are wired into the mini-node:

### 1a. `relay.forward` — JSON RPC relay
Source: `mini_node.cpp:1240-1299`, paired with `on_relay_reply`
(`mini_node.cpp:1317-1365`) and `relay.push.forward`
(`mini_node.cpp:1200-1239`).

Used for **JSON envelopes** (verb calls). The mini-node mints a fresh
`req_id`, parks `{originator_peer_id, original_req_id, ts}` in
`g_pending_relays`, forwards the rewritten request to the target full
node, and routes the reply back when it arrives. Push-only
notifications (no req_id correlation) use `relay.push.forward`.

### 1b. `'F'` (0x46) tag — binary-payload relay
Source: `mini_node.cpp:1367-1434`, registered in `main()` at
`mini_node.cpp:1674-1676`.

Used for **binary** (audio chunks, upload chunks, anything that would
suffer a 33 % base64 tax inside a JSON RPC). Wire format:

```
offset 0     : 0x46 ('F')                              1 byte
offset 1..40 : target peer_id as 40-byte HEX ASCII    40 bytes
offset 41..N : payload bytes                          N-41 bytes
```

The mini-node does **no envelope rewriting** on the payload: it calls
`rats_send_binary(g_client, target_hex, b + 41, N - 41)` and lets the
receiver's regular binary callback see the inner buffer with zero
relay metadata. The target peer doesn't even know the chunk was
relayed (which is what `WsAudioBridge::dispatch_chunk` relies on —
mini_node.cpp:1402-1412).

### When the `'F'` path kicks in

Today, **the caller decides**. There is no signal from the mini-node
saying "use the relay". The Dart player and the full node fall back to
the `'F'` tag only after a direct `rats_connect(public_address)` times
out — and they only learn that public address in the first place
through `player.locate` (mini_node.cpp:1125-1155) or
`routes.get` (mini_node.cpp:367-403).

For the **cellular-symmetric-NAT** case
([[project-cellular-symmetric-nat]]) the direct `rats_connect` will
*always* fail because the peer's NAT remaps the source port for every
new flow. The fallback timeout is therefore pure waste — typically
3–10 s per attempt, and uploads stall until the player retries via
relay. A player that knew up front "this pair must relay" could skip
the punch attempt entirely.

---

## 2. What the chain looks like for two cellular peers A ↔ B

Both peers are on cellular and connected only to the same mini-node V:

```
A ── librats/QUIC ──▶ V ◀── librats/QUIC ── B
```

Direct path (will fail on symmetric NAT pair):

1. A → V `stun.observe`             → V replies with A's mapped ip:port (own table)
2. A → V `player.locate {peer_id:B}` → V replies with B's mapped ip:port
3. A `rats_connect(B_addr)`         → TIMEOUT after kProbeTimeoutMs-ish
4. A → V `ice.connect_request {target_pid:B, my_addr:A_addr}`
                                    → V relays `ice.connect_invite` to B
                                    → B has no handler today (audit §3), drop on floor
5. Same TIMEOUT on the punch retry → fallback to relay

Relay path (today's working path, with no advance notice):

1. A → V `relay.forward {target:B, type:upload.chunk, body:{…}}`
   (small / control-plane messages).
2. A → V **binary frame** `0x46 || B_hex(40) || payload` for bulk
   bytes. V forwards verbatim via `rats_send_binary(B_hex, …)`. B's
   binary callback receives a clean payload with no relay envelope.
3. B replies symmetrically: any push goes via `relay.push.forward`
   (small / control) or its own `0x46`-tagged frame (bulk).

What's stored on the mini-node about each peer:

| Peer kind  | Table       | Reachability field?                |
|------------|-------------|------------------------------------|
| full node  | `g_routes`  | YES — `reachability` ∈ {Unknown, Direct, Relay}, tested by `probe_reachability` (mini_node.cpp:425-463) |
| player     | `g_players` | NO — only `public_address` + `announced_at_ms` (mini_node.cpp:154-158) |

The asymmetry is the root cause of the spec gap below: when a
**player** is the target, the mini-node has no opinion about whether
B can be directly punched. It only knows B's mapped ip:port from when
B connected to V — which is exactly the port that B's symmetric NAT
won't accept new flows on.

---

## 3. Proposed verb: `nat.diagnose`

Goal: a one-shot RPC that returns "direct" or "via_relay" so callers
stop wasting a hole-punch attempt on hopeless pairs and so a UI can
show the user *why* a stream is going through the VPS.

### Request

```json
{
  "req_id": "<caller's id>",
  "type":   "nat.diagnose",
  "body": {
    "peer_a": "<40-hex rats_peer_id>",   // usually the caller's own id
    "peer_b": "<40-hex rats_peer_id>"    // the target
  }
}
```

`peer_a` is allowed to equal the caller's `peer_id`; that's the common
case ("can I reach B directly?"). It is also legal for a third party
(e.g. a full node coordinating an upload) to ask about an A,B pair
where neither side is the caller, so the mini-node MUST NOT shortcut
by reading the request's transport-level peer_id.

### Response

```json
{
  "req_id":  "<caller's id>",
  "status":  "ok",
  "body": {
    "recommendation": "direct" | "via_relay" | "unknown",
    "peer_a": {
      "known":          true | false,
      "role":           "full_node" | "player" | "unknown",
      "reachability":   "direct" | "relay" | "unknown",
      "public_address": "ip:port" | "",
      "tested_at_ms":   <uint64 | 0>,
      "fresh":          true | false       // tested within kProbeMinIntervalMs
    },
    "peer_b": { … same shape … },
    "via_mininode_peer_id": "<this VPS's own rats_peer_id>",
    "reason": "<short human-readable string>"
  }
}
```

### Decision table

The mini-node combines what it knows about each peer. The matrix is
asymmetric because a single `direct` side is **not** enough — both
sides must be punchable for the connect to succeed when one of them
is behind a symmetric NAT.

| A.reachability | B.reachability | recommendation | reason                                  |
|----------------|----------------|----------------|-----------------------------------------|
| direct         | direct         | `direct`       | both endpoints punchable                |
| direct         | relay          | `via_relay`    | B is relay-only (symmetric NAT side)    |
| relay          | direct         | `via_relay`    | A is relay-only                         |
| relay          | relay          | `via_relay`    | both relay-only — TURN-style mandatory  |
| direct         | unknown        | `direct`       | optimistic; caller may retry            |
| unknown        | direct         | `direct`       | optimistic; caller may retry            |
| unknown        | unknown        | `unknown`      | mini-node has no opinion yet            |
| any            | not-known      | `via_relay`    | safer to use the relay we *do* control  |

"Not known" means the peer is absent from both `g_routes` and
`g_players` on this mini-node. In a multi-VPS deployment a follow-up
fan-out to peer mini-nodes is reasonable (out of scope here — see §5).

### Reachability for players (the new data this RPC needs)

Today `PlayerEntry` does not store reachability. The proposed verb
needs the same `Reachability { Unknown, Direct, Relay }` field
`RouteEntry` already has, populated by reusing
`probe_reachability(node_id, public_address)` (mini_node.cpp:425-463).

Probe trigger policy (spec, not code):
- On `player.announce`, schedule a probe iff
  `now_ms() - last_probe_ms > kProbeMinIntervalMs` (60 s — same as
  routes).
- On `nat.diagnose` for a player that has never been probed, schedule
  one **and** return `unknown` immediately so the caller doesn't
  block. The next call (≥ probe RTT later) will see the cached
  verdict.
- Expiry: treat any cached verdict older than `kReachabilityTtlMs`
  (proposed 10 min) as `unknown`. Cellular networks rebind ports
  often; stale `direct` verdicts are worse than `unknown`.

### Edge cases

- **Peer unknown to this mini-node.** Return
  `recommendation:"via_relay"` only if at least one of A/B is known
  here; otherwise `unknown` (we have nothing to relay through).
- **Peer is a full node with `reachability=Direct` but not currently
  connected to this VPS.** Trust `g_routes.reachability` — it was
  tested from a fresh ephemeral socket, so it generalises across
  callers.
- **Self-diagnose (`peer_a == peer_b`).** Return
  `status:"invalid_diagnose"`.
- **`req_id` reuse.** None — the RPC is request/reply, no async push.

### Why this is safe to ship without changing the relay path

`nat.diagnose` is a **read-only advisory**. The `'F'`-tag binary relay
and `relay.forward` continue to work exactly as today. A caller that
ignores the recommendation still gets a working (if slower) result.
That makes the rollout trivial: ship the verb on the mini-node, then
let the player consume it opportunistically; older players keep
working unchanged.

---

## 4. Integration with `ice.connect_request`

Per `ice_audit.md` §3, `ice.connect_request` is currently
fire-and-forget and the receiver-side `ice.connect_invite` has no
handler. The clean ordering for a player is:

1. `nat.diagnose {peer_a:self, peer_b:target}` → if `via_relay`, go
   straight to the `'F'` relay / `relay.forward` for everything.
2. If `direct`, fire `ice.connect_request` AND start a
   `rats_connect(target_addr)` in parallel. The mini-node's
   recommendation is a hint, not a guarantee.
3. If the direct connect fails despite a `direct` recommendation,
   downgrade and retry over relay. Optionally re-call `nat.diagnose`
   so the mini-node can downgrade its cached verdict.

This sequencing also lets us delete `ice.connect_request` for pairs
where `nat.diagnose` returns `via_relay`: we save a useless
`ice.connect_invite` send to a peer that has no handler today.

---

## 5. Multi-VPS considerations (out of scope, noted)

In the multi-VPS topology ([[project-musicchain-multi-vps-topology]]),
two players A and B may be connected to **different** mini-nodes V1
and V2. `nat.diagnose` against V1 only knows A's reachability and
maybe B's via cross-replication (we already replicate
`g_routes`-style entries via `mini.hello` / `replicate_routes_to_peer`,
mini_node.cpp:472-475 — no equivalent yet for `g_players`).

A future v2 of the verb could fan-out to `mininodes.list` and merge
responses, but the v1 spec deliberately scopes to "what this single
mini-node knows" so it can ship without touching the mesh-replication
machinery.

---

## 6. Summary of changes implied (for whoever implements this later)

- Add `Reachability reachability` + `uint64_t reachability_tested_at_ms`
  to `PlayerEntry` (mini_node.cpp:154-158).
- Hook `probe_reachability` (already exists for routes,
  mini_node.cpp:425-463) into `player.announce` (mini_node.cpp:1088-1124)
  and into `nat.diagnose` cache-miss path.
- Add the `nat.diagnose` arm to the type-dispatch chain
  (mini_node.cpp:1063-1300) **before** the trailing `else` so the
  `unknown_type` error message at mini_node.cpp:1303 stays in sync.
- Update the help string at mini_node.cpp:1303 to include
  `nat.diagnose`.
- Add Dart client wrapper in the player + the full node so the
  recommendation can be acted upon.

No changes to the `'F'`-tag binary format, no changes to
`relay.forward`. Backwards compatible.
