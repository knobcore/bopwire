# mini-node ICE / NAT-traversal audit

Scope: `tools/mini_node.cpp` as of 2026-06-21.
Companion memory: [[project-cellular-symmetric-nat]], [[project-musicchain-multi-vps-topology]].

## 1. Served verb inventory (mini_node.cpp:1303)

The unknown-type reply lists the full RPC surface:

```
routes.get / mininodes.list / mini.hello / status / stun.observe
/ relay.forward / relay.push.forward / ice.connect_request
```

Plus implicit handlers: `player.announce`, `player.locate`, and the binary
`'F'` (0x46) relay tag.

## 2. `ice.connect_request` — what it actually does

`mini_node.cpp:1165-1199`:

- Input body: `{target_pid, my_addr: "ip:port"}`.
- The caller (a player) asks the mini-node to invite `target_pid` (a full
  node it has discovered via `routes.get` / DHT) to hole-punch back to
  `my_addr` (the caller's NAT-mapped external endpoint, normally learned
  via `stun.observe` against this same mini-node).
- The mini-node wraps that as `{type: ice.connect_invite, body:
  {caller_addr, caller_pid}}` and `rats_send_message`s it to `target_pid`
  over the existing librats link. It immediately replies `status: ok` /
  `forwarded_to: <target>` to the player — fire-and-forget, no
  punch-result correlation.
- Intent (per comment): the full node "responds by initiating a
  hole-punch outbound to my_addr". This is the textbook session-init
  half of ICE.

## 3. Is the mini-node a signaling channel for phone↔phone punching?

Partially, by accident of plumbing — not by design.

What works today:

- `player.announce` records `(rats_peer_id → public_address)` from the
  source-port the mini-node observed on the librats handshake
  (`mini_node.cpp:1063-1124`, mirrors `stun.observe`). This is a
  signaling-server role.
- `player.locate` lets a second player resolve a peer-id back to that
  observed `ip:port` (`mini_node.cpp:1125-1155`). That's enough for a
  Trickle-ICE-style attempt where the dialer just calls `rats_connect`.

What is missing for phone↔phone:

- `ice.connect_request` is hard-wired to "player asks mini-node to ask
  *a full node*". Nothing inspects whether `target_pid` is another
  player. It will still forward the invite, but —
- **No receiver implements `ice.connect_invite`.** A grep across
  `src/` and the Dart player tree returns zero matches. The full node
  binary doesn't know the verb either. So today the invite lands and is
  dropped on the floor. The protocol only "works" because the player
  ALSO calls `rats_connect(public_address)` directly after `player.locate`
  — the mini-node invite is decorative.
- There is no `punch_window_open` / `punch_failed` correlation, no
  retry, no nonce. The mini-node returns `status:ok` the instant
  `rats_send_message` queues the invite, regardless of whether the peer
  is even online.

## 4. Relay / TURN fallback

Two distinct fallback channels exist:

- **JSON RPC fallback — `relay.forward`** (`mini_node.cpp:1240-1299`).
  The mini-node unpacks `{target_peer_id, type, body}`, mints a fresh
  `req_id`, forwards over its existing librats link to the target, and
  `on_relay_reply` (`mini_node.cpp:1317-1365`) catches the reply by
  `req_id` and re-routes it to the original caller. This is mature and
  well-instrumented — it includes `originator_peer_id` so the far side
  can push later notifications back via `relay.push.forward`.

- **Binary fallback — `'F'` tag, 0x46** (`mini_node.cpp:1389-1434`).
  Wire format: `byte 0 = 'F'`, `bytes 1..40 = target peer_id as 40-byte
  hex ASCII`, `bytes 41..N = payload`. The mini-node re-emits the
  payload via `rats_send_binary(target_hex, …)` with no envelope
  rewriting. This IS the TURN-like fallback — explicitly documented at
  `mini_node.cpp:1367-1387` as "even their *binary* traffic (upload
  chunks, stream chunks) has to take the relay route through us" for
  symmetric-NAT cellular peers, and the registration in `main()` at
  `mini_node.cpp:1674-1676` calls it out by name.

  Caveats: no flow control, no per-stream accounting, no quota; the
  mini-node simply repeats every chunk. A misbehaving or malicious
  client can use the VPS as free bandwidth.

## 5. Cellular / symmetric NAT path

There is **no automatic decision** in the mini-node that says "this
peer is symmetric, don't bother punching, route via me". The mini-node
treats relay vs. punch as orthogonal services: `relay.forward` and the
`'F'` binary path are always available, and the caller decides when to
use them. In practice the player is expected to:

1. `stun.observe` to learn its own mapped address.
2. `player.locate` (or `routes.get`) to learn the target's address.
3. Try `rats_connect` directly.
4. On timeout, fall back to `relay.forward` / `'F'`-tag binary.

For cellular ↔ cellular through symmetric NATs, step 3 will reliably
fail (per the memory note) and step 4 is the only path. The mini-node
does not surface its own NAT classification, so the player has no hint
beyond "the connect attempt timed out".

## Gaps

1. **`ice.connect_invite` has no receiver.** The signaling half-handshake
   is dead code on the far end. Either remove `ice.connect_request` or
   ship a handler in the full node / Dart player.
2. **No phone↔phone path in the signaling verb.** `ice.connect_request`
   assumes target == full node and uses the mini-node's own peer table.
   Phone targets that have only `player.announce`d need the same
   treatment.
3. **No reverse / lateral invite.** There is no way for the *target* to
   say "I tried, it failed, please relay" — the only signal of failure
   is a missing TCP/QUIC connection on the caller side.
4. **No NAT-classification hint in `stun.observe` reply.** Caller cannot
   tell mapped-port stability without making multiple calls to
   different mini-nodes (which is exactly what the multi-VPS topology
   makes possible — currently unused).
5. **Binary relay has no accounting or quota.** TURN-like service is
   wide open to any connected peer.
6. **`status:ok` on `ice.connect_request` is misleading** — it confirms
   only that the mini-node sent the librats message, not that the
   invite was delivered or the punch began.

## Proposal

Phase 1 — fix the existing verb without protocol churn:

- Implement `ice.connect_invite` in the full node and Dart player:
  on receipt, do `rats_connect(caller_addr)` immediately, then push
  `ice.connect_result {success: bool, peer_id}` back to `caller_pid`
  via the same mini-node using `relay.push.forward`.
- Have the mini-node correlate `ice.connect_request` ↔
  `ice.connect_result` by req_id and return the real outcome to the
  caller instead of fire-and-forget `ok`.
- Make `ice.connect_request` look up `target_pid` in **both**
  `g_players` and the full-node table so the same verb covers
  phone↔phone.

Phase 2 — NAT classification hint:

- Add `nat_observed: {mapped_ip, mapped_port, source_port_drift}` to
  the `stun.observe` reply. Caller queries two mini-nodes (we already
  have `mininodes.list`), compares mapped ports, decides "cone" vs.
  "symmetric" and skips straight to relay on symmetric.

Phase 3 — relay quotas:

- Track per-peer bytes/sec on the `'F'` path; cap or drop when over a
  configurable threshold. Surface counters in `status`.

Phase 4 — relay-of-last-resort selection:

- When the dialer falls back to relay, prefer the mini-node that
  already has both peers connected (a `mininodes.list` cross-product
  in the player), not just the closest one. Avoids the
  hairpin-through-two-VPSes case.
