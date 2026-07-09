# P4+P5 Implementation Plan

I have all the load-bearing paths confirmed. Note one key finding that resolves red-team item 1: `apply_node_auth` (chain.cpp:685-690) already writes the 33-byte pubkey to `v:` AND an empty marker to `va:`. So `v:` already has a clean on-chain write path via NodeAuthTx; the ONLY non-deterministic writer is the boot self-register at node_main.cpp:430. Design-1's pick is coherent; design-2's `va:`-pubkey rewrite is unnecessary and wrong. Here is the synthesized plan.

---

# P4 + P5 UNIFIED IMPLEMENTATION PLAN (branch `reward-onchain-scale`)

Legend: **[CC]** = consensus-critical (needs adversarial pass + determinism test before merge). **[SAFE]** = local/perf/no consensus surface. Every file path absolute-relative to `C:\Users\lain\blockchain\bopwire`.

---

## 0. GLOBAL DECISIONS (settle before any code)

**D1 — `v:` decision (final).** `v:` is PURELY ON-CHAIN and INCLUDED in `state_root`. `apply_node_auth` (chain.cpp:685-690) already writes the 33-byte pubkey to `v:` and an empty marker to `va:`. The sole divergent writer is the boot self-register at **tools/node_main.cpp:430** — DELETE it. `check_play`/`recompute_mint` (mint.cpp:149/175) keep reading `v:` unchanged. `va:` stays as-is (empty on-chain "authorized" marker the founder emitter polls; it is already in `clear_derived_state` at database.cpp:1035). **Reject design-2's `va:`-pubkey migration entirely** — it would blind `check_play`, which needs the pubkey that lives in `v:`.

**D2 — Single source of truth for the STATE set.** Define ONE constant used by the accumulator hook, `clear_derived_state`, and the snapshot dumper. They can never be allowed to drift.

**D3 — Clean-slate hard fork.** Bump `BLOCK_VERSION 3→4` (block.h:29) AND `MC_CHAIN_ID 19779→19780` (block.h:43), fresh genesis. This makes the v4 `state_root` gate always-live (no mixed v3/v4 DB to reason about) and kills the "legacy v3 carries no root" ambiguity. The `version>=4` deserialize gate is still written (cheap, future-proof) but is effectively always true.

---

## PHASE 4.0 — Determinism substrate & the STATE_PREFIXES constant  **[CC]**

**Step 4.0.1 — New file `src/storage/state_prefixes.h`.** Define the single canonical set:
```
namespace mc {
// The exact multiset of key-prefixes the committed state_root commits to.
// MUST equal: (accumulator hook set) == (clear_derived_state set) == (snapshot dump set).
inline constexpr const char* kStatePrefixes[] = {
  "a:","s:","f:","i:","u:","us:","va:","v:","bh:",
  "nv:","sm:","ia:","ig:",
  "founder:","mlvl:","mpub:","mact:","slashed:",
  "un:","addrun:",
  "prop:","propstatus:","propvote:"
};
// Single-key STATE members (not prefixes): "c:total_supply".
bool has_state_prefix(const std::string& key);  // longest-match over kStatePrefixes + the c:total_supply exact key
}
```
Note vs current code: **`v:` is ADDED** (was excluded), per D1. `fph:` is deliberately NOT here (excluded from root, derivable from `f:`) but is added to the CLEAR set separately in 4.0.3.

**Step 4.0.2 — database.cpp:1034-1039 : replace the inline `prefixes` vector with `kStatePrefixes` + explicit `c:total_supply`.** `clear_derived_state` iterates `kStatePrefixes` (now including `v:`) and then `batch.Delete("c:total_supply")` (unchanged at :1057). Update the comment block at :1020-1033 to state `v:` is now on-chain-only (self-register deleted).

**Step 4.0.3 — database.cpp:1039 : add `fph:` to the CLEAR loop only** (NOT to `kStatePrefixes`). Fixes red-team T3-13: `put_fingerprint` writes `fph:` but it was never cleared, leaking fuzzy-dup false-positives across reorg branches. It stays out of the root because it is derivable from `f:`. Concretely iterate a second local list `{"fph:"}` after the `kStatePrefixes` loop, or append it to a "clear-but-not-root" vector.

**Step 4.0.4 — tools/node_main.cpp:430 : DELETE the `db.put("v:"+node_id, pubkey)` self-register.** Keep the NodeAuthTx emitter at :766-808. Add a one-line comment: `v:` is populated exclusively by replayed founder-signed NodeAuthTx.

**Determinism audit hook (the cheapest guard — build first):**
**Step 4.0.5 — debug-only full-scan assertion.** In `connect_block` after `db_.write(batch)` succeeds (chain.cpp:194), under `#ifndef NDEBUG`, call a new `Database::scan_sum_state_root()` that iterates `kStatePrefixes` + `c:total_supply` and folds every live leaf into a fresh LtHash, then `assert(scan_root == working_acc_.root())`. This catches ANY stray raw `db.put` on a STATE key or a hook/clear drift immediately in CI (red-team T1-5, T2-12). Same assertion at the end of `rebuild_derived_state`. **[CC]** for correctness of the guard; the assertion itself never ships in release.

---

## PHASE 4.1 — Committed `state_root` via LtHash accumulator  **[CC — the core adversarial surface]**

### 4.1.A The consensus predicate (identical at produce / connect / rebuild)

Define once, in prose that the three call sites must satisfy byte-for-byte:

```
leaf(key, val) := expand( SHA256( u32le(key.size) || key || u32le(val.size) || val ) )
  where expand(seed) emits 1024 little-endian u16 lanes:
        for i in 0..15: block_i = SHA256( seed || u32le(i) )   // 32 bytes each → 16*32=512 bytes = 256 u16... 
```
(Sizing: to fill 1024 u16 lanes = 2048 bytes you need 64 SHA256 output-bytes*32... use 64 blocks of 32 bytes = 2048 bytes → 1024 u16. So `for i in 0..63: block_i = SHA256(seed || u32le(i))`, concatenate to 2048 bytes, read as 1024 u16 LE.)

```
A := elementwise ( Σ over live (key,val) leaves )  mod 2^16      // std::array<uint16_t,1024>
state_root(A) := SHA256( A as 2048 LE bytes )                     // Hash256, goes in the header

on_put(k, new):  old = overlay.count(k)? overlay[k] : db.get(k);
                 A += leaf(k,new);  if(old) A -= leaf(k,*old);  overlay[k]=new
on_del(k):       old = overlay.count(k)? overlay[k] : db.get(k);
                 if(old) A -= leaf(k,*old);           overlay[k]=nullopt
```

Properties relied on: order-independence (commutative add) and homomorphic reversal (subtract old leaf). Integer-only, no float/locale, bit-identical cross-platform.

