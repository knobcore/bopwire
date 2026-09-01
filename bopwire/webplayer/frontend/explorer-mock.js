/* Bopwire explorer — mock gateway.
 *
 * The explorer API (blocks/tx/address/song/stats/search) is landing on the
 * gateway route-by-route. This file is a deterministic in-browser stand-in
 * that answers the SAME contract, so the UI can be built and tested end to
 * end before (and independently of) the backend. Enable with ?mock=1.
 *
 * Everything is generated from seeded PRNGs, so a given height/address/hash
 * always produces the same data. Mock hashes encode their identity in the
 * leading hex (height / height+index / song index) so any 64-hex string we
 * emitted can be decoded again without keeping an index — good enough for a
 * mock, obviously not how real hashes work.
 */
(() => {
  'use strict';

  // ── seeded PRNG ──────────────────────────────────────────
  const mulberry32 = (a) => () => {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  const hexFrom = (rng, n) => {
    let s = '';
    while (s.length < n) s += ((rng() * 0xffffffff) >>> 0).toString(16).padStart(8, '0');
    return s.slice(0, n);
  };

  // ── chain shape ──────────────────────────────────────────
  const TIP        = 4321;
  const BLOCK_MS   = 90_000;
  const NOW        = Date.now();
  const GENESIS_MS = NOW - TIP * BLOCK_MS;
  const T          = 100_000_000;          // 1 token = 1e8 internal units
  const blockTime  = (h) => GENESIS_MS + h * BLOCK_MS;

  // identity-encoding hashes (see header comment)
  const blockHash = (h) => h.toString(16).padStart(8, '0') + hexFrom(mulberry32(h * 7 + 1), 56);
  const txHash    = (h, i) => h.toString(16).padStart(8, '0') + i.toString(16).padStart(2, '0')
                              + hexFrom(mulberry32(h * 131 + i * 17 + 5), 54);
  const nodeId    = (i) => 'ee' + i.toString(16).padStart(6, '0') + hexFrom(mulberry32(9000 + i), 56);

  // ── cast: addresses, songs ───────────────────────────────
  const addr = (i) => '0x' + i.toString(16).padStart(4, '0') + hexFrom(mulberry32(400 + i), 36);
  const ARTISTS   = [0, 1, 2, 3].map(addr);
  const LISTENERS = [4, 5, 6, 7, 8, 9].map(addr);
  const SEEDERS   = [10, 11, 12].map(addr);
  const RELAYS    = [13, 14, 15].map(addr);
  const FOUNDER   = addr(99);
  const ALL_ADDRS = [...ARTISTS, ...LISTENERS, ...SEEDERS, ...RELAYS, FOUNDER];

  const ARTIST_NAMES = ['Neon Casket', 'Marrow & Pine', 'DJ Substrate', 'The Quiet Volts'];
  const GENRES = ['synthwave', 'folk', 'techno', 'post-rock'];
  const SONGS = [
    ['Chrome Hearts',        0, 'Night Drive',   2024, 1, 212_000],
    ['Sodium Lights',        0, 'Night Drive',   2024, 2, 187_000],
    ['Terminal Bloom',       0, 'Night Drive',   2024, 3, 243_000],
    ['Riverbed Hymn',        1, 'Undergrowth',   2023, 1, 265_000],
    ['Cedar and Smoke',      1, 'Undergrowth',   2023, 2, 231_000],
    ['Packet Loss',          2, 'Handshake EP',  2025, 1, 324_000],
    ['Four on the Wire',     2, 'Handshake EP',  2025, 2, 298_000],
    ['Low Tide Telemetry',   3, 'Signal Fires',  2024, 1, 402_000],
    ['Antenna Field',        3, 'Signal Fires',  2024, 2, 356_000],
    ['Last Transmission',    3, 'Signal Fires',  2024, 3, 377_000],
  ].map(([title, ai, album, year, track, dur], i) => ({
    idx: i,
    content_hash: 'f5' + i.toString(16).padStart(6, '0') + hexFrom(mulberry32(700 + i), 56),
    title, album, year, track,
    artist: ARTIST_NAMES[ai],
    artist_address: ARTISTS[ai],
    genre: GENRES[ai],
    duration_ms: dur,
    reg_height: 137 + i * 173,
  }));
  const songByHash = new Map(SONGS.map((s) => [s.content_hash, s]));

  const fpB64 = (song) => {
    const rng = mulberry32(3000 + song.idx);
    let bin = '';
    for (let i = 0; i < 180; i++) bin += String.fromCharCode((rng() * 256) | 0);
    return btoa(bin);
  };

  // ── moderation + ratings (the real gateway shapes) ───────
  //
  // Mirrors /api/moderation, /api/moderation/hidden,
  // /api/moderation/moderator/:addr and the ratings routes exactly, INCLUDING
  // all five hide-provenance flavours so every rendering path can be seen:
  // founder hide, moderator hide, moderator-vote hide, downvote auto-hide,
  // and an item hidden with no attributable record ("origin unrecorded").
  const MOD1 = addr(50), MOD2 = addr(51), MOD3 = addr(52);
  const LEVEL_NAME = { 0: 'NONE', 1: 'VOICE', 2: 'OP', 3: 'FOUNDER' };
  const MOD_LEVEL_NOW = { [FOUNDER]: 3, [MOD1]: 2, [MOD3]: 1 };   // MOD2 revoked
  const modLevelNow = (a) => MOD_LEVEL_NOW[a] || 0;

  // The auto-hide rule in force on this mock chain (set by proposal, not the
  // compiled-in default — so the "source: chain" path is exercised).
  const RATING_POLICY_SET_H = 3000;
  const RATING_POLICY = {
    min_ratings: 10, down_ratio_bps: 7500, down_ratio_pct: '75.00',
    source: 'chain', set_height: RATING_POLICY_SET_H, set_by: FOUNDER,
    proposal_tx: proposalTxHash(RATING_POLICY_SET_H),
    defaults: { min_ratings: 20, down_ratio_bps: 8000 },
    bounds: { min_ratings_min: 5, min_ratings_max: 1000,
              down_ratio_bps_min: 5000, down_ratio_bps_max: 9500 },
    rule: 'hide when total_ratings >= min_ratings AND downvotes/total_ratings >= down_ratio_bps/10000',
    proposal_kind: 5,
  };

  // Per-song rating totals. Song 8 crosses the rule (27 of 30 down = 90%).
  const RATING_COUNTS = [
    { up: 42, down: 3 }, { up: 18, down: 1 }, { up: 27, down: 5 },
    { up: 9,  down: 0 }, { up: 14, down: 2 }, { up: 6,  down: 4 },
    { up: 11, down: 2 }, { up: 8,  down: 1 }, { up: 3,  down: 27 },
    { up: 21, down: 6 },
  ];
  // Individual rating txs, so RATING transactions actually appear in blocks.
  const RATING_EVENTS = [];
  (() => {
    let h = 3300;
    RATING_COUNTS.forEach((c, i) => {
      // Distinct raters per song: the chain allows one rating per wallet per
      // track, so the mock must never emit the same (wallet, track) twice.
      for (let k = 0; k < Math.min(3, c.up); k++)
        RATING_EVENTS.push({ h: h++, idx: i, value: 1, rater: LISTENERS[(k + i) % LISTENERS.length] });
      for (let k = 0; k < Math.min(2, c.down); k++)
        RATING_EVENTS.push({ h: h++, idx: i, value: 2, rater: LISTENERS[(k + i + 3) % LISTENERS.length] });
    });
  })();
  const ratingEventsAt = (h) => RATING_EVENTS.filter((e) => e.h === h);
  // The downvote that tipped song 8 over is its last rating event.
  const AUTOHIDE_IDX = 8;
  const autoHideEvent = RATING_EVENTS.filter((e) => e.idx === AUTOHIDE_IDX).slice(-1)[0];

  // Block-borne moderation txs: grants, revokes, proposals and votes.
  const BLOCK_MOD_EVENTS = [
    { h: 1,    op: 'grant',  level: 3, subject: FOUNDER, by: FOUNDER },
    { h: 777,  op: 'grant',  level: 2, subject: MOD1,    by: FOUNDER },
    { h: 1500, op: 'grant',  level: 2, subject: MOD2,    by: FOUNDER },
    { h: 2200, op: 'grant',  level: 1, subject: MOD3,    by: FOUNDER },
    { h: RATING_POLICY_SET_H, prop: 5, by: FOUNDER,
      amount: RATING_POLICY.min_ratings + RATING_POLICY.down_ratio_bps * 4294967296 },
    { h: 3900, op: 'revoke', level: 0, subject: MOD2,    by: FOUNDER },
    { h: 4150, prop: 1, by: MOD1, target_song: 7 },              // HIDE_CONTENT proposal
    { h: 4160, vote_for: 4150, by: MOD3 },                       // reaches quorum → executed
  ];
  // Gossip hide/unhide envelopes — signed by a moderator, replicated in the
  // mod log, never a block transaction (so they carry no tx hash).
  const GOSSIP_MOD_EVENTS = [
    { h: 2600, action: 'hide_hash',   value: () => SONGS[5].content_hash, by: MOD1 },
    { h: 2900, action: 'unhide_hash', value: () => SONGS[5].content_hash, by: MOD2 },
    { h: 3200, action: 'hide_hash',   value: () => SONGS[9].content_hash, by: MOD1 },
    { h: 3500, action: 'hide_artist', value: () => 'DJ Substrate',        by: MOD1 },
    { h: 4000, action: 'hide_album',  value: () => 'Undergrowth',         by: FOUNDER },
  ];
  // An item the chain shows hidden with NO attributable hide record. The
  // explorer must say "origin not recorded on chain" rather than guess.
  const ORPHAN_HIDDEN_TITLE = 'Riverbed Hymn';

  const modOpName = { 1: 'grant', 2: 'revoke', 3: 'tag_label_edit' };
  const propKindName = { 1: 'hide_content', 2: 'release_escrow', 3: 'vote_yes',
                         4: 'grant_moderator', 5: 'set_rating_threshold' };

  function blockModTxs(h, startIdx) {
    const out = [];
    for (const ev of BLOCK_MOD_EVENTS) {
      if (ev.h !== h) continue;
      const i = startIdx + out.length;
      if (ev.op) {
        out.push({ type: 'moderator_op', type_byte: 0x20, tx_hash: txHash(h, i),
                   op_code: ev.op === 'grant' ? 1 : 2, op_name: ev.op,
                   level: ev.level, subject: ev.subject,
                   subject_pubkey: '02' + hexFrom(mulberry32(h * 3 + i), 64),
                   proposer: ev.by,
                   proposer_pubkey: '03' + hexFrom(mulberry32(h * 5 + i), 64),
                   nonce: h, meta_json: '',
                   signature: hexFrom(mulberry32(h * 7 + i), 128) });
      } else if (ev.prop) {
        const j = { type: 'moderator_proposal', type_byte: 0x30, tx_hash: txHash(h, i),
                    kind: ev.prop, kind_name: propKindName[ev.prop],
                    target_hash: ev.target_song !== undefined
                      ? SONGS[ev.target_song].content_hash : '0'.repeat(64),
                    target_addr: '0x' + '0'.repeat(40),
                    amount: ev.amount || 0, proposer: ev.by,
                    proposer_pubkey: '03' + hexFrom(mulberry32(h * 11 + i), 64),
                    nonce: h, subject_pubkey: '00'.repeat(33),
                    signature: hexFrom(mulberry32(h * 13 + i), 128) };
        if (ev.prop === 5)
          j.rating_threshold = { min_ratings: RATING_POLICY.min_ratings,
                                 down_ratio_bps: RATING_POLICY.down_ratio_bps };
        out.push(j);
      } else if (ev.vote_for) {
        out.push({ type: 'moderator_proposal', type_byte: 0x30, tx_hash: txHash(h, i),
                   kind: 3, kind_name: 'vote_yes',
                   target_hash: proposalTxHash(ev.vote_for),
                   target_addr: '0x' + '0'.repeat(40), amount: 0, proposer: ev.by,
                   proposer_pubkey: '03' + hexFrom(mulberry32(h * 17 + i), 64),
                   nonce: h, subject_pubkey: '00'.repeat(33),
                   signature: hexFrom(mulberry32(h * 19 + i), 128) });
      }
    }
    return out;
  }
  // A proposal's tx hash is stable: it is the first mod tx in its block.
  function proposalTxHash(h) {
    const before = SONGS.some((s) => s.reg_height === h) ? 1 : 0;
    return txHash(h, before + (h > 0 && h % 500 === 0 ? 1 : 0));
  }

  function ratingTxs(h, startIdx) {
    return ratingEventsAt(h).map((e, k) => ({
      type: 'rating', type_byte: 0x80, tx_hash: txHash(h, startIdx + k),
      content_hash: SONGS[e.idx].content_hash,
      value: e.value, value_name: e.value === 1 ? 'up' : 'down',
      rater: e.rater, rater_pubkey: '02' + hexFrom(mulberry32(h * 23 + k), 64),
      nonce: h, signature: hexFrom(mulberry32(h * 29 + k), 128),
    }));
  }
  const autoHideHeight = () => (autoHideEvent ? autoHideEvent.h : 0);
  function autoHideTriggerTx() {
    if (!autoHideEvent) return '0'.repeat(64);
    const txs = blockTxs(autoHideEvent.h);
    const t = txs.find((x) => x.type === 'rating' &&
      x.content_hash === SONGS[AUTOHIDE_IDX].content_hash && x.value === 2);
    return t ? t.tx_hash : '0'.repeat(64);
  }

  // The proposal-vote provenance object the node attaches to a `vote` hide.
  function voteProposalJson() {
    return {
      proposal_tx_hash: proposalTxHash(4150), kind: 1, kind_name: 'hide_content',
      proposer: MOD1, proposed_height: 4150, timestamp_ms: blockTime(4150),
      yes_votes: 2,
      voters: [{ voter: MOD3, height: 4160, tx_hash: proposalTxHash(4160) }],
      executed: true, executed_height: 4160,
      active_moderators_now: 3, threshold_now: 2,
      target_content_hash: SONGS[7].content_hash,
    };
  }

  // Every moderation action, chain order (ascending height).
  function modActionsChrono() {
    const rows = [];
    const push = (a) => rows.push(Object.assign({
      signer_is_founder: a.moderator === FOUNDER,
      signer_mod_level_now: modLevelNow(a.moderator),
    }, a));
    for (const ev of BLOCK_MOD_EVENTS) {
      const txs = blockTxs(ev.h);
      if (ev.op) {
        const tx = txs.find((t) => t.type === 'moderator_op');
        push({ kind: ev.op, category: 'moderator', value: ev.subject,
               moderator: ev.by, ts_ms: blockTime(ev.h), height: ev.h,
               source: 'block', tx_hash: tx && tx.tx_hash, level: ev.level,
               id: tx && tx.tx_hash });
      } else if (ev.prop) {
        const tx = txs.find((t) => t.type === 'moderator_proposal' && t.kind === ev.prop);
        const row = { kind: ev.prop === 1 ? 'proposal_hide' : 'proposal_rating_threshold',
                      category: ev.prop === 1 ? 'content' : 'rating_policy',
                      value: ev.prop === 1 ? SONGS[ev.target_song].content_hash
                                           : String(ev.amount),
                      moderator: ev.by, ts_ms: blockTime(ev.h), height: ev.h,
                      source: 'block', tx_hash: tx && tx.tx_hash, id: tx && tx.tx_hash };
        if (ev.prop === 1) row.proposal = voteProposalJson();
        push(row);
      } else if (ev.vote_for) {
        const tx = txs.find((t) => t.type === 'moderator_proposal' && t.kind === 3);
        push({ kind: 'vote_yes', category: 'proposal', value: proposalTxHash(ev.vote_for),
               moderator: ev.by, ts_ms: blockTime(ev.h), height: ev.h,
               source: 'block', tx_hash: tx && tx.tx_hash, id: tx && tx.tx_hash });
      }
    }
    for (const ev of GOSSIP_MOD_EVENTS) {
      const us = ev.action.indexOf('_');
      const sig = hexFrom(mulberry32(ev.h * 61 + 3), 128);
      push({ kind: ev.action.slice(0, us), category: ev.action.slice(us + 1),
             value: ev.value(), moderator: ev.by, ts_ms: blockTime(ev.h),
             height: ev.h, source: 'gossip', sig, id: sig });
    }
    if (autoHideEvent)
      push({ kind: 'hide', category: 'hash', value: SONGS[AUTOHIDE_IDX].content_hash,
             moderator: '0x' + '0'.repeat(40), ts_ms: blockTime(autoHideHeight()),
             height: autoHideHeight(), source: 'rating',
             tx_hash: autoHideTriggerTx(), id: autoHideTriggerTx() });
    rows.sort((a, b) => a.height - b.height);
    return rows;
  }
  const modActionsNewestFirst = () => modActionsChrono().reverse();

  function modCounts() {
    const c = { hides: 0, unhides: 0, grants: 0, revokes: 0, label_edits: 0 };
    for (const a of modActionsChrono())
      if (a.kind === 'hide') c.hides++;
      else if (a.kind === 'unhide') c.unhides++;
      else if (a.kind === 'grant') c.grants++;
      else if (a.kind === 'revoke') c.revokes++;
      else if (a.kind === 'label_edit') c.label_edits++;
    return c;
  }
  const activeModerators = () => Object.keys(MOD_LEVEL_NOW).length;

  // Provenance for one hidden item, in the node's own shape. Mirrors
  // ExplorerIndex::provenance_json_locked: latest gossip hide wins; else an
  // executed HIDE_CONTENT proposal; else the rating auto-hide record; else
  // "unknown". `reason` is ALWAYS "unrecorded" — the envelope has no reason
  // field, so a takedown-driven hide is indistinguishable from any other.
  function provenanceFor(category, value) {
    const vlc = String(value).toLowerCase();
    const hides = modActionsChrono().filter((a) =>
      a.source === 'gossip' && a.kind === 'hide' &&
      a.category === category && String(a.value).toLowerCase() === vlc);
    const hide = hides[hides.length - 1];
    if (hide)
      return { reason: 'unrecorded',
               by: hide.moderator === FOUNDER ? 'founder' : 'moderator',
               moderator: hide.moderator, mod_level_now: modLevelNow(hide.moderator),
               ts_ms: hide.ts_ms, height: hide.height, sig: hide.sig };
    if (category === 'hash') {
      if (value === SONGS[7].content_hash)
        return { reason: 'unrecorded', by: 'vote', proposal: voteProposalJson() };
      if (value === SONGS[AUTOHIDE_IDX].content_hash)
        return { reason: 'unrecorded', by: 'ratings', height: autoHideHeight(),
                 ratings: { up: RATING_COUNTS[AUTOHIDE_IDX].up,
                            down: RATING_COUNTS[AUTOHIDE_IDX].down,
                            total: RATING_COUNTS[AUTOHIDE_IDX].up +
                                   RATING_COUNTS[AUTOHIDE_IDX].down },
                 trigger_tx: autoHideTriggerTx(),
                 threshold_in_force: {
                   min_ratings: RATING_POLICY.min_ratings,
                   down_ratio_bps: RATING_POLICY.down_ratio_bps,
                   down_ratio_pct: RATING_POLICY.down_ratio_pct,
                   set_height: RATING_POLICY.set_height, set_by: RATING_POLICY.set_by } };
    }
    return { reason: 'unrecorded', by: 'unknown' };
  }

  // Individually hidden content hashes (the d: table), replayed.
  function hiddenHashes() {
    const set = new Set();
    for (const a of modActionsChrono()) {
      if (a.category !== 'hash') continue;
      if (a.kind === 'hide') set.add(a.value);
      if (a.kind === 'unhide') set.delete(a.value);
    }
    set.add(SONGS[7].content_hash);          // executed HIDE_CONTENT proposal
    return set;
  }
  function hiddenBucket(category) {
    const out = [];
    const seen = new Set();
    for (const a of modActionsChrono()) {
      if (a.source !== 'gossip' || a.category !== category) continue;
      if (a.kind === 'hide') seen.add(a.value); else seen.delete(a.value);
    }
    if (category === 'title') seen.add(ORPHAN_HIDDEN_TITLE);   // no hide record
    for (const v of seen) out.push({ value: v, category, provenance: provenanceFor(category, v) });
    return out;
  }
  function hiddenPayload() {
    const hashes = [...hiddenHashes()].map((v) => {
      const s = songByHash.get(v);
      return { value: v, category: 'hash', title: s ? s.title : undefined,
               artist: s ? s.artist : undefined, provenance: provenanceFor('hash', v) };
    });
    const body = { artists: hiddenBucket('artist'), albums: hiddenBucket('album'),
                   titles: hiddenBucket('title'), hashes };
    body.counts = { artists: body.artists.length, albums: body.albums.length,
                    titles: body.titles.length, hashes: body.hashes.length };
    return body;
  }
  const isSongHashHidden = (h) => hiddenHashes().has(h);

  // ── ratings read model ───────────────────────────────────
  function ratingCounts(hash) {
    const s = songByHash.get(hash);
    const c = s ? RATING_COUNTS[s.idx] : { up: 0, down: 0 };
    return { up: c.up, down: c.down, total: c.up + c.down, score: c.up - c.down };
  }
  function ratingHideRecord(hash) {
    const s = songByHash.get(hash);
    if (!s || s.idx !== AUTOHIDE_IDX) return null;
    const c = RATING_COUNTS[AUTOHIDE_IDX];
    return {
      height: autoHideHeight(), up: c.up, down: c.down, total: c.up + c.down,
      trigger_tx: autoHideTriggerTx(),
      threshold_in_force: { min_ratings: RATING_POLICY.min_ratings,
                            down_ratio_bps: RATING_POLICY.down_ratio_bps,
                            down_ratio_pct: RATING_POLICY.down_ratio_pct,
                            set_height: RATING_POLICY.set_height,
                            set_by: RATING_POLICY.set_by },
      currently_hidden: isSongHashHidden(hash), moderator_unhidden: false,
    };
  }
  function ratingsGet(hash, address) {
    const c = ratingCounts(hash);
    const body = { content_hash: hash, up: c.up, down: c.down,
                   total: c.total, score: c.score };
    if (address) {
      const mine = RATING_EVENTS.find((e) =>
        SONGS[e.idx].content_hash === hash && e.rater.toLowerCase() === address.toLowerCase());
      body.address = address;
      body.my_rating = mine ? (mine.value === 1 ? 'up' : 'down') : null;
      body.can_rate = LISTENERS.some((l) => l.toLowerCase() === address.toLowerCase());
    }
    body.hidden = isSongHashHidden(hash);
    body.rating_hide = ratingHideRecord(hash);
    body.threshold = RATING_POLICY;
    return body;
  }
  function ratingsByAddress(address, offset, limit) {
    const low = address.toLowerCase();
    const mine = RATING_EVENTS.filter((e) => e.rater.toLowerCase() === low)
      .sort((a, b) => b.h - a.h);
    const rows = mine.map((e) => {
      const s = SONGS[e.idx], c = RATING_COUNTS[e.idx];
      return { content_hash: s.content_hash, value: e.value === 1 ? 'up' : 'down',
               title: s.title, artist: s.artist, up: c.up, down: c.down };
    });
    return { address, total: rows.length, offset, limit,
             ratings: rows.slice(offset, offset + limit) };
  }

  // ── per-block transactions ───────────────────────────────
  const pick = (rng, arr) => arr[(rng() * arr.length) | 0];

  function mintTx(h, i, rng) {
    const song     = pick(rng, SONGS.filter((s) => s.reg_height <= h)) || SONGS[0];
    const listener = pick(rng, LISTENERS);
    const seeder   = pick(rng, SEEDERS);
    const relay    = pick(rng, RELAYS);
    const end      = blockTime(h) - ((rng() * 30_000) | 0);
    const burn     = h > 2000 && rng() < 0.5 ? (0.05 * T) | 0 : 0;
    return {
      type: 'mint', hash: txHash(h, i),
      proof: {
        session_id: hexFrom(mulberry32(h * 977 + i), 64),
        content_hash: song.content_hash,
        block_hash: blockHash(Math.max(0, h - 3)),
        artist_address: song.artist_address,
        player_address: listener,
        serving_node_id: nodeId((rng() * 3) | 0),
        play_start_timestamp: end - song.duration_ms,
        play_end_timestamp: end,
        total_duration_ms: song.duration_ms,
        heartbeat_count: Math.max(2, (song.duration_ms / 30_000) | 0),
        seeder_address: seeder,
        mini_node_address: relay,
        version: 3,
      },
      outputs: [
        { address: song.artist_address, amount: 1 * T, lane: 'artist' },
        { address: listener,            amount: 1 * T, lane: 'listener' },
        { address: seeder,              amount: 1 * T, lane: 'seeder' },
        { address: relay,               amount: 1 * T, lane: 'relay' },
      ],
      burn_amount: burn,
    };
  }

  function transferTx(h, i, rng) {
    const from = pick(rng, ALL_ADDRS), to = pick(rng, ALL_ADDRS.filter((a) => a !== from));
    return {
      type: 'transfer', hash: txHash(h, i),
      from_address: from, to_address: to,
      amount: ((rng() * 40 + 0.5) * T) | 0,
      nonce: (rng() * 300) | 0,
      from_pubkey: '02' + hexFrom(mulberry32(h * 31 + i), 64),
      signature: hexFrom(mulberry32(h * 37 + i), 128),
    };
  }

  function songRegTx(h, i, song) {
    return {
      type: 'song_register', hash: txHash(h, i),
      content_hash: song.content_hash,
      compressed_fingerprint: fpB64(song),
      artist_address: song.artist_address,
      royalty_splits: [{ address: song.artist_address, percent: 100 }],
      title: song.title, artist: song.artist, album: song.album,
      genre: song.genre, year: song.year, track: song.track,
      duration_ms: song.duration_ms,
    };
  }

  function blockTxs(h) {
    const rng = mulberry32(h * 2654435761 + 3);
    const txs = [];
    const reg = SONGS.find((s) => s.reg_height === h);
    if (reg) txs.push(songRegTx(h, txs.length, reg));
    if (h > 0 && h % 500 === 0)
      txs.push({ type: 'node_auth', hash: txHash(h, txs.length),
                 founder_address: FOUNDER, node_pubkey: '03' + hexFrom(mulberry32(h), 64),
                 node_id: nodeId(h % 3), action: 'authorize' });
    for (const t of blockModTxs(h, txs.length)) txs.push(t);
    for (const t of ratingTxs(h, txs.length)) txs.push(t);
    const nMints = h < 200 ? 0 : (rng() * 4.4) | 0;
    for (let m = 0; m < nMints; m++) txs.push(mintTx(h, txs.length, rng));
    if (rng() < 0.3) txs.push(transferTx(h, txs.length, rng));
    return txs;
  }

  // Every tx carries both `tx_hash` (the gateway's key) and `hash` (the key
  // the older mock rows used) so either lookup path resolves.
  const _blockTxs = blockTxs;
  blockTxs = function (h) {
    return _blockTxs(h).map((t) => (t.hash && t.tx_hash) ? t
      : Object.assign({}, t, { hash: t.hash || t.tx_hash, tx_hash: t.tx_hash || t.hash }));
  };

  function blockSummary(h) {
    const txs = blockTxs(h);
    const size = 220 + txs.length * 610;
    return { height: h, hash: blockHash(h), timestamp_ms: blockTime(h),
             tx_count: txs.length, weight: size * 4, size_bytes: size };
  }

  function fullBlock(h) {
    const txs = blockTxs(h);
    const size = 220 + txs.length * 610;
    return {
      height: h, hash: blockHash(h), prev_hash: h ? blockHash(h - 1) : '0'.repeat(64),
      merkle_root: hexFrom(mulberry32(h * 51 + 7), 64),
      timestamp_ms: blockTime(h),
      producer_node_id: nodeId(h % 3),
      version: 3, chain_id: 'bopwire-main',
      tx_count: txs.length, weight: size * 4, size_bytes: size,
      signature: hexFrom(mulberry32(h * 53 + 11), 128),
      transactions: txs,
    };
  }

  // decode identity-encoding hashes
  const decodeBlockHash = (hx) => {
    const h = parseInt(hx.slice(0, 8), 16);
    return Number.isInteger(h) && h >= 0 && h <= TIP && blockHash(h) === hx ? h : null;
  };
  const decodeTxHash = (hx) => {
    const h = parseInt(hx.slice(0, 8), 16), i = parseInt(hx.slice(8, 10), 16);
    if (!(Number.isInteger(h) && h >= 0 && h <= TIP)) return null;
    return txHash(h, i) === hx ? { h, i } : null;
  };

  // ── address + song aggregates (scan a recent window) ─────
  function txTouches(tx, a) {
    const roles = [];
    if (tx.type === 'mint') {
      const p = tx.proof;
      if (p.player_address === a)    roles.push('listener');
      if (p.artist_address === a)    roles.push('artist');
      if (p.seeder_address === a)    roles.push('seeder');
      if (p.mini_node_address === a) roles.push('relay');
    } else if (tx.type === 'transfer') {
      if (tx.from_address === a) roles.push('sender');
      if (tx.to_address === a)   roles.push('recipient');
    } else if (tx.type === 'song_register' && tx.artist_address === a) roles.push('artist');
    else if (tx.type === 'node_auth' && tx.founder_address === a)      roles.push('node');
    return roles;
  }

  const SCAN_FROM = Math.max(0, TIP - 1200);   // aggregate window for mock stats

  function addressInfo(a) {
    let plays = { listener: 0, artist: 0, seeder: 0, relay: 0 };
    let earned = 0, burned = 0, txc = 0, first = null, last = null, bal = 0;
    for (let h = SCAN_FROM; h <= TIP; h++) {
      for (const tx of blockTxs(h)) {
        const roles = txTouches(tx, a);
        if (!roles.length) continue;
        txc++; last = h; if (first === null) first = h;
        for (const r of roles) if (r in plays) plays[r]++;
        if (tx.type === 'mint') {
          for (const o of tx.outputs) if (o.address === a) { earned += o.amount; bal += o.amount; }
          if (tx.proof.player_address === a) { burned += tx.burn_amount; bal -= tx.burn_amount; }
        } else if (tx.type === 'transfer') {
          if (tx.from_address === a) bal -= tx.amount;
          if (tx.to_address === a)   bal += tx.amount;
        }
      }
    }
    return {
      address: a, balance: Math.max(0, bal), escrow_balance: 0,
      nonce: (mulberry32(a.length * 7)() * 50) | 0, tx_count: txc,
      first_seen_height: first, last_seen_height: last,
      plays_as_listener: plays.listener, plays_as_artist: plays.artist,
      plays_as_seeder: plays.seeder, plays_as_relay: plays.relay,
      earned_total: earned, burned_total: burned,
    };
  }

  function addressHistory(a, offset, limit) {
    const items = [];
    for (let h = TIP; h >= SCAN_FROM; h--) {
      const txs = blockTxs(h);
      for (let i = txs.length - 1; i >= 0; i--) {
        const roles = txTouches(txs[i], a);
        for (const r of roles)
          items.push({ role: r, tx_hash: txs[i].hash, type: txs[i].type,
                       block_height: h, block_hash: blockHash(h),
                       timestamp_ms: blockTime(h),
                       content_hash: txs[i].type === 'mint' ? txs[i].proof.content_hash
                                   : txs[i].content_hash,
                       amount: txs[i].type === 'transfer' ? txs[i].amount
                             : txs[i].type === 'mint'
                               ? (txs[i].outputs.find((o) => o.address === a) || {}).amount
                               : undefined });
      }
    }
    return { total: items.length, history: items.slice(offset, offset + limit) };
  }

  function songMetrics(hash) {
    const song = songByHash.get(hash);
    if (!song) return null;
    let plays = 0, earned = 0; const listeners = new Set();
    for (let h = SCAN_FROM; h <= TIP; h++)
      for (const tx of blockTxs(h))
        if (tx.type === 'mint' && tx.proof.content_hash === hash) {
          plays++; listeners.add(tx.proof.player_address);
          earned += tx.outputs.find((o) => o.lane === 'artist')?.amount || 0;
        }
    const chash = song.content_hash;
    const hidden = isSongHashHidden(chash);
    const rc = ratingCounts(chash);
    const out = {
      content_hash: chash,
      song: {
        content_hash: chash, title: song.title, artist: song.artist,
        artist_address: song.artist_address, album: song.album,
        genre: song.genre, year: song.year, track_number: song.track,
        duration_ms: song.duration_ms, audio_format: 'mp3',
        compressed_fingerprint: fpB64(song),
        royalty_splits: [{ address: song.artist_address, percent: 100 }],
      },
      registration_height: song.reg_height,
      registration_block_hash: blockHash(song.reg_height),
      play_count: plays, unique_listeners: listeners.size, earned_total: earned,
      holders: [{ address: song.artist_address, online: true }],
      holders_online: 1,
      state: { play_count: plays, discoverer_address: LISTENERS[song.idx % LISTENERS.length],
               first_play_block: song.reg_height + 3,
               first_play_timestamp: blockTime(song.reg_height + 3) },
      ratings: {
        up: rc.up, down: rc.down, total: rc.total, score: rc.score,
        auto_hide: ratingHideRecord(chash),
        moderator_unhidden: false,
        threshold_in_force: { min_ratings: RATING_POLICY.min_ratings,
                              down_ratio_bps: RATING_POLICY.down_ratio_bps,
                              source: RATING_POLICY.source,
                              set_height: RATING_POLICY.set_height,
                              set_by: RATING_POLICY.set_by },
      },
      hidden,
    };
    if (hidden) out.hidden_provenance = provenanceFor('hash', chash);
    return out;
  }

  function songPlays(hash, offset, limit) {
    const rows = [];
    for (let h = TIP; h >= SCAN_FROM; h--)
      for (const tx of blockTxs(h))
        if (tx.type === 'mint' && tx.proof.content_hash === hash)
          rows.push({ player_address: tx.proof.player_address,
                      seeder_address: tx.proof.seeder_address,
                      relay_address: tx.proof.mini_node_address,
                      serving_node_id: tx.proof.serving_node_id,
                      block_height: h, timestamp_ms: tx.proof.play_end_timestamp,
                      tx_hash: tx.hash, total_duration_ms: tx.proof.total_duration_ms });
    return { total: rows.length, plays: rows.slice(offset, offset + limit) };
  }

  function topRank(kind, n, sinceMs) {
    const counts = new Map();
    for (let h = SCAN_FROM; h <= TIP; h++) {
      if (sinceMs && blockTime(h) < sinceMs) continue;
      for (const tx of blockTxs(h))
        if (tx.type === 'mint') {
          const p = tx.proof;
          const key = kind === 'songs' ? p.content_hash
                    : kind === 'artists' ? p.artist_address : p.player_address;
          counts.set(key, (counts.get(key) || 0) + 1);
        }
    }
    return [...counts.entries()].sort((a, b) => b[1] - a[1]).slice(0, n)
      .map(([id, plays]) => {
        const e = { id, plays };
        if (kind === 'songs') { e.content_hash = id;
          const s = songByHash.get(id); if (s) { e.name = s.title; e.artist = s.artist; } }
        else { e.address = id;
          if (kind === 'artists') {
            const i = ARTISTS.indexOf(id); if (i >= 0) e.name = ARTIST_NAMES[i];
          } }
        return e;
      });
  }

  // ── stats ────────────────────────────────────────────────
  function stats() {
    const days = 30, series = [];
    for (let d = days - 1; d >= 0; d--) {
      const rng = mulberry32(60_000 + d);
      const t = NOW - d * 86_400_000;
      const ramp = (days - d) / days;
      series.push({ date: new Date(t).toISOString().slice(0, 10),
                    plays: (40 + ramp * 220 + rng() * 60) | 0,
                    mints: (38 + ramp * 205 + rng() * 55) | 0 });
    }
    let mints = 0, transfers = 0, regs = 0, other = 0, burned = 0;
    const listeners = new Set(), artists = new Set(), seeders = new Set();
    for (let h = SCAN_FROM; h <= TIP; h++)
      for (const tx of blockTxs(h)) {
        if (tx.type === 'mint') {
          mints++; burned += tx.burn_amount;
          listeners.add(tx.proof.player_address);
          artists.add(tx.proof.artist_address);
          seeders.add(tx.proof.seeder_address);
        } else if (tx.type === 'transfer') transfers++;
        else if (tx.type === 'song_register') regs++;
        else other++;
      }
    return {
      height: TIP, blocks: TIP + 1,
      txs_by_type: { mint: mints, transfer: transfers, song_register: SONGS.length,
                     node_auth: (TIP / 500) | 0, moderator_op: 1 },
      total_txs: mints + transfers + regs + other,
      total_plays: mints, unique_listeners: listeners.size,
      unique_artists: artists.size, unique_seeders: seeders.size,
      total_minted: mints * 4 * T, total_burned: burned,
      songs_registered: SONGS.length,
      per_day: series,
      moderation: (() => {
        const c = modCounts(), hid = hiddenPayload();
        return { ...c, actions_total: modActionsChrono().length,
                 active_moderators: activeModerators(),
                 hidden_artists: hid.counts.artists, hidden_albums: hid.counts.albums,
                 hidden_titles: hid.counts.titles, hidden_hashes: hid.counts.hashes };
      })(),
      window_note: `aggregates computed over blocks ${SCAN_FROM}–${TIP}`,
    };
  }

  function artistStats(id) {
    const a = ARTISTS.find((x) => x.toLowerCase() === id.toLowerCase())
           || ARTISTS[ARTIST_NAMES.findIndex((n) => n.toLowerCase() === id.toLowerCase())];
    if (!a) return null;
    const name = ARTIST_NAMES[ARTISTS.indexOf(a)];
    const per = new Map(), listeners = new Set(), overTime = new Map();
    const seedersServing = new Set(), relaysServing = new Set();
    let plays = 0, earned = 0;
    for (let h = SCAN_FROM; h <= TIP; h++)
      for (const tx of blockTxs(h))
        if (tx.type === 'mint' && tx.proof.artist_address === a) {
          plays++; listeners.add(tx.proof.player_address);
          seedersServing.add(tx.proof.seeder_address);
          relaysServing.add(tx.proof.mini_node_address);
          earned += tx.outputs.find((o) => o.lane === 'artist')?.amount || 0;
          per.set(tx.proof.content_hash, (per.get(tx.proof.content_hash) || 0) + 1);
          const day = new Date(blockTime(h)).toISOString().slice(0, 10);
          overTime.set(day, (overTime.get(day) || 0) + 1);
        }
    const perSong = [...per.entries()].map(([hash, n]) => {
      const s = songByHash.get(hash);
      return { content_hash: hash, title: s?.title || hash.slice(0, 12), plays: n };
    }).sort((x, y) => y.plays - x.plays);
    return {
      artist_address: a, name,
      total_plays: plays, unique_listeners: listeners.size, earned_total: earned,
      plays_per_song: perSong, top_songs: perSong.slice(0, 5),
      plays_over_time: [...overTime.entries()].sort()
        .map(([date, n]) => ({ date, plays: n })),
      serving_seeders: [...seedersServing], serving_relays: [...relaysServing],
    };
  }

  // ── search ───────────────────────────────────────────────
  function search(q) {
    const s = q.trim(), hits = [];
    if (/^\d+$/.test(s) && +s <= TIP) hits.push({ type: 'block', height: +s, hash: blockHash(+s) });
    const hx = s.replace(/^0x/i, '').toLowerCase();
    if (/^[0-9a-f]{64}$/.test(hx)) {
      const bh = decodeBlockHash(hx); if (bh !== null) hits.push({ type: 'block', height: bh, hash: hx });
      const th = decodeTxHash(hx);    if (th) hits.push({ type: 'tx', hash: hx, block_height: th.h });
      if (songByHash.has(hx)) {
        const sg = songByHash.get(hx);
        hits.push({ type: 'song', content_hash: hx, title: sg.title, artist: sg.artist });
      }
    }
    if (/^0x[0-9a-f]{40}$/i.test(s)) hits.push({ type: 'address', address: s });
    const low = s.toLowerCase();
    if (low.length >= 2) {
      for (const sg of SONGS)
        if (sg.title.toLowerCase().includes(low) || sg.album.toLowerCase().includes(low))
          hits.push({ type: 'song', content_hash: sg.content_hash, title: sg.title,
                      artist: sg.artist, album: sg.album });
      ARTIST_NAMES.forEach((n, i) => {
        if (n.toLowerCase().includes(low))
          hits.push({ type: 'artist', name: n, artist_address: ARTISTS[i] });
      });
      for (const g of GENRES)
        if (g.includes(low))
          for (const sg of SONGS.filter((x) => x.genre === g))
            hits.push({ type: 'song', content_hash: sg.content_hash, title: sg.title,
                        artist: sg.artist, genre: g });
      const hid = hiddenPayload();
      for (const bucket of ['artists', 'albums', 'titles', 'hashes'])
        for (const hd of hid[bucket])
          if (String(hd.value || '').toLowerCase().includes(low) ||
              String(hd.title || '').toLowerCase().includes(low))
            hits.push({ type: 'hidden', category: hd.category, value: hd.value,
                        title: hd.title, artist: hd.artist,
                        provenance: hd.provenance });
    }
    return { query: q, results: hits };
  }

  // ── mock router: same contract as the gateway ────────────
  // Returns {status, body} — body is an object (JSON routes) or string.
  function route(path) {
    const [p, qs] = path.split('?');
    const params = new URLSearchParams(qs || '');
    let m;

    if (p === '/api/blocks') {
      const offset = +(params.get('offset') || 0), limit = Math.min(+(params.get('limit') || 25), 100);
      const blocks = [];
      for (let h = TIP - offset; h > TIP - offset - limit && h >= 0; h--) blocks.push(blockSummary(h));
      return { status: 200, body: { blocks, total: TIP + 1 } };
    }
    if ((m = p.match(/^\/api\/blocks\/([^/]+)$/))) {
      const id = m[1];
      if (/^\d+$/.test(id)) return +id <= TIP ? { status: 200, body: fullBlock(+id) }
                                              : { status: 404, body: { error: 'no such height' } };
      const h = decodeBlockHash(id.replace(/^0x/i, '').toLowerCase());
      return h !== null ? { status: 200, body: fullBlock(h) }
                        : { status: 404, body: { error: 'unknown block' } };
    }
    if ((m = p.match(/^\/api\/tx\/([0-9a-fA-Fx]+)$/))) {
      const d = decodeTxHash(m[1].replace(/^0x/i, '').toLowerCase());
      if (!d) return { status: 404, body: { error: 'unknown tx' } };
      const tx = blockTxs(d.h)[d.i];
      return tx ? { status: 200, body: { ...tx, block_height: d.h, block_hash: blockHash(d.h),
                                         timestamp_ms: blockTime(d.h) } }
                : { status: 404, body: { error: 'unknown tx' } };
    }
    if ((m = p.match(/^\/api\/address\/(0x[0-9a-fA-F]{40})$/)))
      return { status: 200, body: addressInfo(m[1]) };
    if ((m = p.match(/^\/api\/address\/(0x[0-9a-fA-F]{40})\/history$/)))
      return { status: 200, body: addressHistory(m[1], +(params.get('offset') || 0),
                                                 Math.min(+(params.get('limit') || 25), 100)) };
    if (p === '/api/songs') {
      // catalog shape the player already consumes — reused as the Songs index
      return { status: 200, body: SONGS.map((s) => ({
        contentHash: s.content_hash, title: s.title, artist: s.artist, album: s.album,
        genre: s.genre, year: s.year, track: s.track, durationMs: s.duration_ms,
        plays: songMetrics(s.content_hash).play_count })) };
    }
    if (p === '/api/search') return { status: 200, body: search(params.get('q') || '') };
    if (p === '/api/stats')  return { status: 200, body: stats() };
    if (p === '/api/moderation') {
      const all = modActionsNewestFirst();            // newest first, like the gateway
      const offset = +(params.get('offset') || 0);
      const limit = Math.min(+(params.get('limit') || 25), 100);
      return { status: 200, body: {
        total: all.length, offset, limit,
        actions: all.slice(offset, offset + limit),
        counts: modCounts(), active_moderators: activeModerators() } };
    }
    if (p === '/api/moderation/hidden')
      return { status: 200, body: hiddenPayload() };
    if ((m = p.match(/^\/api\/moderation\/moderator\/((?:0x)?[0-9a-fA-F]{40})$/))) {
      const a = m[1].toLowerCase();
      const all = modActionsNewestFirst().filter((x) => x.moderator.toLowerCase() === a);
      const offset = +(params.get('offset') || 0);
      const limit = Math.min(+(params.get('limit') || 50), 200);
      const grant = modActionsChrono().find((x) =>
        x.kind === 'grant' && String(x.value).toLowerCase() === a);
      return { status: 200, body: {
        address: m[1], is_moderator: modLevelNow(m[1]) > 0,
        is_founder: m[1].toLowerCase() === FOUNDER.toLowerCase(),
        mod_level: modLevelNow(m[1]),
        active_since_height: grant ? grant.height : null,
        total: all.length, offset, limit,
        actions: all.slice(offset, offset + limit) } };
    }
    if (p === '/api/ratings/threshold')
      return { status: 200, body: { ...RATING_POLICY } };
    if ((m = p.match(/^\/api\/song\/([0-9a-fA-F]{64})\/ratings$/))) {
      const h = m[1].toLowerCase();
      if (!songByHash.has(h)) return { status: 404, body: { error: 'unknown song' } };
      return { status: 200, body: ratingsGet(h, params.get('address')) };
    }
    if ((m = p.match(/^\/api\/address\/((?:0x)?[0-9a-fA-F]{40})\/ratings$/)))
      return { status: 200, body: ratingsByAddress(m[1], +(params.get('offset') || 0),
                                                   Math.min(+(params.get('limit') || 200), 1000)) };
    if (p === '/api/top') {
      const kind = params.get('kind') || 'songs';
      if (!['songs', 'artists', 'listeners'].includes(kind))
        return { status: 400, body: { error: 'kind must be songs|artists|listeners' } };
      const since = params.get('since');
      return { status: 200, body: { kind,
        entries: topRank(kind, Math.min(+(params.get('n') || 10), 50),
                         since ? Date.parse(since + 'T00:00:00Z') : null) } };
    }
    if ((m = p.match(/^\/api\/song\/([0-9a-fA-F]{64})\/plays$/))) {
      if (!songByHash.has(m[1].toLowerCase())) return { status: 404, body: { error: 'unknown song' } };
      return { status: 200, body: songPlays(m[1].toLowerCase(),
        +(params.get('offset') || 0), Math.min(+(params.get('limit') || 100), 500)) };
    }
    if ((m = p.match(/^\/api\/stats\/artist\/(.+)$/))) {
      const st = artistStats(decodeURIComponent(m[1]));
      return st ? { status: 200, body: st } : { status: 404, body: { error: 'unknown artist' } };
    }
    if ((m = p.match(/^\/api\/song\/([0-9a-fA-F]{64})\/fingerprint$/))) {
      const sg = songByHash.get(m[1].toLowerCase());
      return sg ? { status: 200, body: fpB64(sg) } : { status: 404, body: { error: 'unknown song' } };
    }
    if ((m = p.match(/^\/api\/song\/([0-9a-fA-F]{64})$/))) {
      const rec = songMetrics(m[1].toLowerCase());
      return rec ? { status: 200, body: rec } : { status: 404, body: { error: 'unknown song' } };
    }
    return { status: 404, body: { error: 'mock: no such route ' + p } };
  }

  window.BOPWIRE_MOCK = { route, TIP };
})();
