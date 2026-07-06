/* Bopwire web player — Discover. Vanilla JS, no framework.
 * Talks only to the gateway (window.BOPWIRE.gateway).
 *
 * Views:
 *   Home   — Spotify-shaped scroll of deterministic node-curated carousels
 *            (/api/collections: Rising, Top 50, New Releases, genres, years)
 *            plus genre tiles and a hero. Cover art is generated locally from
 *            each song's content hash — same integer algorithm as the native
 *            app, so both clients render identical art.
 *   Browse — the classic facet drill (Artist/Genre → Album → tracks), now fed
 *            by /api/facets + paged /api/songs so the full catalog is never
 *            shipped to the browser.
 *   List   — "See all" for a collection, or server-side search results.
 */
(() => {
  'use strict';
  const CFG = window.BOPWIRE;
  const $   = (id) => document.getElementById(id);

  // ───────────────────────── State ─────────────────────────
  const state = {
    view:     'home',      // 'home' | 'browse' | 'list'
    lastView: 'home',      // view to restore when search clears
    collections: null,     // /api/collections payload
    facets:      null,     // /api/facets payload

    // browse drill
    mode:  'artist',       // 'artist' | 'genre'
    path:  [],             // [{key,label}] (artist) or [{genre},{artist}]
    album: null,           // {key,label} selected album → track pane
    browseSongs:   [],     // songs fetched for the current drill facet
    browseLoading: false,

    // list view ("See all" / search)
    list:  null,           // {kicker,title,sub,songs,seed}
    query: '',

    queue:   [],           // current track context for auto-advance
    qIndex:  -1,
    playing: null,         // contentHash currently playing
    playId:  null,         // active reward session id (gateway)
  };

  // ─────────────────────── Utilities ───────────────────────
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g,
    (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const normKey = (s) => String(s ?? '').trim().toLowerCase().replace(/\s+/g, ' ');

  const fmtDur = (ms) => {
    const t = Math.max(0, Math.floor((ms || 0) / 1000));
    const m = Math.floor(t / 60), s = t % 60;
    return `${m}:${String(s).padStart(2, '0')}`;
  };
  const fmtPlays = (n) => {
    n = n || 0;
    if (n < 1000)   return `${n}`;
    if (n < 1e6)    return `${(n / 1e3).toFixed(1).replace(/\.0$/, '')}K`;
    return `${(n / 1e6).toFixed(1).replace(/\.0$/, '')}M`;
  };
  const avail = (s) => s.available !== false && (s.swarmSize || 0) >= 0; // membership is deterministic; only an explicit false dims

  let toastTimer = null;
  function toast(msg) {
    const t = $('toast');
    t.textContent = msg; t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, 3200);
  }

  // Group items by normalized key; label = most frequent original spelling.
  function bucketByNorm(items, keyFn) {
    const map = new Map();
    for (const it of items) {
      const raw = keyFn(it);
      const key = normKey(raw);
      let b = map.get(key);
      if (!b) { b = { key, items: [], spell: new Map() }; map.set(key, b); }
      b.items.push(it);
      const label = (raw ?? '').toString().trim();
      b.spell.set(label, (b.spell.get(label) || 0) + 1);
    }
    for (const b of map.values()) {
      let best = '', bestN = -1;
      for (const [sp, n] of b.spell) if (n > bestN) { best = sp; bestN = n; }
      b.label = best;
    }
    return map;
  }

  // ───────────────── Deterministic cover art ─────────────────
  // Integer-only algorithm shared verbatim with the Flutter client
  // (_CoverArtPainter in discover_screen.dart) — same seed bytes, same
  // hues, same motif geometry ⇒ identical art on both clients. Keep the
  // two implementations in lockstep.
  //
  // Song seed:  first 8 bytes of the content hash.
  // Name seed:  FNV-1a-32(name) ++ FNV-1a-32(name + '*'), big-endian bytes.
  function seedFromHash(hex) {
    const b = [];
    for (let i = 0; i < 8; i++) b.push(parseInt(String(hex).substr(i * 2, 2), 16) || 0);
    return b;
  }
  function fnv32(str) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
      h ^= str.charCodeAt(i) & 0xff;
      h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
  }
  function seedFromName(name) {
    const n = normKey(name);
    const a = fnv32(n), b = fnv32(n + '*');
    return [(a >>> 24) & 255, (a >>> 16) & 255, (a >>> 8) & 255, a & 255,
            (b >>> 24) & 255, (b >>> 16) & 255, (b >>> 8) & 255, b & 255];
  }
  function artParams(seed) {
    const h1    = ((seed[0] << 8) | seed[1]) % 360;
    const h2    = (h1 + 40 + (seed[2] % 200)) % 360;
    const angle = seed[3] % 360;
    const motif = seed[4] % 5;
    const n     = 3 + (seed[5] % 4);
    const rot   = (seed[6] % 4) * 90;
    return { h1, h2, angle, motif, n, rot, seed };
  }
  // 96×96 SVG string. All geometry integer math from the seed.
  function coverArt(seed, opts) {
    const p  = artParams(seed);
    const v  = (i) => p.seed[(i + 2) % 8];
    const bg1 = `hsl(${p.h1},45%,15%)`, bg2 = `hsl(${p.h2},60%,28%)`;
    const a1  = `hsl(${p.h1},75%,62%)`, a2  = `hsl(${p.h2},85%,60%)`;
    const alt = (i) => (i % 2 === 0 ? a1 : a2);
    let m = '';
    if (p.motif === 0) {            // concentric rings
      const step = Math.floor(36 / p.n);
      for (let i = 0; i < p.n; i++)
        m += `<circle cx="48" cy="48" r="${10 + i * step}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85"/>`;
    } else if (p.motif === 1) {     // bars
      const w = Math.floor(64 / p.n);
      for (let i = 0; i < p.n; i++) {
        const h = 18 + (v(i) % 52);
        m += `<rect x="${16 + i * w}" y="${48 - (h >> 1)}" width="${w - 4}" height="${h}" rx="3" fill="${alt(i)}" opacity=".85"/>`;
      }
    } else if (p.motif === 2) {     // nested diamonds
      const step = Math.floor(26 / p.n);
      for (let i = 0; i < p.n; i++) {
        const half = 34 - i * step;
        m += `<rect x="${48 - half}" y="${48 - half}" width="${half * 2}" height="${half * 2}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85" transform="rotate(45,48,48)"/>`;
      }
    } else if (p.motif === 3) {     // dot grid
      const sp = Math.floor(64 / (p.n - 1));
      for (let i = 0; i < p.n; i++)
        for (let j = 0; j < p.n; j++)
          m += `<circle cx="${16 + i * sp}" cy="${16 + j * sp}" r="${4 + (v(i + j) % 3)}" fill="${alt(i + j)}" opacity=".85"/>`;
    } else {                        // stacked triangles
      const step = Math.floor(24 / p.n);
      for (let i = 0; i < p.n; i++) {
        const s = 38 - i * step;
        m += `<polygon points="48,${48 - s} ${48 + s},${48 + s} ${48 - s},${48 + s}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85"/>`;
      }
    }
    const gid = 'g' + p.seed.map((x) => x.toString(16).padStart(2, '0')).join('');
    return `<svg viewBox="0 0 96 96" xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="xMidYMid slice"${opts && opts.attrs ? ' ' + opts.attrs : ''}>
      <defs><linearGradient id="${gid}" gradientTransform="rotate(${p.angle},.5,.5)">
        <stop offset="0" stop-color="${bg1}"/><stop offset="1" stop-color="${bg2}"/>
      </linearGradient></defs>
      <rect width="96" height="96" fill="url(#${gid})"/>
      <g transform="rotate(${p.rot},48,48)">${m}</g>
    </svg>`;
  }
  const songArt  = (s) => coverArt(seedFromHash(s.contentHash));
  const nameArt  = (name) => coverArt(seedFromName(name));
  // Genre tile background: the two hues of the same seed as a CSS gradient.
  function tileGradient(name) {
    const p = artParams(seedFromName(name));
    return `linear-gradient(${p.angle}deg, hsl(${p.h1},60%,30%), hsl(${p.h2},70%,42%))`;
  }

  // ─────────────────────── Gateway API ─────────────────────
  async function apiGet(path) {
    const r = await fetch(CFG.gateway + path, { mode: 'cors' });
    if (!r.ok) {
      let msg = `HTTP ${r.status}`;
      try { msg = (await r.json()).error || msg; } catch (_) {}
      throw new Error(msg);
    }
    return r.json();
  }
  const streamUrl = (hash) => `${CFG.gateway}/api/stream/${hash}`;

  async function apiPost(path, body) {
    const r = await fetch(CFG.gateway + path, {
      method: 'POST', mode: 'cors',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  }

  // Server-side search / facet page — the catalog never ships whole.
  async function fetchSongs(params) {
    const qs = new URLSearchParams(params).toString();
    const r  = await apiGet('/api/songs?' + qs);
    return Array.isArray(r) ? r : (r.songs || []);
  }

  // ───────────────────── View switching ────────────────────
  function showView(v) {
    state.view = v;
    if (v !== 'list') state.lastView = v;
    $('home').hidden   = v !== 'home';
    $('browse').hidden = v !== 'browse';
    $('list').hidden   = v !== 'list';
    document.querySelectorAll('#viewnav .vn-btn').forEach((b) =>
      b.setAttribute('aria-selected', String(b.dataset.view === v ||
        (v === 'list' && b.dataset.view === state.lastView))));
  }
  function renderView() {
    if (state.view === 'home')   renderHome();
    if (state.view === 'browse') renderBrowse();
    if (state.view === 'list')   renderList();
  }

  // ─────────────────────── Home render ─────────────────────
  function renderHome() {
    const cols = state.collections?.collections || [];
    const nonEmpty = cols.filter((c) => (c.songs || []).length > 0);
    $('home-empty').hidden = nonEmpty.length > 0;
    if (!nonEmpty.length) {
      $('hero').innerHTML = ''; $('genre-tiles').innerHTML = ''; $('rows').innerHTML = '';
      $('home-empty').textContent = state.collections
        ? 'The network hasn’t curated anything yet — check back soon.'
        : 'Loading the network’s picks…';
      return;
    }
    renderHero(nonEmpty);
    renderGenreTiles(nonEmpty);
    renderRows(nonEmpty);
    const meta = state.collections;
    if (meta && meta.contentDigest) {
      $('verify-line').hidden = false;
      $('verify-line').innerHTML =
        `curated deterministically by the network · epoch ${esc(meta.epoch)} · ` +
        `digest <code>${esc(String(meta.contentDigest).slice(0, 16))}…</code>`;
    }
  }

  function renderHero(cols) {
    const rising = cols.find((c) => c.kind === 'rising') || cols[0];
    const songs  = rising.songs || [];
    const idx    = Math.max(0, songs.findIndex((s) => avail(s)));
    const s      = songs[idx];
    if (!s) { $('hero').innerHTML = ''; return; }
    $('hero').innerHTML = `
      <div class="hero">
        <div class="hero-art">${songArt(s)}</div>
        <div class="hero-meta">
          <div class="hero-kicker">✦ Rising — every listen pays the artist in full</div>
          <div class="hero-title">${esc(s.title) || '(untitled)'}</div>
          <div class="hero-sub">${esc(s.artist)}${s.playCount ? ` · ${fmtPlays(s.playCount)} plays` : ''}</div>
          <div class="hero-actions">
            <button class="btn-play" id="hero-play">▶ Play</button>
            <button class="btn-ghost" id="hero-more">Explore Rising</button>
          </div>
        </div>
        <div class="hero-badge">under 10k plays</div>
      </div>`;
    $('hero-play').onclick = () => playCollection(rising, idx);
    $('hero-more').onclick = () => openCollection(rising);
  }

  function renderGenreTiles(cols) {
    const genres = cols.filter((c) => c.kind === 'genre');
    const el = $('genre-tiles');
    if (!genres.length) { el.innerHTML = ''; return; }
    el.innerHTML = `
      <div class="tiles-title">Genres</div>
      <div class="tiles">${genres.map((c, i) => `
        <div class="tile" data-i="${i}" style="background:${tileGradient(c.facet)}">
          <span class="tile-count">${(c.songs || []).length}</span>
          ${esc(c.title.replace(/^Best of /, ''))}
        </div>`).join('')}
      </div>`;
    el.querySelectorAll('.tile').forEach((t) => {
      t.onclick = () => openCollection(genres[+t.dataset.i]);
    });
  }

  function renderRows(cols) {
    // Genre rows live in the tile strip; carousels carry everything else
    // plus the genre rows again further down for the scrollers.
    const order = ['rising', 'top', 'new', 'year', 'genre'];
    const rows = [...cols].sort((a, b) =>
      order.indexOf(a.kind) - order.indexOf(b.kind));
    $('rows').innerHTML = rows.map((c, ri) => `
      <section class="carousel" data-ri="${ri}">
        <div class="row-head">
          <span class="row-title">${esc(c.title)}</span>
          <span class="row-sub">${esc(c.subtitle)}</span>
          <button class="row-more" data-ri="${ri}">See all ›</button>
        </div>
        <div class="row-scroll">${(c.songs || []).slice(0, 20).map((s, i) =>
          cardEl(s, ri, i)).join('')}
        </div>
      </section>`).join('');
    $('rows').querySelectorAll('.row-more').forEach((b) => {
      b.onclick = () => openCollection(rows[+b.dataset.ri]);
    });
    $('rows').querySelectorAll('.card').forEach((el) => {
      el.onclick = () => {
        const c = rows[+el.dataset.ri], i = +el.dataset.i;
        const s = c.songs[i];
        if (!avail(s)) { toast('No seeders online for this track right now.'); return; }
        playCollection(c, i);
      };
    });
  }

  function cardEl(s, ri, i) {
    const off     = !avail(s);
    const playing = state.playing === s.contentHash;
    const rising  = (s.playCount || 0) > 0 && (s.playCount || 0) < 10000;
    return `<div class="card${off ? ' off' : ''}${playing ? ' playing' : ''}" data-ri="${ri}" data-i="${i}">
      <div class="cover">${songArt(s)}
        <div class="cov-play">${playing ? '♫' : '▶'}</div>
      </div>
      ${off ? '<div class="off-chip">offline</div>' : ''}
      <div class="c-title">${esc(s.title) || '(untitled)'}</div>
      <div class="c-sub">${esc(s.artist)}</div>
      <div class="c-plays">${rising ? '<span class="rising-dot"></span>' : ''}${fmtPlays(s.playCount)} plays</div>
    </div>`;
  }

  // Play from a collection: the queue is the collection's ordered songs,
  // minus the ones with no seeders (kept visible, dimmed, but unplayable).
  function playCollection(c, idx) {
    const playable = (c.songs || []).filter((s) => avail(s));
    if (!playable.length) { toast('Nothing in this row is streamable right now.'); return; }
    const want = c.songs[idx];
    let qi = playable.findIndex((s) => s.contentHash === want.contentHash);
    if (qi < 0) qi = 0;
    state.queue = playable;
    playFromQueue(qi);
  }

  function openCollection(c) {
    state.list = {
      kicker: c.kind === 'genre' ? 'Genre' : c.kind === 'year' ? 'Year' : 'Collection',
      title:  c.title,
      sub:    `${c.subtitle ? c.subtitle + ' · ' : ''}${(c.songs || []).length} songs`,
      songs:  c.songs || [],
      seed:   c.facet ? seedFromName(c.facet) : seedFromName(c.id),
    };
    showView('list'); renderList();
  }

  // ─────────────────────── List render ─────────────────────
  function renderList() {
    const L = state.list;
    if (!L) { $('list-head').innerHTML = ''; $('list-tracks').innerHTML = ''; return; }
    const playable = L.songs.filter((s) => avail(s));
    $('list-head').innerHTML = `
      <div class="list-art">${coverArt(L.seed)}</div>
      <div class="list-meta">
        <div class="list-kicker">${esc(L.kicker)}</div>
        <div class="list-title">${esc(L.title)}</div>
        <div class="list-sub">${esc(L.sub)}</div>
        <div class="list-actions">
          <button class="btn-play" id="list-play" ${playable.length ? '' : 'disabled'}>▶ Play all</button>
          <button class="btn-ghost" id="list-shuffle" ${playable.length ? '' : 'disabled'}>🔀 Shuffle</button>
        </div>
      </div>
      <button class="btn-ghost list-back" id="list-back">← Back</button>`;
    $('list-back').onclick = () => { showView(state.lastView); renderView(); };
    $('list-play').onclick = () => { if (playable.length) { state.queue = playable; playFromQueue(0); } };
    $('list-shuffle').onclick = () => {
      if (!playable.length) return;
      state.queue = [...playable].sort(() => Math.random() - 0.5);
      playFromQueue(0);
    };
    $('list-empty').hidden = L.songs.length > 0;
    renderTrackList($('list-tracks'), L.songs, /*numbered=*/false, /*withArt=*/true);
  }

  // ────────────────────── Browse render ────────────────────
  function rootLabel() { return state.mode === 'artist' ? 'Artists' : 'Genres'; }

  function levelKind() {
    if (state.mode === 'artist') return state.path.length === 0 ? 'artist' : 'album';
    return ['genre', 'artist', 'album'][Math.min(state.path.length, 2)];
  }

  // Songs narrowed by the current drill path (within the fetched facet page).
  function songsForPath() {
    let arr = state.browseSongs;
    if (state.mode === 'genre' && state.path[1]) {
      arr = arr.filter((s) => normKey(s.artist) === state.path[1].key);
    }
    return arr;
  }

  const albumLabelOf = (s) => (s.album && s.album.trim()) ? s.album.trim() : 'Singles';

  function renderBrowse() {
    renderModeToggle();
    renderBreadcrumb();
    renderChips();
    renderTrackPane();
  }

  function renderModeToggle() {
    document.querySelectorAll('#mode-toggle .seg').forEach((btn) => {
      btn.setAttribute('aria-selected', String(btn.dataset.mode === state.mode));
    });
  }

  function renderBreadcrumb() {
    const bc = $('breadcrumb');
    const crumbs = [{ label: rootLabel(), go: () => { state.path = []; state.album = null; state.browseSongs = []; } }];
    state.path.forEach((p, i) => crumbs.push({
      label: p.label, go: () => { state.path = state.path.slice(0, i + 1); state.album = null; },
    }));
    if (state.album) crumbs.push({ label: state.album.label, current: true });

    bc.innerHTML = crumbs.map((c, i) => {
      const last = i === crumbs.length - 1;
      const cls = (last || c.current) ? 'crumb current' : 'crumb';
      const sep = i > 0 ? '<span class="crumb-sep">›</span>' : '';
      return `${sep}<span class="${cls}" data-i="${i}">${esc(c.label)}</span>`;
    }).join(' ');

    bc.querySelectorAll('.crumb:not(.current)').forEach((el) => {
      el.onclick = () => { crumbs[+el.dataset.i].go(); renderBrowse(); };
    });
  }

  // Fetch the songs for the first drill level SERVER-SIDE (one facet's
  // worth, not the catalog): artist mode → ?artist=…, genre mode → ?genre=….
  async function drillFetch(param, value) {
    state.browseLoading = true;
    renderChips();
    try {
      state.browseSongs = await fetchSongs({ [param]: value, limit: 500, sort: 'album' });
    } catch (e) {
      state.browseSongs = [];
      toast('Could not load songs: ' + e.message);
    }
    state.browseLoading = false;
    renderBrowse();
  }

  function facetChipsData() {
    const kind = levelKind();
    if (kind === 'artist' && state.mode === 'artist') {
      // root of artist mode — straight from /api/facets
      return { kind, buckets: (state.facets?.artists || []).map((a) =>
        ({ key: normKey(a.name), label: a.name, count: a.count })) };
    }
    if (kind === 'genre') {
      return { kind, buckets: (state.facets?.genres || []).map((g) =>
        ({ key: normKey(g.name), label: g.name, count: g.count })) };
    }
    // deeper levels bucket the fetched facet page client-side
    const arr  = songsForPath();
    const keyFn = kind === 'artist' ? (s) => s.artist : (s) => albumLabelOf(s);
    const buckets = [...bucketByNorm(arr, keyFn).values()]
      .map((b) => ({ key: b.key, label: b.label, count: b.items.length, items: b.items }));
    if (kind === 'album') {
      for (const b of buckets) {
        b.year = b.items.reduce((y, s) => (s.year && (!y || s.year < y)) ? s.year : y, 0);
      }
      buckets.sort((a, b) => (a.year || 9999) - (b.year || 9999) || a.label.localeCompare(b.label));
    } else {
      buckets.sort((a, b) => (!!b.key - !!a.key) || a.label.localeCompare(b.label));
    }
    return { kind, buckets };
  }

  function renderChips() {
    const wrap = $('chips'), empty = $('chips-empty');
    if (state.browseLoading) {
      wrap.innerHTML = ''; empty.hidden = false;
      empty.textContent = 'Loading…';
      return;
    }
    const { kind, buckets } = facetChipsData();
    if (!buckets.length) {
      wrap.innerHTML = ''; empty.hidden = false;
      empty.textContent = state.facets && state.facets.total
        ? 'Nothing here right now.'
        : 'No streamable songs on the network yet.';
      return;
    }
    empty.hidden = true;
    const ico = kind === 'genre' ? '🏷' : kind === 'artist' ? '👤' : '💿';
    wrap.innerHTML = buckets.map((b, i) => {
      const active = (kind === 'album' && state.album && state.album.key === b.key) ? ' active' : '';
      return `<button class="chip${active}" data-i="${i}">
        <span class="c-ico">${ico}</span>
        <span class="c-name">${esc(b.label)}</span>
        <span class="c-count">${b.count}</span>
      </button>`;
    }).join('');

    wrap.querySelectorAll('.chip').forEach((el) => {
      el.onclick = () => {
        const b = buckets[+el.dataset.i];
        if (kind === 'album') {
          state.album = { key: b.key, label: b.label };
          renderBrowse();
          return;
        }
        state.path.push({ key: b.key, label: b.label });
        state.album = null;
        // First drill level pulls that facet's songs from the server;
        // deeper levels slice the already-fetched page.
        if (state.path.length === 1) {
          drillFetch(state.mode === 'artist' ? 'artist' : 'genre', b.label);
        } else {
          renderBrowse();
        }
      };
    });
  }

  function renderTrackPane() {
    const head = $('track-head'), list = $('tracks'), empty = $('track-empty');
    if (!state.album) {
      head.hidden = true; list.innerHTML = ''; empty.hidden = false;
      empty.textContent = 'Pick an album to see its tracks.';
      return;
    }
    const tracks = songsForPath()
      .filter((s) => normKey(albumLabelOf(s)) === state.album.key)
      .sort((a, b) => (a.trackNumber || 9999) - (b.trackNumber || 9999)
                   || a.title.localeCompare(b.title));

    const year = tracks.reduce((y, s) => s.year || y, 0);
    head.hidden = false;
    head.innerHTML = `
      <div>
        <div class="th-title">${esc(state.album.label)}</div>
        <div class="th-sub">${tracks.length} track${tracks.length === 1 ? '' : 's'}${year ? ' · ' + year : ''}</div>
      </div>
      <button class="th-close" id="th-close">✕ close</button>`;
    $('th-close').onclick = () => { state.album = null; renderBrowse(); };

    empty.hidden = true;
    renderTrackList(list, tracks, /*numbered=*/true, /*withArt=*/true);
  }

  // ─────────────────── Shared track list ────────────────────
  function renderTrackList(list, tracks, numbered, withArt) {
    const playable = tracks.filter((s) => avail(s));
    list.innerHTML = tracks.map((s, i) => {
      const off = !avail(s);
      const playing = state.playing === s.contentHash ? ' playing' : '';
      const num = numbered ? (s.trackNumber || i + 1) : i + 1;
      return `<div class="track${playing}${off ? ' off' : ''}" data-i="${i}">
        <div class="t-num">${num}</div>
        <div class="t-art">${withArt ? songArt(s) : ''}</div>
        <div class="t-main">
          <div class="t-title">${esc(s.title) || '(untitled)'}</div>
          <div class="t-artist">${esc(s.artist)}</div>
        </div>
        <div class="t-plays">${fmtPlays(s.playCount)} plays</div>
        <div class="t-dur">${fmtDur(s.durationMs)}</div>
        <div class="t-play">${off ? '' : '▶'}</div>
      </div>`;
    }).join('');
    list.querySelectorAll('.track').forEach((el) => {
      el.onclick = () => {
        const s = tracks[+el.dataset.i];
        if (!avail(s)) { toast('No seeders online for this track right now.'); return; }
        state.queue = playable;
        playFromQueue(playable.findIndex((p) => p.contentHash === s.contentHash));
      };
    });
  }

  // ─────────────────────── Search ──────────────────────────
  async function runSearch(q) {
    try {
      const songs = await fetchSongs({ q, limit: 200, sort: 'plays' });
      if (state.query !== q) return;   // superseded while in flight
      state.list = {
        kicker: 'Search',
        title:  `“${q}”`,
        sub:    `${songs.length} result${songs.length === 1 ? '' : 's'}`,
        songs,
        seed:   seedFromName(q),
      };
      showView('list'); renderList();
    } catch (e) {
      toast('Search failed: ' + e.message);
    }
  }

  // ───────────────────────── Playback ──────────────────────
  // Two engines: the WASM decoder (low click-to-play latency, mp3/flac/ogg) and
  // the native <audio> element as a fallback for anything WASM can't decode.
  const audio = $('audio');
  const wasm  = window.WasmPlayer ? new window.WasmPlayer() : null;
  let engine  = 'audio';            // which engine is currently driving playback

  const engPaused = () => (engine === 'wasm' && wasm ? wasm.paused : audio.paused);
  const engCurSec = () => ((engine === 'wasm' && wasm ? wasm.currentTime : audio.currentTime) || 0);
  const engDurSec = () => (engine === 'wasm' && wasm ? wasm.duration : (audio.duration || 0));

  function npToggleIcon() { $('np-toggle').textContent = engPaused() ? '▶' : '⏸'; }
  function npProgress() {
    const cur = engCurSec(), dur = engDurSec();
    $('np-cur').textContent = fmtDur(cur * 1000);
    if (dur) { $('np-dur').textContent = fmtDur(dur * 1000); $('np-bar').value = Math.round((cur / dur) * 1000); }
  }

  function playFromQueue(i) {
    if (i < 0 || i >= state.queue.length) return;
    state.qIndex = i;
    play(state.queue[i]);
  }

  function play(song) {
    if (!song) return;
    // Silence whatever is playing RIGHT NOW, synchronously, before the async
    // startEngine for the new song runs — otherwise a quick second click leaves
    // the first track playing underneath the new one.
    audio.pause();
    try { audio.removeAttribute('src'); audio.load(); } catch (_) {}
    if (wasm) { wasm.unlock(); wasm.stop(); }   // unlock resumes the ctx in this gesture; stop silences the old song
    completePlay();                 // finalize the previous song's reward session
    state.playing = song.contentHash;
    $('nowplaying').hidden = false;
    $('np-art').innerHTML  = songArt(song);
    $('np-title').textContent  = song.title || '(untitled)';
    $('np-artist').textContent = song.artist || '';
    $('np-cur').textContent = '0:00'; $('np-bar').value = 0;
    $('np-dur').textContent = fmtDur(song.durationMs || 0);
    $('np-spin').hidden = false;
    $('np-toggle').textContent = '⏸';
    startEngine(song);
    startPlay(song);                // open reward session (artist/seeder/mini mint)
    renderView();                   // refresh playing highlights wherever we are
  }

  // Try the WASM decoder first; fall back to <audio> on unsupported codec, error,
  // or if no audio actually starts within a few seconds (watchdog).
  async function startEngine(song) {
    const url = streamUrl(song.contentHash);
    const durSec = (song.durationMs || 0) / 1000;
    audio.pause();
    try { audio.removeAttribute('src'); audio.load(); } catch (_) {}
    const useAudio = () => {
      if (state.playing !== song.contentHash) return;
      engine = 'audio';
      audio.src = url;
      audio.play().catch(() => {/* autoplay policy: user can hit play */});
    };
    if (wasm) {
      let started = false;
      wasm.onplaying    = () => { started = true; if (state.playing === song.contentHash) { $('np-spin').hidden = true; npToggleIcon(); } };
      wasm.ontimeupdate = () => { if (state.playing === song.contentHash) npProgress(); };
      wasm.onended      = () => { if (state.playing === song.contentHash) onTrackEnded(); };
      try {
        await wasm.load(url, durSec);
        if (state.playing !== song.contentHash) return;   // superseded by a newer click
        engine = 'wasm';
        setTimeout(() => {          // watchdog: no sound? drop to <audio>
          if (!started && engine === 'wasm' && state.playing === song.contentHash) {
            wasm.stop(); useAudio();
          }
        }, 5000);
        return;
      } catch (_) {                 // unsupported codec or decode error
        if (state.playing !== song.contentHash) return;   // a newer song is loading — don't touch it
        try { await wasm.stop(); } catch (_) {}
      }
    }
    useAudio();
  }

  function onTrackEnded() {
    if (state.qIndex >= 0 && state.qIndex + 1 < state.queue.length) {
      playFromQueue(state.qIndex + 1);   // play() finalizes the finished session
    } else {
      completePlay();
      state.playing = null; $('np-toggle').textContent = '▶'; renderView();
    }
  }

  // ── Reward session: the play earns for the artist/seeder/mini, never the
  // listener. The browser reports REAL playback progress so a mint only lands
  // on a genuine listen — same lifecycle as the native player.
  let hbTimer = null;
  function stopHeartbeat() { if (hbTimer) { clearInterval(hbTimer); hbTimer = null; } }
  async function startPlay(song) {
    stopHeartbeat(); state.playId = null;
    try {
      const r = await apiPost('/api/play/start', { contentHash: song.contentHash });
      if (state.playing === song.contentHash) {
        state.playId = r.playId;
        hbTimer = setInterval(() => {
          if (state.playId && !engPaused())
            apiPost('/api/play/heartbeat',
              { playId: state.playId, positionMs: Math.floor(engCurSec() * 1000) })
              .catch(() => {});
        }, 5000);
      }
    } catch (_) { /* reward best-effort; playback continues regardless */ }
  }
  function completePlay() {
    stopHeartbeat();
    const id = state.playId; state.playId = null;
    if (id) apiPost('/api/play/complete', { playId: id }).catch(() => {});
  }

  // <audio> events only drive the UI when it's the active engine.
  audio.addEventListener('playing',        () => { if (engine === 'audio') { $('np-spin').hidden = true; npToggleIcon(); } });
  audio.addEventListener('waiting',        () => { if (engine === 'audio') $('np-spin').hidden = false; });
  audio.addEventListener('pause',          () => { if (engine === 'audio') npToggleIcon(); });
  audio.addEventListener('play',           () => { if (engine === 'audio') npToggleIcon(); });
  audio.addEventListener('loadedmetadata', () => { if (engine === 'audio') $('np-dur').textContent = fmtDur(audio.duration * 1000); });
  audio.addEventListener('timeupdate',     () => { if (engine === 'audio') npProgress(); });
  audio.addEventListener('ended',          () => { if (engine === 'audio') onTrackEnded(); });
  audio.addEventListener('error',          () => {
    if (engine !== 'audio') return;
    $('np-spin').hidden = true;
    if (state.playing) toast('Could not stream this track — no seeders online right now.');
  });

  // Finalize the reward session if the tab is closed mid-play.
  window.addEventListener('pagehide', () => {
    if (state.playId && navigator.sendBeacon)
      navigator.sendBeacon(CFG.gateway + '/api/play/complete',
                           JSON.stringify({ playId: state.playId }));
  });

  $('np-toggle').onclick = () => {
    if (engine === 'wasm' && wasm) { wasm.paused ? wasm.resume() : wasm.pause(); setTimeout(npToggleIcon, 0); }
    else { audio.paused ? audio.play() : audio.pause(); }
  };
  $('np-prev').onclick = () => playFromQueue(state.qIndex - 1);
  $('np-next').onclick = () => playFromQueue(state.qIndex + 1);
  // While dragging (input): update the time label live, and for <audio> seek
  // natively (cheap). For WASM, DON'T seek on every input event — each seek is a
  // fresh fetch + decoder, so a drag would fire a storm of them and stall. Seek
  // once on release (change).
  $('np-bar').oninput = () => {
    const frac = +$('np-bar').value / 1000;
    const dur = engDurSec();
    if (dur) $('np-cur').textContent = fmtDur(frac * dur * 1000);
    if (engine === 'audio' && audio.duration) audio.currentTime = frac * audio.duration;
  };
  $('np-bar').onchange = () => {
    if (engine === 'wasm' && wasm && wasm.duration) {
      const frac = +$('np-bar').value / 1000;
      wasm.seek(frac * wasm.duration);
    }
  };

  // ───────────────────── Data + status ─────────────────────
  async function refreshFacets() {
    try {
      state.facets = await apiGet('/api/facets');
      setStatus('ok', `${state.facets.total || 0} songs online`);
      if (state.view === 'browse' && state.path.length === 0) renderChips();
    } catch (e) {
      setStatus('err', 'gateway unreachable');
    }
  }

  async function refreshCollections() {
    try {
      state.collections = await apiGet('/api/collections');
      if (state.view === 'home') renderHome();
    } catch (e) {
      if (!state.collections && state.view === 'home') {
        $('home-empty').hidden = false;
        $('home-empty').textContent = 'Could not reach the network. Retrying…';
      }
    }
  }

  function setStatus(kind, text) {
    $('status-dot').className = 'dot ' + (kind === 'ok' ? 'ok' : kind === 'warn' ? 'warn' : 'err');
    $('status-text').textContent = text;
  }

  // ───────────────────────── Wiring ────────────────────────
  function wire() {
    document.querySelectorAll('#viewnav .vn-btn').forEach((btn) => {
      btn.onclick = () => {
        $('search').value = ''; state.query = '';
        showView(btn.dataset.view);
        renderView();
      };
    });

    document.querySelectorAll('#mode-toggle .seg').forEach((btn) => {
      btn.onclick = () => {
        if (state.mode === btn.dataset.mode) return;
        state.mode = btn.dataset.mode; state.path = []; state.album = null; state.browseSongs = [];
        renderBrowse();
      };
    });

    let searchTimer = null;
    $('search').addEventListener('input', (e) => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => {
        const q = e.target.value.trim();
        state.query = q;
        if (!q) {
          if (state.view === 'list' && state.list?.kicker === 'Search') {
            showView(state.lastView); renderView();
          }
          return;
        }
        runSearch(q);   // server-side — never pulls the catalog
      }, 250);
    });

    // CTA download dropdown
    const link = $('cta-link'), dd = $('cta-dropdown');
    document.querySelectorAll('.cta-opt').forEach((a) => { a.href = CFG.downloads[a.dataset.os] || '#'; });
    link.onclick = (e) => {
      e.stopPropagation();
      const open = dd.hidden;
      dd.hidden = !open; link.setAttribute('aria-expanded', String(open));
    };
    document.addEventListener('click', () => { dd.hidden = true; link.setAttribute('aria-expanded', 'false'); });

    // Draggable pane divider
    const divider = $('divider'), chipPane = $('chip-pane');
    let dragging = false;
    divider.addEventListener('pointerdown', (e) => { dragging = true; divider.setPointerCapture(e.pointerId); });
    divider.addEventListener('pointermove', (e) => {
      if (!dragging) return;
      const panes = $('panes').getBoundingClientRect();
      const pct = Math.min(80, Math.max(20, ((e.clientY - panes.top) / panes.height) * 100));
      chipPane.style.flexBasis = pct + '%';
    });
    divider.addEventListener('pointerup',  () => { dragging = false; });
  }

  // ───────────────────────── Boot ──────────────────────────
  function boot() {
    wire();
    showView('home');
    renderHome();
    refreshFacets();
    refreshCollections();
    setInterval(refreshFacets, CFG.refreshMs);
    setInterval(refreshCollections, Math.max(CFG.refreshMs, 60000));
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