### 4.1.B Data structures

**Step 4.1.1 — New files `src/storage/accumulator.h` / `.cpp`.**
```
struct StateAccumulator {
    std::array<uint16_t,1024> vec{};                              // running A
    std::map<std::string, std::optional<std::vector<uint8_t>>> overlay;  // block-scoped old-value cache
    Database* db = nullptr;
    void copy_from(const std::array<uint16_t,1024>& committed);   // start-of-block
    void on_put(const std::string& k, const std::vector<uint8_t>& v);
    void on_del(const std::string& k);
    Hash256 root() const;                                         // SHA256(vec bytes)
    static std::array<uint16_t,1024> leaf(const std::string& k, const std::vector<uint8_t>& v);
    static std::vector<uint8_t> serialize_vec(const std::array<uint16_t,1024>&);   // for sr:vec
    static std::array<uint16_t,1024> deserialize_vec(const std::vector<uint8_t>&);
};
```
`leaf()` reuses `crypto::sha256` — no new dependency.

### 4.1.C Hook into the single write choke point

**Step 4.1.2 — database.h : add `StateAccumulator* acc_ = nullptr;` + `void set_accumulator(StateAccumulator*)`; declare `get_state_vec()/put_state_vec_batch()` for the `sr:vec` key.**

**Step 4.1.3 — database.cpp:81-88 : gate the hook in `put_batch`/`del_batch`.**
```
void Database::put_batch(batch, key, value) {
    if (acc_ && has_state_prefix(key)) acc_->on_put(key, value);   // BEFORE b.Put
    b.Put(...);
}
void Database::del_batch(batch, key) {
    if (acc_ && has_state_prefix(key)) acc_->on_del(key);          // BEFORE b.Delete
    b.Delete(key);
}
```
`clear_derived_state` uses raw `db_->Write` (database.cpp:1058) and bypasses the hook — intended (clearing must not perturb the accumulator). **Audit obligation (red-team T1-5):** confirm EVERY STATE-prefix write goes through `put_batch`/`del_batch`. The one known raw `db.put` on a STATE key (`v:` self-register) is deleted in 4.0.4. `apply_node_auth`'s direct `batch.Delete(key)` at chain.cpp:692-693 for the revoke path **must be changed to `db_.del_batch(batch, key)`** so the accumulator sees the delete — otherwise a revoke desyncs the root. Same for any other bare `batch.Delete` on a STATE key: grep and route all through `del_batch`.

### 4.1.D Chain ownership + the one commit

**Step 4.1.4 — chain.h : add members** `std::array<uint16_t,1024> committed_acc_{};`, `StateAccumulator working_acc_;`, and decl `Hash256 compute_candidate_state_root(const Block&);` (locked). On chain construction, load `committed_acc_` from `sr:vec` (see 4.1.8 boot path).

**Step 4.1.5 — Factor the block write-set into ONE helper (red-team T0-3).** New private method
```
bool Chain::apply_block_writes(const Block& block, uint32_t height, leveldb::WriteBatch& batch);
```
containing EXACTLY the song-index block (chain.cpp:158-170: `put_fingerprint`, `put_song_meta`, `add_to_artist_index`, `add_to_genre_index`, `set_content_height`, `del_batch sp:`) **plus** `apply_transactions(block,height,batch)`. Both `connect_block` and `compute_candidate_state_root` and `rebuild_derived_state`'s per-block body call this identical helper so the stamped root can never miss the five index leaves. **[CC]**

**Step 4.1.6 — chain.cpp:237-245 : accumulator lifecycle in `apply_transactions` start** — actually hoist to the helper boundary. At the top of `apply_block_writes` (or in connect_block just before calling it): `working_acc_.copy_from(committed_acc_); working_acc_.overlay.clear(); working_acc_.db = &db_; db_.set_accumulator(&working_acc_);`. At every exit path of `connect_block`/rebuild-body: `db_.set_accumulator(nullptr);` (use a scope-guard to cover early returns).

**Step 4.1.7 — chain.cpp:194 : recompute + gate + persist in the SAME batch.** Immediately before `db_.write(batch)`:
```
Hash256 root = working_acc_.root();
if (block.header.version >= 4 && root != block.header.state_root) {
    db_.set_accumulator(nullptr);
    return false;                              // reject pre-commit
}
db_.put_state_vec_batch(batch, "sr:vec", StateAccumulator::serialize_vec(working_acc_.vec));
// also stamp height alongside: sr:height = new_height (see 4.1.8)
```
After `db_.write(batch)` succeeds: `committed_acc_ = working_acc_.vec; db_.set_accumulator(nullptr);`. `sr:vec`/`sr:height` are node-local, NOT in `kStatePrefixes` (they must never feed their own root). Atomicity: accumulator vector and chain state fall or commit together. **[CC]**

**Step 4.1.8 — Boot reconstruction (red-team T0-4).** On chain open: read `sr:vec` + `sr:height`. If present AND `sr:height == tip_.height`, load `committed_acc_` from it. Otherwise (missing, or height mismatch → upgrade/crash/snapshot-install-without-vec) run a ONE-TIME `Database::scan_sum_state_root()`-style full fold over `kStatePrefixes`+`c:total_supply` to rebuild `committed_acc_`, then persist `sr:vec`+`sr:height=tip_`. Never default to zero silently. **[CC]**

### 4.1.E Producer stamp

**Step 4.1.9 — chain.cpp new `compute_candidate_state_root(const Block& block)`** (holds `mu_`): copy `committed_acc_` into a throwaway `StateAccumulator`, `set_accumulator`, run `apply_block_writes(block, tip_.height+1, throwaway_batch)` against a **discarded** WriteBatch, capture `root()`, `set_accumulator(nullptr)`, DO NOT write the batch. Returns the 32-byte root. Reuses the exact helper from 4.1.5 → produce path and connect path are provably identical.

**Step 4.1.10 — candidate.cpp:161, inside `commit_block` before `chain.connect_block(block)`:**
```
block.header.state_root = chain.compute_candidate_state_root(block);
```
The header is hashed AFTER this (block.hash() is computed lazily in serialize/connect). `connect_block` then re-derives and asserts equality as a self-check. Double-apply cost accepted at current volume; Phase-5 optimization noted in open risks.

### 4.1.F Rebuild + reorg (floor-aware — red-team T0-2)

