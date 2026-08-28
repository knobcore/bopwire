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

  // ── moderation timeline (public chain data, derived from blocks) ──
  // Hides are how DMCA takedowns propagate: a moderator action goes on
  // chain and every node stops surfacing the target. Grants/revokes ride
  // ModeratorOpTx; hide/unhide ride moderation envelopes.
  const MOD1 = addr(50), MOD2 = addr(51), MOD3 = addr(52);
  const LEVEL_NAME = { 0: 'NONE', 1: 'VOICE', 2: 'OP', 3: 'FOUNDER' };
  const MOD_EVENTS = [
    { h: 1,    action: 'grant',  level: 3, subject: FOUNDER, by: FOUNDER },   // bootstrap self-grant
    { h: 777,  action: 'grant',  level: 2, subject: MOD1, by: FOUNDER },
    { h: 1500, action: 'grant',  level: 2, subject: MOD2, by: FOUNDER },
    { h: 2200, action: 'grant',  level: 1, subject: MOD3, by: FOUNDER },
    { h: 2600, action: 'hide',   target: { kind: 'title',  name: 'Packet Loss',
        content_hash: SONGS[5].content_hash, artist: 'DJ Substrate' }, by: MOD1 },
    { h: 2900, action: 'unhide', target: { kind: 'title',  name: 'Packet Loss',
        content_hash: SONGS[5].content_hash, artist: 'DJ Substrate' }, by: MOD2 },
    { h: 3200, action: 'hide',   target: { kind: 'title',  name: 'Last Transmission',
        content_hash: SONGS[9].content_hash, artist: 'The Quiet Volts' }, by: MOD1 },
    { h: 3500, action: 'hide',   target: { kind: 'artist', name: 'DJ Substrate' }, by: MOD1 },
    { h: 3900, action: 'revoke', level: 0, subject: MOD2, by: FOUNDER },
    { h: 4000, action: 'hide',   target: { kind: 'album',  name: 'Undergrowth',
        artist: 'Marrow & Pine' }, by: FOUNDER },
  ];

  function modTx(h, i, ev) {
    if (ev.action === 'grant' || ev.action === 'revoke')
      return { type: 'moderator_op', hash: txHash(h, i), action: ev.action,
               op_code: ev.action === 'grant' ? 1 : 2, level: ev.level,
               subject: ev.subject, proposer: ev.by };
    return { type: 'moderation', hash: txHash(h, i), action: ev.action,
             target: ev.target, moderator_address: ev.by };
  }

  const targetKey = (t) => `${t.kind}:${String(t.name || t.address || '').toLowerCase()}`;

  function modActions() {                       // chain order, with tx hashes
    return MOD_EVENTS.map((ev) => {
      const tx = blockTxs(ev.h).find((t) => t.type === 'moderator_op' || t.type === 'moderation');
      return { action: ev.action,
               target: ev.target || { kind: 'moderator', address: ev.subject, level: ev.level },
               level: ev.level, moderator_address: ev.by,
               block_height: ev.h, timestamp_ms: blockTime(ev.h), tx_hash: tx.hash };
    });
  }

  function hiddenNow() {                        // replay hides/unhides
    const map = new Map();
    for (const a of modActions()) {
      if (a.action === 'hide')   map.set(targetKey(a.target), a);
      if (a.action === 'unhide') map.delete(targetKey(a.target));
    }
    return [...map.values()].map((a) => ({
      kind: a.target.kind, name: a.target.name, artist: a.target.artist,
      content_hash: a.target.content_hash,
      hidden_at_height: a.block_height, timestamp_ms: a.timestamp_ms,
      moderator_address: a.moderator_address, tx_hash: a.tx_hash }));
  }

  function moderatorsNow() {                    // replay grants/revokes
    const map = new Map();
    for (const a of modActions()) {
      if (a.action === 'grant')
        map.set(a.target.address, { address: a.target.address, level: a.level,
                                    level_name: LEVEL_NAME[a.level] || String(a.level),
                                    granted_at_height: a.block_height,
                                    granted_by: a.moderator_address });
      if (a.action === 'revoke') map.delete(a.target.address);
    }
    return [...map.values()];
  }

  function songHidden(song) {                   // hidden via title, artist or album
    for (const h of hiddenNow()) {
      if (h.content_hash === song.content_hash) return h;
      if (h.kind === 'artist' && h.name === song.artist) return h;
      if (h.kind === 'album' && h.name === song.album && h.artist === song.artist) return h;
    }
    return null;
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
    for (const ev of MOD_EVENTS)
      if (ev.h === h) txs.push(modTx(h, txs.length, ev));
    const nMints = h < 200 ? 0 : (rng() * 4.4) | 0;
    for (let m = 0; m < nMints; m++) txs.push(mintTx(h, txs.length, rng));
    if (rng() < 0.3) txs.push(transferTx(h, txs.length, rng));
    return txs;
  }

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
    return {
      ...song, compressed_fingerprint: fpB64(song),
      registration_height: song.reg_height, registration_block: blockHash(song.reg_height),
      play_count: plays, unique_listeners: listeners.size, earned_total: earned,
      holders: [{ address: song.artist_address, percent: 100 }],
      hidden: songHidden(song),
    };
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
        const c = { hides: 0, unhides: 0, grants: 0, revokes: 0 };
        for (const a of modActions()) c[a.action + 's'] = (c[a.action + 's'] || 0) + 1;
        return { ...c, active_moderators: moderatorsNow().length,
                 hidden_now: hiddenNow().length };
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
      for (const hd of hiddenNow())
        if (String(hd.name || '').toLowerCase().includes(low))
          hits.push({ type: 'hidden', kind: hd.kind, name: hd.name, artist: hd.artist,
                      content_hash: hd.content_hash, hidden_at_height: hd.hidden_at_height,
                      moderator_address: hd.moderator_address, tx_hash: hd.tx_hash });
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
      const all = modActions();                       // chain order (ascending height)
      const offset = +(params.get('offset') || 0), limit = Math.min(+(params.get('limit') || 100), 200);
      return { status: 200, body: { actions: all.slice(offset, offset + limit), total: all.length } };
    }
    if (p === '/api/moderation/hidden')
      return { status: 200, body: { hidden: hiddenNow() } };
    if ((m = p.match(/^\/api\/moderation\/moderator\/(0x[0-9a-fA-F]{40})$/))) {
      const a = m[1].toLowerCase();
      const acts = modActions().filter((x) => x.moderator_address.toLowerCase() === a);
      const cur = moderatorsNow().find((x) => x.address.toLowerCase() === a);
      return { status: 200, body: {
        moderator: { address: m[1], level: cur?.level ?? 0,
                     level_name: cur?.level_name ?? 'NONE (not currently a moderator)' },
        actions: acts, total: acts.length } };
    }
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