**Step 4.1.11 — chain.cpp:1615 `rebuild_derived_state` : make it floor-aware and state-preserving.**
- Add member `uint32_t replay_floor_ = 0;` (0 for archival nodes; = `H_snap` for snapshot/pruned nodes, set on install/prune).
- If `replay_floor_ == 0`: current behavior but with accumulator — `clear_derived_state()`, zero `committed_acc_`, replay `h=1..tip`, per block call `apply_block_writes` under `working_acc_`, and for v4 headers REQUIRE `working_acc_.root() == header.state_root` before the per-block `db_.write(batch)` at :1695 (else break/truncate exactly like the existing prev_hash break at :1633). Persist `sr:vec`+`sr:height` in that batch; commit `committed_acc_`.
- If `replay_floor_ > 0`: **DO NOT `clear_derived_state()`** (it is height-blind and would wipe `< floor` snapshot state → brick). Instead: keep the installed snapshot state in place, seed `committed_acc_` from the snapshot-carried accumulator vector (`sr:vec` written at install, see 4.3), then replay ONLY `floor+1..tip` on top via `apply_block_writes`, gating each root. Add a guard: `for (h = replay_floor_+1; h <= tip; ++h)`.

**Step 4.1.12 — chain.cpp:1579 reorg restore : same floor-awareness.** The unconditional `rebuild_derived_state()` at :1579 now honors `replay_floor_`. On a pruned/snapshot node a reorg never replays below floor (and cannot — bodies are gone), which is safe because `FINALITY_DEPTH`(1000) > any snapshot-to-tip gap by construction (SNAPSHOT_INTERVAL ≪ FINALITY_DEPTH, see 4.4). LtHash subtraction gives a future incremental-disconnect path, but not required now: full floored rebuild is deterministic. **[CC]**

### 4.1.G Header format v4

**Step 4.1.13 — block.h:29 : `BLOCK_VERSION 3→4`; block.h:43 : `MC_CHAIN_ID 19779→19780`.** **[CC]**

**Step 4.1.14 — block.h:171 : add `Hash256 state_root{};` after `timestamp_ms`** in `BlockHeader`.

**Step 4.1.15 — block.cpp:76 : in `BlockHeader::serialize`, after `write_u64le(buf, timestamp_ms)` add `write_bytes(buf, state_root.data(), 32);`.** Folds `state_root` into the block-hash preimage automatically (block.cpp:81-84 `hash()` hashes the serialized header). No separate signing hash.

**Step 4.1.16 — block.cpp:140 : in deserialize, after reading `timestamp_ms`:** `if (out.header.version >= 4) { if(!read_bytes(p,end,out.header.state_root.data(),32)) return false; }`. Leaves zero for v3 (dead path under clean-slate fork but safe).

**Step 4.1.17 — `Block::validate()` UNCHANGED.** `state_root` needs chain context; its check lives ONLY in connect_block/rebuild, never in context-free structural validate. Tx signatures anchor to `TxType::sign_message` (chain_id), not the block hash, so the header change does not touch tx-sig anchoring.

---

## PHASE 4.1.5 — Chromaprint float determinism  **[CC — independent but must ship with the root]**

**Step 4.1.5.1 — the fuzzy uniqueness comparison (`song_on_chain` → `kChromaprintSimThreshold = 0.70`, consumed at chain.cpp:1657 rebuild and the connect-time path).** Red-team T2-12: a float similarity compared across x87/SSE/FMA/fast-math can make one node accept and another reject the SAME song block → divergent chain → now a HARD `state_root` mismatch → truncation. Fix: quantize the similarity to an integer (e.g. `score_q = round(sim * 1e6)`) computed in a fixed, `-ffp-contract=off` / no-fast-math translation unit, and compare `score_q >= 700000` as integers. Add it to the determinism audit list. Locate the impl (audio matcher) and make the comparison integer end-to-end. **[CC]**

---

## PHASE 4.2 — Founder NodeAuthTx guaranteed inclusion (bootstrap mint gap)  **[SAFE-ish, CC on the "privileged inclusion" ordering]**

Red-team T3-15 / open-risk: with the self-register gone, the founder node cannot mint until its own NodeAuthTx lands.

**Step 4.2.1 — Genesis-time NodeAuthTx.** Seed the founder validator as an on-chain NodeAuthTx included in the first block the operator bootstraps (alongside the founder GRANT), so `v:` is populated at height 1. Deterministic (it is a real tx replayed identically). Keep the 30s retry at node_main.cpp:766-808 as belt-and-suspenders.
**Step 4.2.2 — Privileged inclusion:** when the founder producer builds a block, always include its own pending NodeAuthTx first if `va:[node_id]` is absent (it is tx-only, no mint dependency, cannot poison). This is a producer-local selection preference, not a consensus rule — every node still validates the resulting block normally.

---

## PHASE 4.3 — Snapshots  **[CC on the verify path; SAFE on the dump/serve plumbing]**

### 4.3.A Deterministic dump

**Step 4.3.1 — database.{h,cpp} : `dump_state_chunks(size_t chunk_bytes, std::vector<Chunk>& out, Hash256& out_state_root)`.** Opens ONE `leveldb::Snapshot` (pins a consistent sequence vs concurrent apply). Iterates `kStatePrefixes` + `c:total_supply` in leveldb native byte-sorted order, emits length-prefixed records `(u32 keylen, key, u32 vallen, val)` packed into fixed `chunk_bytes` (4 MiB). Each chunk `sha256`'d. Recompute `out_state_root` by folding the dumped leaves through `StateAccumulator::leaf` (must equal the committed header root — order-independent). **[CC]**

**Step 4.3.2 — database.{h,cpp} : `install_state_chunk(bytes)`** raw-writes decoded pairs in a WriteBatch; and `Database::state_vec_from_dump()` to compute `sr:vec` from the freshly-installed STATE so the snapshot carries the accumulator vector for `H_snap` (needed by floored rebuild, 4.1.11).

### 4.3.B Manifest + trust chain

**Step 4.3.3 — Manifest struct** (`src/sync/snapshot_manifest.h`):
```
{ version, uint32 H_snap, Hash256 block_hash (== n:H_snap),
  Hash256 state_root, uint64 weight (== cw:H_snap),
  Hash256 prefix_set_id (== SHA256 of kStatePrefixes text),
  uint32 chunk_size, uint32 chunk_count,
  Hash256 chunk_hashes[], Hash256 snapshot_hash (== merkle(chunk_hashes)),
  Hash256 state_merkle_root,     // NEW, red-team T1-6: merkle over byte-sorted STATE leaves
  uint64 total_bytes }
```
Not signed by any maker — any honest node reproduces it byte-identically. Trust flows: **founder-signed/hardcoded checkpoint → finalized block_hash → header.state_root committed in block H_snap → recomputed from dumped STATE.**

**Step 4.3.4 — Dual binding (red-team T1-6).** LtHash alone has only generalized-birthday second-preimage security, and in snapshot sync the ATTACKER supplies the state bytes. So at snapshot boundaries ONLY, additionally commit `state_merkle_root` = Merkle over byte-sorted STATE leaves, and the installer verifies BOTH `LtHash == header.state_root` AND `state_merkle_root` matches. (The cheap per-block path keeps using LtHash only — there the node computes the state itself, no attacker input.) Do NOT shrink the 1024×u16 lattice. **[CC]**

**Step 4.3.5 — Snapshots produced ONLY at checkpoint heights** so `H_snap == H_ckpt` and `block_hash == checkpoint.hash` → O(1) finality verification. General `H_snap < H_ckpt` fallback needs a header-ancestor proof (optional `snapshot.headers` verb).

### 4.3.C New-node fast sync

**Step 4.3.6 — New files `src/sync/snapshot_sync.{h,cpp}` + wire verbs.** Dispatch mirrors the `block.*` branch at **src/api/rats_api.cpp:716** via `type.rfind("snapshot.",0)==0`. Verbs:
- `snapshot.locate {}` → latest FINALIZED manifest + finalizing checkpoint `{height,hash,pubkey,sig}`. A pruned node returns "I am pruned, snapshot at H" (red-team T2-11 eclipse-by-omission) so the fresh node tries other peers instead of hanging.
- `snapshot.manifest {height}` → manifest.
- `snapshot.getchunk {snapshot_hash, index}` → `snapshot.chunk {snapshot_hash, index, bytes}` push.
- optional `snapshot.headers {from,to}`.
Multi-source chunk fetch mirrors `block.getdata` fan-out with `kMaxGetdataInFlight` bound; verify each chunk `sha256 == chunk_hashes[index]` BEFORE writing; cap chunk size + `total_bytes`; reject manifests with inconsistent `prefix_set_id`/`chunk_count` (red-team T3-18). **[CC on verify, SAFE on transport]**

**Step 4.3.7 — Fresh-node driver (`Chain::install_snapshot`):**
1. `block.hello` → learn peer tip/weight.
2. `snapshot.locate` → manifest + checkpoint.
3. Verify checkpoint EXACTLY like `apply_checkpoint` (founder sig) OR against `hardcoded_checkpoints()`, and `H_ckpt >= H_snap`. **Require the checkpoint to match a HARDCODED entry for genesis-distance sync** (red-team T1-7); treat founder-signed-only checkpoints as advisory above the hardcoded floor.
4. Reject manifest if `prefix_set_id != SHA256(kStatePrefixes)` (schema mismatch).
5. Fetch + verify chunks; `install_state_chunk` each.
6. Seed the H_snap index rows carried in the manifest: `t:tip=(block_hash,H_snap)`, `n:/h:/cw:/k:` for H_snap.
7. Compute `sr:vec` from installed state (`state_vec_from_dump`), persist `sr:vec`+`sr:height=H_snap`, set `committed_acc_`.
8. **Integrity gate:** recompute `state_root` over installed STATE with the SAME `StateAccumulator::leaf` fold AND the Merkle root; assert both `== manifest.state_root`/`state_merkle_root == header.state_root of block H_snap`.
9. Set `replay_floor_ = H_snap`.
10. Catch up `H_snap+1..tip` via existing `block.getblocks/getdata/apply_loop`; each block re-checks `header.state_root` incrementally on the snapshot base.
11. **CRITICAL:** never call the `replay_floor_==0` rebuild path (would wipe snapshot state). Enforced by 4.1.11. **[CC]**

**Step 4.3.8 — rats_api.cpp:716 : on empty-DB fresh start where a peer advertises a much-taller tip, choose snapshot-sync over replay-from-1.**

### 4.3.D Settlement bodies above H_snap

**Step 4.3.9 (red-team T3-16):** `apply_settlement_mint` needs the `sb:<root>` companion body (node-local, not in STATE snapshot). During the H_snap+1..tip tail, ensure the existing `request_settle_body`/`MC_SETTLE_BODY_GET` path fires; block connect WAITS for the body, does not fail. **[SAFE]**

---

## PHASE 4.4 — Pruning  **[SAFE — purely local storage; CC only on the horizon math]**

**Step 4.4.1 — Horizon = `min(tip - FINALITY_DEPTH, latest finalized H_snap, highest checkpoint ≤ that)`.** `SNAPSHOT_INTERVAL` must stay comfortably below `FINALITY_DEPTH=1000` so a finalized snapshot always sits at/below the horizon (red-team open-risk). Set `SNAPSHOT_INTERVAL = 500`.

**Step 4.4.2 — database.{h,cpp} : `prune_below(uint32_t horizon, Chain& chain)`.**
- PRUNABLE (`h < horizon`): `b:<hash>` full bodies (bulk win), `k:<height>` checksums, `sb:<root>` settlement bodies referenced ONLY by blocks at/below H_snap.
- MUST-KEEP all heights (~60 B/block): `n:<h>→hash`, `h:<hash>→height`, `cw:<hash>→weight`, `t:tip`, `cp:` checkpoints — needed for `build_locator`, `hashes_after_locator`, fork-choice, `checkpoint_ok`, serving hash lists.
- MUST-KEEP live: entire STATE set, latest finalized snapshot artifact + manifest, `sr:vec`.
- MUST-KEEP recent window `(tip - FINALITY_DEPTH, tip]`: full `b:/k:/sb:` for reorg (`disconnect_block`/`reorg_to_branch` pull old bodies) + near-tip serving.

**Step 4.4.3 — Set `replay_floor_ = horizon` on prune** so no rebuild path ever replays absent bodies.

**Step 4.4.4 — Require ≥ N archival/snapshot-serving peers before enabling pruning** (red-team T2-11), and keep `block_propagator.cpp:454` returning a clean `notfound` on pruned `b:` so a requesting peer falls through to snapshot sync instead of stalling.

**Step 4.4.5 — jsonrpc_server.cpp : add `node.snapshot_info {}` and `node.prune {}` ops verbs.** **[SAFE]**

---

## PHASE 5 — Two-tier mempool  **[CC on the inclusion rule + caps; SAFE on the hot index / verdict cache]**

### 5.A Consensus caps (version-gated)

**Step 5.1 — candidate.h:44 (near PROPAGATION_WINDOW_MS) : add**
```
static constexpr uint32_t MAX_TXS_PER_BLOCK  = 20000;
static constexpr uint64_t MAX_BLOCK_BYTES    = 8u*1024*1024;   // supersedes/aligns with block.h MAX_BLOCK_SIZE (2 MiB) — RECONCILE: bump MAX_BLOCK_SIZE too
static constexpr uint32_t MAX_TXS_PER_SENDER_PER_BLOCK = 256;  // anti-starvation, deterministic
static constexpr uint32_t N_HOT_MAX = ...;                     // node-local knob, NOT consensus
```
These are consensus rules → pinned as shared constants with a **cutover height** (red-team T2-8/T3-17), same discipline as BLOCK_VERSION. Under the clean-slate fork (D3) the cutover is genesis.

**Step 5.2 — block.cpp:220 `Block::validate()` : add stateless cap enforcement** — reject if `transactions.size() > MAX_TXS_PER_BLOCK` OR `serialize().size() > MAX_BLOCK_BYTES`. Runs on connect_block AND rebuild → no node accepts an over-cap block. **[CC]**

### 5.B The canonical inclusion pipeline (pin the order — red-team T2-8)

The consensus-visible inclusion is a pure function of replicated inputs, computed identically everywhere, in THIS exact order:
```
1. SELECT eligible E:
     song block:  all pending tx with submit_ms <= reg.submit_ms
     heartbeat:   all pending tx with (now - submit_ms) >= PROPAGATION_WINDOW_MS
2. TOTAL-order sort by (sender, nonce, tx_hash)                  [candidate.cpp:435-439, unchanged]
3. APPLICABILITY gate: drop first-unappliable until clean        [semantics of candidate.cpp:489-500]
4. Anti-starvation: enforce MAX_TXS_PER_SENDER_PER_BLOCK (deterministic per-sender cap)
5. TAKE prefix up to MAX_TXS_PER_BLOCK and MAX_BLOCK_BYTES
6. block_ts = max(submit_ms) over the FINAL capped+gated set     [candidate.cpp:506-510, recompute from final set]
```
Every node computes `block_ts` from the identical final set (kills the cap-boundary/block_ts fork). Overflow stays in HOT/COLD and drains next block; the sort key is total+stable so the next build continues the canonical prefix. **[CC]**

### 5.C Two-tier structure

**Step 5.3 — COLD tier unchanged:** the durable, gossiped, min-merged LevelDB mempool (`p:`/`pt:`, `sp:`). Source of truth + replication + restart substrate. Flood/min-merge/30s window untouched.

**Step 5.4 — New files `src/consensus/mempool_index.{h,cpp}` : HOT tier `MempoolIndex`** (owned by CandidateManager). In-memory ordered set keyed `(sender,nonce,tx_hash)` of txs that are (a) past the 30s window, (b) sig-verified, (c) appliability-projected clean. Per-sender cursor `{next_nonce, projected_balance}`; per-sender parked out-of-order map. API: `promote(meta)`, `take_for_block(chain, boundary_or_now, cap) → capped canonical prefix`, `on_block_connected(senders_touched)`, `rebuild_from_disk(db)`. Node-local perf index — never gossiped, never in block bytes, excluded from root, rebuildable from `p:`. **[SAFE]** (consensus outcome identical whether HOT or naive scan is used).

**Step 5.5 — Conservative projected-balance (red-team T2-9).** At promote, never admit a tx whose cumulative per-sender projected debit exceeds committed balance (per-sender in-flight-debit cap). Bound the commit-failure backstop (candidate.cpp:594-607) to K iterations, then drop the whole sender's tail. Keep the backstop as the safety net — correctness still rests on connect_block's real apply, so any projection error is liveness not safety. Measure backstop fire-rate under adversarial flood. **[CC-adjacent: measure]**

### 5.D Verify-once verdict cache

**Step 5.6 — database.cpp put/get `vc:<tx_hash> = {ok, sender, nonce, type}`** and a `pi:<be64 submit_ms><tx_hash>` submit-ms-ordered index. `put_pending_tx`/`del_pending_tx` (database.cpp:301-311) write/delete `pi:` alongside; add `get_matured_since(cursor,limit)` range scan over `pi:`. Both `pi:`/`vc:` are node-local mempool-class.

**Step 5.7 — database.cpp:1029-1030 : add `pi:` and `vc:` to the node-local EXCLUDE set** (never cleared by rebuild, never in root), alongside `p:/pt:/sp:`. Confirm they are NOT in `kStatePrefixes`.

**Step 5.8 — rats_api.cpp ingest_tx (~3408, after `tx_preflight_ok` passes) : write `vc:` + `pi:`.** Min-merge/flood unchanged. Promotion to HOT happens later in the maturation pass, not at ingest.

**Step 5.9 — apply path sig-skip (advisory).** In `apply_transfer` (chain.cpp:209) and other `apply_*`, consult `vc:` to SKIP the Ed25519 verify on a hit; miss → full verify + populate. Stateful nonce/balance/authority checks ALWAYS run. **Never skip on the node's own block-BUILD first inclusion** (red-team T3-14) — skip only on re-apply/rebuild; CRC the `vc:` entry; periodic re-verify sampling. **[CC — advisory cache must not weaken validation]**

### 5.E Wiring into the producer

**Step 5.10 — candidate.cpp:225-235 (30s wake) : COLD→HOT maturation pass.** Advance the `pi:` cursor to `(now - PROPAGATION_WINDOW_MS)`, promote newly-matured txs into HOT (sig from `vc:`, nonce-contiguity + conservative projected-balance), park out-of-order. O(newly-matured), not O(mempool). The now-vs-submit_ms gate is the SAME comparison already accepted as fork-choice-resolved (red-team open-risk: do not introduce a new clock divergence).

**Step 5.11 — candidate.cpp:269-510 : replace `get_all_pending_txs()` full scan + per-tx submit_ms get + per-tx deserialize/verify + the O(n²) per-build applicability gate with ONE `MempoolIndex.take_for_block(chain, boundary_or_now, cap)`** returning the capped, sorted, applicability-projected, per-sender-capped set. Keep the commit-failure backstop (594-607) VERBATIM as the rare-race net. Keep `block_ts` semantics per 5.B step 6. **[CC on inclusion equivalence]**

**Step 5.12 — chain.cpp:187-192 drain loop : also `del_batch pi:<..>` and `vc:<..>` for each included tx; after commit call `MempoolIndex.on_block_connected(senders_touched)`.** `senders_touched` MUST be enumerated from APPLIED OUTPUTS (mint recipients, settlement recipients, relay recipient — NOT `tx.from_address`, since MINT/SETTLEMENT/RELAY have no conventional sender), red-team T2-10; on any doubt invalidate the whole HOT (rebuildable). On reorg the same per-sender invalidation runs during replay. **[CC-adjacent]**

**Step 5.13 — HOT `rebuild_from_disk` on startup must be bounded/streamed** (promote-lazily-on-first-build, not eager full rebuild) so a restart under peak load doesn't stall production (red-team open-risk). **[SAFE]**

---

## CONSENSUS-CRITICAL SET (adversarial pass required)

4.0.1-4.0.5 (STATE_PREFIXES + audit hook), all of 4.1 (accumulator, helper unification, boot reconstruction, floor-aware rebuild, header v4), 4.1.5 (chromaprint float), 4.3.4/4.3.7 (dual binding + install verify + hardcoded-checkpoint floor), 4.4.1 (horizon math), 5.1/5.2/5.B (caps + inclusion pipeline), 5.9/5.12 (advisory cache + invalidation).

## SAFE SET

4.2 (bootstrap inclusion preference), 4.3.6 transport, 4.4.5 ops verbs, 5.4/5.6/5.13 (HOT index plumbing, `vc:`/`pi:` storage, streamed rebuild).

## BUILD ORDER (dependencies)

1. **4.0** (constant + self-register delete + audit assertion) — everything else depends on the single STATE set.
2. **4.1** (accumulator + helper + header v4 + boot + floor-aware rebuild) — must land atomically as the clean-slate fork; the audit assertion from 4.0.5 guards it in CI.
3. **4.1.5** (chromaprint) — must precede any live v4 chain.
4. **4.2** (bootstrap) — needed to actually mine post-fork.
5. **4.3 → 4.4** (snapshots then pruning; pruning depends on finalized snapshots + `replay_floor_` from 4.1.11).
6. **5** (mempool) — independent of 4.3/4.4 but shares the cutover/fork from 4.1; ship in the same version bump to avoid a second consensus fork.

## RESOLVED RED-TEAM CONTRADICTIONS

- **T0-1 (v:/va: contradiction):** adopt design-1 (`v:` on-chain, in root, self-register deleted); drop design-2's `va:` rewrite. `apply_node_auth` already writes the pubkey to `v:`. Also route its revoke-path `batch.Delete` through `del_batch` (4.1.3).
- **T0-2 (pruned reorg brick):** floor-aware, non-clearing rebuild seeded from snapshot `sr:vec` (4.1.11/4.1.12).
- **T0-3 (partial write-set stamp):** single `apply_block_writes` helper used by produce/connect/rebuild (4.1.5).
- **T0-4 (undefined boot acc):** scan-and-sum reconstruction when `sr:vec` missing or height-mismatched (4.1.8).

# RED-TEAM

I've read the load-bearing paths (connect_block/apply_transactions at chain.cpp:108-361, rebuild_derived_state at 1592-1718, the reorg restore at 1560-1582, clear_derived_state at database.cpp:1034-1058, put_batch/del_batch at 81-88, check_play/recompute_mint/validate_mint at mint.cpp:130-218, and candidate inclusion at 428-534). Here is the ranked red-team.

---

# RED-TEAM: reward-onchain-scale Phases 4–5

Ranking is by blast radius: **T0** = designs are mutually incompatible or self-wedge as written; **T1** = remote-triggerable fork or fund/trust compromise; **T2** = divergence that fork-choice papers over (liveness/perf); **T3** = local/correctness/DoS. Each item: subsystem · mechanism · why it breaks · mitigation.

## T0 — Design incoherence / self-wedge (fix before coding)

**1. The two subsystems make OPPOSITE v:/va: decisions. [state_root ↔ snapshot coupling]**
Subsystem-1 explicitly PICKS "make v: purely on-chain, add v: to STATE_PREFIXES, add v: to clear, INCLUDE v: in the root, delete the node_main.cpp:430 self-register." Subsystem-2 explicitly says "a snapshot EXCLUDES v: (node-local self-register makes it non-deterministic) … route check_play/validate_mint to read va:." These cannot both ship. If v: is on-chain-and-in-root (design 1), it is a STATE prefix, so it IS in the snapshot and check_play (mint.cpp:149) and recompute_mint (mint.cpp:175) keep working unchanged — design-2's va: rewrite is not just unnecessary, it's wrong (va: currently stores an empty marker; check_play needs the 33-byte pubkey, which lives in v:). **Mitigation:** adopt design-1 wholesale (v: on-chain, in root, self-register deleted); DELETE design-2's va:-pubkey migration; snapshot dumps v: with the rest of the STATE set. Reconcile the `kStatePrefixes`/`STATE_PREFIXES` constant so it contains v: in exactly one place both subsystems import.

**2. A pruned or snapshot-synced node that reorgs calls rebuild_derived_state → irrecoverable state loss + wrong root. [snapshot ↔ state_root ↔ reorg]**
reorg restore (chain.cpp:1579) unconditionally calls `rebuild_derived_state()`, whose first act is `db_.clear_derived_state()` (chain.cpp:1615) — a prefix scan that wipes ALL state keys at ALL heights, then replays h=1..tip. Design-2 correctly bans replay-from-1 via `replay_floor_ = H_snap` and starts the loop at floor+1. But clear_derived_state is height-blind: it deletes the snapshot's <floor state too, and the accumulator is zeroed at rebuild start (design-1: "reset committed_acc_ to all-zero"). Replaying only floor+1..tip therefore (a) leaves <floor state permanently deleted and (b) produces `committed_acc_` = sum of leaves floor+1..tip only, missing every <floor leaf → root ≠ header.state_root at the first replayed v4 block → truncate-to-genesis → node bricks. **Mitigation:** rebuild on a floored node must NOT clear+replay-from-zero. It must re-install the snapshot state (or keep it) and seed `committed_acc_` from the snapshot's persisted accumulator vector, then replay floor+1..tip on top. Persist the accumulator vector INSIDE the snapshot manifest/chunks (a new `sr:vec` leaf is not enough — the snapshot must carry the committed vector for height H_snap). Make clear_derived_state take a floor and refuse to clear below it, or make rebuild call a `resync_from_floor()` variant that never clears.

**3. compute_candidate_state_root must reproduce the FULL connect_block write set, not just apply_transactions. [state_root producer stamp]**
connect_block writes the song indexes OUTSIDE apply_transactions: put_fingerprint / put_song_meta / add_to_artist_index / add_to_genre_index / set_content_height at chain.cpp:158-163 (all STATE prefixes f:/sm:/ia:/ig:/bh:). Design-1's `compute_candidate_state_root` "trial-applies the block" — if it only runs apply_transactions (like first_unappliable_tx does), it misses those five index leaves. The producer then stamps a root that its own connect_block will recompute WITH the index leaves → self-mismatch → the producer rejects the block it just built → chain never advances on song blocks. **Mitigation:** compute_candidate_state_root must run the identical write sequence as connect_block (the has_song index block + apply_transactions + the sp: delete at :169), against the throwaway batch. Factor the connect_block body into one `apply_block_writes(block, height, batch, acc)` helper called by BOTH the stamp path and the real connect_block, so they cannot diverge.

**4. Boot / upgrade path with no sr:vec leaves committed_acc_ undefined. [state_root persistence]**
Design-1: "restart loads committed_acc_ from sr:vec (no replay)." On the very first v4 boot (upgrade from a v3 DB, or a snapshot install that didn't write sr:vec), sr:vec is absent. If the loader defaults committed_acc_ to zero, the next block's incremental root = leaf(delta) only, but a peer that DID a full rebuild has the true root → the upgraded node and fresh nodes disagree forever. **Mitigation:** on boot, if sr:vec is missing OR its stored height ≠ tip height, reconstruct committed_acc_ by a full one-time scan-and-sum over kStatePrefixes (same function the snapshot dump uses), then persist sr:vec. Store the height alongside the vector and assert it equals tip on every boot.

## T1 — Remote fork / trust compromise

**5. STATE_PREFIXES ↔ clear_derived_state drift, and any raw db.put on a STATE key. [state_root accumulator]**
The accumulator hooks put_batch/del_batch (database.cpp:81-88) and fires on has_state_prefix. clear_derived_state (database.cpp:1058) uses `db_->Write` directly, bypassing the hook (intended). The invariant "live-apply accumulator == rebuild accumulator" holds ONLY if every STATE-prefix write goes through put_batch AND the hook-set == clear-set. Two concrete break vectors in the current tree: (a) the v: boot self-register (node_main.cpp:430) is a raw db.put on a root-included prefix — captured by design-1's deletion, must actually be deleted. (b) The moderation/label prefixes (ha:/hb:/ht:/d:/fr:/ml:/ms:/label:/art_label:) are written by apply_moderator_op through put_batch but are deliberately EXCLUDED from both root and clear "until audited." If any of them is later added to the root without also being in clear (or vice versa), the next rebuild yields a different set → root mismatch → mass truncation network-wide. **Mitigation:** single `kStatePrefixes` constant, `static_assert`/unit test that (hook-set) == (clear-set) == (union of prefixes any apply_* writes). Add a debug build mode that, after each connect_block, does a full scan-sum and asserts == incremental committed_acc_ — this catches a stray raw put or a missed prefix immediately in CI instead of at a production reorg.

**6. LtHash second-preimage lets a malicious snapshot server serve wrong-but-valid-root state. [snapshot sync]**
The whole snapshot trust chain terminates in "recompute LtHash over the dumped STATE == manifest.state_root == header.state_root (checkpoint-pinned)." LtHash is a lattice/additive multiset hash: it has only ~generalized-birthday second-preimage security tied to the 1024×u16 lanes, strictly weaker than a Merkle binding. In connect_block this is harmless (the root is a checksum of state the node computed itself — an attacker can't feed it a different multiset). But in snapshot sync the attacker DOES supply the state bytes. If they can find any STATE multiset S' ≠ S with the same LtHash as the checkpointed root, install_snapshot's integrity gate passes and the new node boots on forged balances. **Mitigation:** at snapshot boundaries only, additionally commit a Merkle root over the byte-sorted STATE leaves and put it in the manifest; verify BOTH (LtHash for the cheap per-block path, Merkle for the attacker-supplied snapshot path). Do NOT shrink the 1024×u16 lattice. Alternatively bind the snapshot to `snapshot_hash` = merkle(chunk_hashes) AND require it equal a value committed on-chain (a SnapshotCommitTx), not merely derivable.

**7. The founder checkpoint key is the sole trust root for fast-sync; compromise = total new-node takeover. [snapshot sync ↔ checkpoints]**
snapshot.locate returns a manifest finalized by a founder-signed CheckpointTx or hardcoded_checkpoints(). A new node verifies only that signature. If the founder key leaks (or hardcoded list is stale and the fallback is "trust any founder-signed checkpoint"), an attacker signs a checkpoint over a fabricated block_hash + a matching fabricated snapshot (its own state_root) and owns every node that fast-syncs. This is strictly more dangerous than today because pre-Phase-4 nodes replay-verify from genesis (no such shortcut). **Mitigation:** ship hardcoded_checkpoints() with the binary and require snapshot finality to match a HARDCODED checkpoint height/hash for genesis-distance sync, not just any valid founder signature; treat founder-signed checkpoints as advisory above the hardcoded floor. Consider N-of-M founder keys for checkpoints.

**8. MAX_TXS cap + heartbeat block_ts recomputation create a new maturity-frontier fork surface. [mempool cap ↔ block_ts]**
Heartbeat block_ts = max included submit_ms, recomputed from the PRUNED set (candidate.cpp:506-510). Adding MAX_TXS_PER_BLOCK means the included set is now `canonical_sort → take(cap)`. If node A computes block_ts over the pre-cap set and node B over the post-cap set — or they cap before vs after the applicability gate — the header timestamp (which is in the block-hash preimage) differs for the same tx content → different block hash → fork. Worse: a tx that is mature on A but immature on B (the acknowledged ~skew residual) can now be the marginal tx AT the cap boundary, changing which txs make the prefix on one node but not the other. Existing code tolerates this via fork-choice for the small edge; a hard cap magnifies it because a single boundary tx shifts the entire tail in/out. **Mitigation:** define one canonical pipeline order and pin it in the shared spec: (1) select eligible (maturity/submit_ms), (2) applicability gate, (3) canonical sort, (4) take(cap, MAX_BLOCK_BYTES), (5) block_ts = max submit_ms of the FINAL capped+gated set. Every node computes block_ts from the identical final set. Version-gate the caps.

## T2 — Divergence that fork-choice resolves (liveness/perf)

**9. HOT projected-balance approximation disagrees with real apply → divergent candidates + O(n²) backstop DoS. [Phase-5 hot tier]**
Design-5 replaces the exact per-build applicability gate (first_unappliable_tx, candidate.cpp:489-500) with a linear per-sender projected-balance check at promote time. A sender with several mature txs interleaved with incoming transfers can have a real balance the linear projection mis-estimates, so a tx enters HOT that fails balance at connect_block. take_for_block then returns a body that won't connect → falls to the retained backstop, which is exactly the O(n²) trial-apply the design was trying to delete. Under a crafted flood this forces repeated evict-one-rebuild cycles at 15k tx/s. Two nodes with slightly different HOT projection states also emit different candidate bodies. **Mitigation:** make the projection conservative (per-sender in-flight debit cap; never promote a tx whose cumulative projected debit exceeds committed balance), and bound the backstop to K iterations then drop the whole sender's tail. Accept that HOT is an optimization: correctness still rests on connect_block's real apply, so divergence is liveness not safety — but measure backstop fire-rate under adversarial load.

**10. on_block_connected must enumerate sender=0 / issuer for MINT/SETTLEMENT/RELAY_REWARD or HOT silently drops senders. [Phase-5 invalidation]**
The connect_block drain loop (chain.cpp:187-192) currently deletes p:/pt: by tx_hash. Design-5 extends it to invalidate HOT cursors for `senders_touched`. MINT (sender=0/nonce=0, chain.cpp:283), SETTLEMENT_MINT, and RELAY_REWARD don't have a conventional from_address; if senders_touched misses them, the recipients' projected_balance goes stale and their subsequent transfers get dropped from HOT until restart — a slow, node-local liveness leak that makes different nodes' HOT sets drift. **Mitigation:** enumerate touched addresses from the APPLIED outputs (mint recipients, settlement recipients, relay recipient), not from tx.from_address; on any doubt, invalidate the whole HOT (it's rebuildable from p:).

**11. All-pruned network = new nodes cannot join (eclipse-by-omission). [pruning ↔ sync]**
Pruning drops block bodies below the horizon; snapshot sync depends on at least one peer serving a finalized manifest+chunks. If every reachable peer has pruned and none serves snapshots (or a partition withholds snapshot.* while answering block.hello), a fresh node can neither replay-from-1 (bodies gone) nor fast-sync → it stalls with no error distinguishable from "still catching up." **Mitigation:** require a minimum number of archival/snapshot-serving peers before a node enables pruning; have snapshot.locate return a signed "I am pruned, snapshot at H" so a fresh node can detect omission and try other peers; keep the getdata notfound path (block_propagator.cpp:454) returning clean notfound so the driver falls through to snapshot sync rather than hanging.

**12. Float chromaprint uniqueness gate (0.70) is a pre-existing cross-platform fork landmine the root design now makes fatal. [rebuild ↔ block validity]**
rebuild_derived_state step 3 (chain.cpp:1657) rejects a song block whose fingerprint is ≥ kChromaprintSimThreshold (0.70) similar to an already-replayed one — a floating-point comparison. If that similarity is computed with float math that differs across compilers/arch (x87 vs SSE, fast-math, FMA contraction), one node accepts the song block and another rejects it as a duplicate → different canonical chain → different state_root. The committed root doesn't cause this, but it converts a soft "which chain wins" into a hard root mismatch that truncates the losing node. **Mitigation:** make the similarity comparison integer/fixed-point and platform-invariant (quantize the score, compare integers), and add it to the determinism audit alongside the accumulator. This is independent of Phase 4/5 but must be closed before a committed root ships.

## T3 — Local correctness / DoS

**13. fph: written-but-never-cleared → duplicate-song false positives on a reorg branch. [rebuild hygiene]**
Design-1's own edit note flags it: put_fingerprint writes fph: but clear_derived_state never removes it, so an abandoned reorg branch's fph: rows persist and can false-positive the fuzzy dup check on the winning branch. Excluded from the root (derivable from f:), so no root divergence — but a real correctness bug. **Mitigation:** add fph: to the clear set (not the root set).

**14. Verdict-cache false-positive wastes a local block; self-corrects at the network. [Phase-5 vc:]**
vc:=ok is only written after a real local verify, so peers can't poison it. A memory/disk bit-flip that flips a vc: verdict, combined with apply-time sig-skip, could let a bad-sig tx into a block THIS node builds; peers re-verify (cache miss) and orphan it. No consensus break, but wasted production and a possible wedge if the corrupt node is the only producer. **Mitigation:** CRC/hash the vc: entry; periodic re-verify sampling; never skip verify on the block-BUILD path for the node's own first inclusion (skip only on re-apply/rebuild).

**15. Bootstrap mint gap: v: empty until the founder NodeAuthTx confirms. [v: decision fallout]**
With the self-register deleted (design-1), the founder node cannot validate its own plays until its self-authorizing NodeAuthTx mines. If that tx is starved (mempool full at 15k/s, or the founder's own producer never includes it), the node can never mint — a chicken-and-egg. **Mitigation:** privilege the founder NodeAuthTx for guaranteed inclusion in the next block it produces (it's tx-only, no mint dependency); keep the 30s retry; hardcode the founder validator entry as a genesis-time on-chain NodeAuthTx so v: is populated at height 1.

**16. Settlement bodies (sb:) above H_snap are absent from a snapshot node → block stall. [snapshot ↔ settlement]**
apply_settlement_mint needs the sb:<root> companion body, which is node-local and not in the STATE snapshot. A snapshot node catching up past H_snap stalls on the first SETTLEMENT_MINT until it lazily fetches the body. Matches current IBD behavior but must be wired. **Mitigation:** ensure the request_settle_body / MC_SETTLE_BODY_GET path fires during snapshot-tail catch-up; block connect waits, doesn't fail.

**17. Caps are new consensus rules + low-address spam starvation. [Phase-5 caps]**
Different builds shipping different MAX_TXS/MAX_BLOCK_BYTES fork exactly like a BLOCK_VERSION mismatch — needs a coordinated cutover height. Separately, the (sender,nonce,tx_hash) sort means a spammer at a low address perpetually occupies the canonical prefix under sustained load, starving others (liveness, inherited from today's ordering but newly visible with a cap). **Mitigation:** version-gate caps with a cutover height; add a deterministic per-sender inclusion cap per block for anti-starvation.

**18. Chunk-transfer resource exhaustion. [snapshot sync]**
snapshot.getchunk/chunk without bounds lets a peer advertise a huge chunk_count/total_bytes and exhaust a syncing node's memory/disk. **Mitigation:** cap chunk size and total snapshot bytes, verify each chunk's sha256 before writing, bound in-flight chunks (mirror kMaxGetdataInFlight), reject manifests with inconsistent prefix_set_id/chunk_count.

---

## Cross-cutting must-fix summary
1. Reconcile the v:/va: contradiction (item 1) — this is the single most load-bearing incoherence; pick design-1's "v: on-chain, in root" and drop design-2's va: rewrite.
2. Make rebuild floor-aware AND state-preserving on pruned/snapshot nodes (item 2) — the reorg path will brick pruned nodes as written.
3. Unify the connect_block write set behind one helper used by the producer stamp, connect_block, and rebuild (item 3) so the root can never self-mismatch.
4. Add a debug scan-sum-vs-incremental assertion (item 5) — it is the cheapest possible guard against the entire class of accumulator drift bugs and will catch items 5, 12, and any future prefix mistake in CI.

Files that carry the load-bearing invariants for these fixes: `C:\Users\lain\blockchain\bopwire\src\core\chain.cpp` (connect_block 108-206, apply_transactions 237-361, reorg restore 1560-1582, rebuild 1592-1718), `C:\Users\lain\blockchain\bopwire\src\storage\database.cpp` (put_batch/del_batch 81-88, clear_derived_state 1034-1058), `C:\Users\lain\blockchain\bopwire\src\tokens\mint.cpp` (check_play/recompute_mint 130-198 — the v: readers), and `C:\Users\lain\blockchain\bopwire\src\consensus\candidate.cpp` (inclusion + block_ts 428-534).