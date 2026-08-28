/* Bopwire block explorer — vanilla JS, no framework, no build step.
 *
 * Talks only to the gateway (window.BOPWIRE.gateway) against the explorer
 * API contract:
 *   /api/blocks, /api/blocks/:id, /api/tx/:hash, /api/address/:addr[,/history],
 *   /api/search, /api/stats, /api/stats/artist/:id, /api/song/:hash[,/fingerprint]
 *
 * The backend lands route-by-route, so every view degrades to a labelled
 * "endpoint not live yet" panel instead of breaking, and ?mock=1 swaps the
 * whole fetch layer for the deterministic in-browser mock (explorer-mock.js).
 *
 * The search box is a COMMAND BAR: bare heights / hashes / addresses / text
 * still route straight to the right page, but it also understands ~24
 * commands ("blocks 100..200", "plays by 0x…", "top songs 10", …) documented
 * in the collapsible help under the bar. Commands are parsed and composed
 * client-side from whatever endpoints exist.
 */
(() => {
  'use strict';
  const CFG  = window.BOPWIRE;
  const $    = (id) => document.getElementById(id);
  const MOCK = new URLSearchParams(location.search).get('mock') === '1';

  const UNITS = 100000000n;              // 1 token = 1e8 internal units
  const RANGE_CAP  = 100;                // widest block range a command fetches
  const EXPAND_CAP = 20;                 // widest range rendered fully expanded
  const SCAN_N     = 120;                // blocks scanned for client-side rankings

  // ─────────────────────── Utilities ───────────────────────
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g,
    (c) => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[c]));

  const fmtInt = (n) => (n === undefined || n === null) ? '—' : Number(n).toLocaleString('en-US');

  function fmtAmt(v, full) {
    if (v === undefined || v === null) return '—';
    let n;
    try { n = typeof v === 'bigint' ? v : BigInt(typeof v === 'number' ? Math.round(v) : String(v)); }
    catch (_) { return esc(String(v)); }
    const neg = n < 0n; if (neg) n = -n;
    let frac = (n % UNITS).toString().padStart(8, '0').replace(/0+$/, '');
    if (!full && frac.length > 4) frac = frac.slice(0, 4);
    return (neg ? '-' : '') + (n / UNITS).toLocaleString('en-US') + (frac ? '.' + frac : '');
  }
  const amt = (v) => v === undefined || v === null ? '—'
    : `<span title="${fmtAmt(v, true)} tokens">${fmtAmt(v)}</span> <span class="dim mini">tok</span>`;

  const tsMs = (v) => {                  // seconds-vs-ms heuristic
    if (v === undefined || v === null) return null;
    const n = Number(v);
    return n > 1e12 ? n : n > 1e9 ? n * 1000 : n;
  };
  const fmtTime = (v) => {
    const ms = tsMs(v);
    return ms === null ? '—' : new Date(ms).toISOString().replace('T', ' ').replace(/\.\d+Z/, ' UTC');
  };
  const fmtAgo = (v) => {
    const ms = tsMs(v); if (ms === null) return '—';
    let d = Math.max(0, Date.now() - ms) / 1000;
    if (d < 90)     return `${d | 0}s ago`;
    if (d < 5400)   return `${(d / 60) | 0}m ago`;
    if (d < 129600) return `${(d / 3600) | 0}h ago`;
    return `${(d / 86400) | 0}d ago`;
  };
  const fmtDur = (ms) => {
    const t = Math.max(0, Math.floor((ms || 0) / 1000));
    return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
  };
  const fmtBytes = (b) => b === undefined ? '—'
    : b < 1024 ? `${b} B` : b < 1048576 ? `${(b / 1024).toFixed(1)} KB` : `${(b / 1048576).toFixed(2)} MB`;

  const shortHex = (h, n) => {
    h = String(h || ''); n = n || 10;
    return h.length <= n * 2 + 1 ? h : `${h.slice(0, n)}…${h.slice(-6)}`;
  };

  const COPY_SVG = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 3h11a1 1 0 0 1 1 1v13h-2V5H8V3zM5 7h11a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1zm1 2v10h9V9H6z"/></svg>';
  const copyBtn = (v) =>
    `<button class="copy-btn" data-copy="${esc(v)}" title="Copy" aria-label="Copy">${COPY_SVG}</button>`;

  function toast(msg) {
    const t = $('toast');
    t.textContent = msg; t.hidden = false;
    clearTimeout(toast._t); toast._t = setTimeout(() => { t.hidden = true; }, 2200);
  }
  document.addEventListener('click', (e) => {
    const b = e.target.closest('.copy-btn');
    if (!b) return;
    navigator.clipboard?.writeText(b.dataset.copy)
      .then(() => toast('Copied'), () => toast('Copy failed'));
  });

  // linked hex/address renderers
  const blockLink = (id, label) =>
    `<a class="h-link" href="#/block/${esc(id)}">${esc(label ?? shortHex(id))}</a>${copyBtn(id)}`;
  const txLink = (h) => `<a class="h-link" href="#/tx/${esc(h)}">${shortHex(h)}</a>${copyBtn(h)}`;
  const addrLink = (a, label) =>
    `<a class="h-link" href="#/address/${esc(a)}">${esc(label ?? shortHex(a, 8))}</a>${copyBtn(a)}`;
  const songLink = (h, label) =>
    `<a class="h-link" href="#/song/${esc(h)}">${esc(label ?? shortHex(h))}</a>${copyBtn(h)}`;

  const TX_LABEL = { mint: 'Mint (play)', transfer: 'Transfer', song_register: 'Song registration',
                     node_auth: 'Node auth', moderator_op: 'Moderator op',
                     moderation: 'Moderation (hide/unhide)',
                     moderator_proposal: 'Moderator proposal', username_register: 'Username',
                     slash: 'Slash', relay_reward: 'Relay reward', checkpoint: 'Checkpoint',
                     settlement_mint: 'Settlement mint' };
  const TX_NUM = { 0x01: 'transfer', 0x10: 'mint', 0x20: 'moderator_op', 0x30: 'moderator_proposal',
                   0x40: 'username_register', 0x50: 'slash', 0x60: 'relay_reward',
                   0x70: 'node_auth', 0x71: 'checkpoint', 0x72: 'settlement_mint' };
  const txType = (tx) => {
    let t = tx.type ?? tx.tx_type ?? tx.kind;
    if (typeof t === 'number') t = TX_NUM[t] || String(t);
    t = String(t || 'unknown').toLowerCase();
    if (t === 'song' || t === 'song_section' || t === 'songregister') t = 'song_register';
    return t;
  };
  const txChip = (t) => {
    const cls = t === 'mint' ? 't-mint' : t === 'transfer' ? 't-transfer'
              : t === 'song_register' ? 't-song' : t === 'node_auth' ? 't-nodeauth' : 't-mod';
    return `<span class="chip ${cls}">${esc(TX_LABEL[t] || t)}</span>`;
  };
  const roleChip = (r) => `<span class="chip r-${esc(r)}">${esc(r)}</span>`;

  // ── moderation vocabulary ──
  const LEVEL_NAME = { 0: 'NONE', 1: 'VOICE', 2: 'OP', 3: 'FOUNDER' };
  const OP_NAME    = { 1: 'GRANT', 2: 'REVOKE', 3: 'TAG_LABEL_EDIT' };
  const ACTION_CLS = { hide: 't-mod', unhide: 't-mint', grant: 't-nodeauth', revoke: 't-mod' };
  const actionChip = (a) =>
    `<span class="chip ${ACTION_CLS[a] || ''}">${esc(a)}</span>`;
  const levelName = (l) => LEVEL_NAME[l] ?? String(l);

  // a moderation action's target: a song/artist/album, or a moderator wallet
  function targetHtml(t) {
    if (!t) return '<span class="dim">—</span>';
    if (t.address || t.kind === 'moderator')
      return `<span class="chip">moderator</span> ${addrLink(t.address)}` +
        (t.level !== undefined ? ` <span class="dim mini">level ${esc(levelName(t.level))}</span>` : '');
    const name = t.content_hash
      ? `<a href="#/song/${esc(t.content_hash)}">${esc(t.name || shortHex(t.content_hash))}</a>`
      : `<b>${esc(t.name || '?')}</b>`;
    return `<span class="chip">${esc(t.kind || '?')}</span> ${name}` +
      (t.artist ? ` <span class="dim">by ${esc(t.artist)}</span>` : '');
  }

  // shared table for lists of moderation actions
  function modActionsTable(actions, opts) {
    opts = opts || {};
    if (!actions.length) return '<div class="note">No moderation actions.</div>';
    return `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Action</th><th>Target</th>` +
      (opts.noModerator ? '' : '<th>Moderator</th>') +
      `<th>Block</th><th>When</th><th>Tx</th></tr></thead><tbody>` +
      actions.map((a) => `<tr><td>${actionChip(a.action)}${
          a.action === 'grant' ? ` <span class="dim mini">${esc(levelName(a.level))}</span>` : ''}</td>
        <td>${targetHtml(a.target)}</td>` +
        (opts.noModerator ? '' : `<td>${addrLink(a.moderator_address)}</td>`) +
        `<td><a href="#/block/${a.block_height}">#${fmtInt(a.block_height)}</a></td>
        <td class="dim" title="${fmtTime(a.timestamp_ms)}">${fmtAgo(a.timestamp_ms)}</td>
        <td><a href="#/tx/${esc(a.tx_hash)}">${shortHex(a.tx_hash, 8)}</a></td></tr>`).join('') +
      '</tbody></table></div>';
  }

  // ─────────────────────── API layer ───────────────────────
  class ApiError extends Error {
    constructor(status, msg, path) { super(msg); this.status = status; this.path = path; }
  }

  async function exGet(path) {                       // JSON routes
    if (MOCK) {
      const r = window.BOPWIRE_MOCK.route(path);
      if (r.status !== 200) throw new ApiError(r.status, r.body?.error || `HTTP ${r.status}`, path);
      return r.body;
    }
    let r;
    try { r = await fetch(CFG.gateway + path, { mode: 'cors' }); }
    catch (e) { throw new ApiError(0, 'network error — gateway unreachable', path); }
    if (!r.ok) {
      let msg = `HTTP ${r.status}`;
      try { msg = (await r.json()).error || msg; } catch (_) {}
      throw new ApiError(r.status, msg, path);
    }
    return r.json();
  }

  async function exText(path) {                      // raw-body routes (fingerprint)
    if (MOCK) {
      const r = window.BOPWIRE_MOCK.route(path);
      if (r.status !== 200) throw new ApiError(r.status, r.body?.error || `HTTP ${r.status}`, path);
      return typeof r.body === 'string' ? r.body : JSON.stringify(r.body);
    }
    let r;
    try { r = await fetch(CFG.gateway + path, { mode: 'cors' }); }
    catch (e) { throw new ApiError(0, 'network error — gateway unreachable', path); }
    if (!r.ok) throw new ApiError(r.status, `HTTP ${r.status}`, path);
    return r.text();
  }

  const notLive = (e) => e instanceof ApiError && (e.status === 0 || e.status === 404 || e.status === 503);

  function stubPanel(what, endpoint, extra) {
    return `<div class="stub"><b>${esc(what)} isn't available yet.</b><br/>
      This needs <code>GET ${esc(endpoint)}</code> on the gateway, which hasn't gone live —
      or the thing you asked for doesn't exist on chain. ${extra || ''}<br/>
      <span class="mini">Tip: append <code>?mock=1</code> to the page URL to browse the demo dataset.</span></div>`;
  }
  const errPanel = (e) =>
    `<div class="stub err-panel"><b>Error:</b> ${esc(e.message)}${e.path ? ` <span class="mini">(${esc(e.path)})</span>` : ''}</div>`;

  // ─────────────── data composition helpers ────────────────
  let tipCache = { h: null, at: 0 };
  async function tipHeight() {
    if (tipCache.h !== null && Date.now() - tipCache.at < 30_000) return tipCache.h;
    const r = await exGet('/api/blocks?offset=0&limit=1');
    const h = r.blocks?.[0]?.height ?? (r.total ? r.total - 1 : 0);
    tipCache = { h, at: Date.now() };
    return h;
  }

  // fetch full blocks for heights[], concurrency-limited, with progress
  async function fetchBlocksByHeight(heights, onProgress) {
    const out = new Array(heights.length);
    let i = 0, done = 0;
    async function worker() {
      while (i < heights.length) {
        const my = i++;
        try { out[my] = await exGet(`/api/blocks/${heights[my]}`); }
        catch (e) { if (!notLive(e) && e.status !== 404) throw e; out[my] = null; }
        done++; if (onProgress) onProgress(done, heights.length);
      }
    }
    await Promise.all(Array.from({ length: Math.min(6, heights.length) }, worker));
    return out.filter(Boolean);
  }

  const blockTxsOf = (b) => b.transactions || b.txs || [];

  // pull address history pages until `max` items or exhausted
  async function pullHistory(a, max) {
    const items = []; let offset = 0, total = Infinity;
    while (items.length < max && offset < total) {
      const r = await exGet(`/api/address/${a}/history?offset=${offset}&limit=100`);
      const page = r.history || r.items || r.txs || [];
      total = r.total ?? (page.length < 100 ? offset + page.length : Infinity);
      items.push(...page);
      if (!page.length) break;
      offset += page.length;
    }
    return { items: items.slice(0, max), total: total === Infinity ? items.length : total };
  }

  // pull the moderation log (chain order) up to `max` actions
  async function pullModeration(max) {
    const items = []; let offset = 0, total = Infinity;
    while (items.length < max && offset < total) {
      const r = await exGet(`/api/moderation?offset=${offset}&limit=100`);
      const page = r.actions || r.items || [];
      total = r.total ?? (page.length < 100 ? offset + page.length : Infinity);
      items.push(...page);
      if (!page.length) break;
      offset += page.length;
    }
    return items.slice(0, max);
  }

  // song title cache for enriching history rows
  const songTitleCache = new Map();
  async function songTitle(hash) {
    if (!hash) return null;
    if (songTitleCache.has(hash)) return songTitleCache.get(hash);
    let t = null;
    try { const s = await exGet(`/api/song/${hash}`); t = s.title || null; } catch (_) {}
    songTitleCache.set(hash, t);
    return t;
  }

  // resolve free text to an artist {id,name} or song hash via /api/search
  async function searchTyped(q) {
    const r = await exGet(`/api/search?q=${encodeURIComponent(q)}`);
    if (Array.isArray(r))              // legacy gateway shape: bare song array
      return r.map((s) => ({ type: 'song', content_hash: s.contentHash || s.content_hash,
                             title: s.title, artist: s.artist, album: s.album, genre: s.genre }));
    return r.results || r.hits || [];
  }

  // ─────────────────────── charts ──────────────────────────
  const SERIES_COLORS = ['#10a274', '#3086d6'];   // validated vs #15171c (see explorer.css)

  function niceTicks(max, n) {
    if (max <= 0) return [0, 1];
    const step = Math.pow(10, Math.floor(Math.log10(max / n)));
    const err = max / n / step;
    const mult = err >= 7.5 ? 10 : err >= 3.5 ? 5 : err >= 1.5 ? 2 : 1;
    const s = mult * step, ticks = [];
    for (let v = 0; v <= max + 1e-9; v += s) ticks.push(v);
    return ticks;
  }

  // series: [{name, points:[{t(ms), v}]}] — up to 2, colors assigned in fixed order
  function lineChart(container, series, opts) {
    opts = opts || {};
    // Size the coordinate system to the real container so axis/label text
    // keeps a readable on-screen size on phones instead of scaling down.
    const W = Math.max(340, Math.min(760, container.clientWidth || 720));
    const H = opts.h || 230, padL = 46, padR = W < 480 ? 54 : 78, padT = 12, padB = 26;
    const all = series.flatMap((s) => s.points);
    if (!all.length) { container.innerHTML = '<div class="note">No data.</div>'; return; }
    const t0 = Math.min(...all.map((p) => p.t)), t1 = Math.max(...all.map((p) => p.t));
    const vmax = Math.max(1, ...all.map((p) => p.v));
    const yticks = niceTicks(vmax, 4);
    const ymax = yticks[yticks.length - 1];
    const X = (t) => t1 === t0 ? (padL + W - padR) / 2 : padL + (t - t0) / (t1 - t0) * (W - padL - padR);
    const Y = (v) => padT + (1 - v / ymax) * (H - padT - padB);

    let g = '<g class="grid">';
    for (const v of yticks) g += `<line x1="${padL}" x2="${W - padR}" y1="${Y(v)}" y2="${Y(v)}"/>`;
    g += '</g><g class="axis">';
    for (const v of yticks) g += `<text x="${padL - 6}" y="${Y(v) + 3}" text-anchor="end">${fmtInt(v)}</text>`;
    const nx = Math.min(5, Math.max(2, Math.floor((W - padL - padR) / 130)));
    for (let i = 0; i <= nx; i++) {
      const t = t0 + (t1 - t0) * i / nx;
      g += `<text x="${X(t)}" y="${H - 8}" text-anchor="middle">${new Date(t).toISOString().slice(5, 10)}</text>`;
    }
    g += '</g>';

    let paths = '', labels = '';
    series.forEach((s, si) => {
      const c = SERIES_COLORS[si % SERIES_COLORS.length];
      const d = s.points.map((p, i) => `${i ? 'L' : 'M'}${X(p.t).toFixed(1)},${Y(p.v).toFixed(1)}`).join('');
      paths += `<path class="serie" stroke="${c}" d="${d}"/>`;
      const lp = s.points[s.points.length - 1];
      labels += `<circle cx="${X(lp.t)}" cy="${Y(lp.v)}" r="3" fill="${c}"/>` +
        `<text class="dlabel" x="${X(lp.t) + 7}" y="${Y(lp.v) + 4}" fill="var(--text-dim)">${esc(s.name)}</text>`;
    });

    const legend = series.length >= 2
      ? `<div class="legend">${series.map((s, i) =>
          `<span class="lg-item"><span class="lg-dot" style="background:${SERIES_COLORS[i % 2]}"></span>${esc(s.name)}</span>`).join('')}</div>`
      : '';
    container.innerHTML = `${legend}<div class="chart-wrap">
      <svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${esc(opts.label || 'chart')}">${g}${paths}${labels}
        <line class="xhair" x1="0" x2="0" y1="${padT}" y2="${H - padB}" stroke="var(--line)" stroke-width="1" visibility="hidden"/>
      </svg><div class="chart-tip"></div></div>`;

    // hover: nearest point on series 0's time base
    const wrap = container.querySelector('.chart-wrap');
    const svg = wrap.querySelector('svg'), tip = wrap.querySelector('.chart-tip'),
          xhair = wrap.querySelector('.xhair');
    const base = series[0].points;
    svg.addEventListener('pointermove', (e) => {
      const r = svg.getBoundingClientRect();
      const t = t0 + (t1 - t0) * Math.min(1, Math.max(0,
        ((e.clientX - r.left) / r.width * W - padL) / (W - padL - padR)));
      let bi = 0;
      for (let i = 1; i < base.length; i++)
        if (Math.abs(base[i].t - t) < Math.abs(base[bi].t - t)) bi = i;
      const pt = base[bi];
      xhair.setAttribute('x1', X(pt.t)); xhair.setAttribute('x2', X(pt.t));
      xhair.setAttribute('visibility', 'visible');
      tip.style.display = 'block';
      tip.innerHTML = `<div class="tt-date">${new Date(pt.t).toISOString().slice(0, 10)}</div>` +
        series.map((s, si) => {
          let near = s.points[0];
          for (const p of s.points) if (Math.abs(p.t - pt.t) < Math.abs(near.t - pt.t)) near = p;
          return `<div class="tt-row"><span class="tt-dot" style="background:${SERIES_COLORS[si % 2]}"></span>${esc(s.name)}: <b>${fmtInt(near.v)}</b></div>`;
        }).join('');
      const px = X(pt.t) / W * r.width;
      tip.style.left = `${Math.min(r.width - tip.offsetWidth - 6, Math.max(4, px + 12))}px`;
      tip.style.top = '10px';
    });
    svg.addEventListener('pointerleave', () => {
      tip.style.display = 'none'; xhair.setAttribute('visibility', 'hidden');
    });
  }

  // rows: [{label(html-safe), value(number), href?, valueLabel?}] — single hue
  const hbarList = (rows) => {
    const max = Math.max(1, ...rows.map((r) => r.value));
    return `<div class="hbars">${rows.map((r) => {
      const lab = r.href ? `<a href="${esc(r.href)}">${esc(r.label)}</a>` : esc(r.label);
      return `<span class="hb-label" title="${esc(r.label)}">${lab}</span>` +
        `<span><span class="hb-bar" style="width:${(r.value / max * 100).toFixed(1)}%"></span></span>` +
        `<span class="hb-val">${r.valueLabel || fmtInt(r.value)}</span>`;
    }).join('')}</div>`;
  };

  // ────────────────── generic field rendering ──────────────
  const HEX64 = /^[0-9a-f]{64}$/i;
  function fmtVal(key, val) {
    const k = key.toLowerCase();
    if (val === null || val === undefined) return '—';
    if (typeof val === 'object')
      return `<code>${esc(JSON.stringify(val))}</code>`;
    const s = String(val);
    if (/^(year|track|version|op_code|level)$/.test(k)) return esc(s);   // no digit grouping
    if (/^0x[0-9a-f]{40}$/i.test(s)) return addrLink(s);   // any wallet-shaped value
    if (k === 'content_hash' && HEX64.test(s)) return songLink(s);
    if ((k === 'block_hash' || k === 'prev_hash' || k === 'registration_block')
        && HEX64.test(s) && !/^0+$/.test(s))
      return blockLink(s);
    if (k === 'tx_hash' && HEX64.test(s)) return txLink(s);
    // bare "hash" is ambiguous (a block header's own hash vs a tx's) — show
    // it copyable but unlinked; explicit link sites build their own anchors.
    if (HEX64.test(s) || /^[0-9a-f]{66,}$/i.test(s))
      return `<span class="h-link" title="${esc(s)}">${shortHex(s)}</span>${copyBtn(s)}`;
    if (/timestamp|_at$/.test(k) && Number(val) > 1e9)
      return `${fmtTime(val)} <span class="dim mini">(${fmtAgo(val)})</span>`;
    if (/duration_ms/.test(k)) return `${fmtDur(Number(val))} <span class="dim mini">(${fmtInt(val)} ms)</span>`;
    if (/amount|balance|earned|burn|minted|reward/.test(k)) return amt(val);
    if (/size_bytes/.test(k)) return fmtBytes(Number(val));
    if (typeof val === 'number') return fmtInt(val);
    return esc(s);
  }
  const kvRow = (k, vHtml) => `<div class="k">${esc(k)}</div><div class="v">${vHtml}</div>`;
  function kvAll(obj, skip) {
    skip = new Set(skip || []);
    return Object.entries(obj)
      .filter(([k]) => !skip.has(k))
      .map(([k, v]) => kvRow(k, fmtVal(k, v))).join('');
  }
  const rawDetails = (obj, label) =>
    `<details class="raw"><summary>${esc(label || 'Raw JSON')}</summary><pre>${esc(JSON.stringify(obj, null, 2))}</pre></details>`;

  // ─────────────────── tx rendering ────────────────────────
  function mintSummary(tx) {
    const p = tx.proof || tx;
    return `${addrLink(p.player_address)} <span class="dim">played</span> ` +
      `${songLink(p.content_hash)} <span class="dim">by</span> ${addrLink(p.artist_address)}`;
  }
  function txSummaryHtml(tx) {
    const t = txType(tx);
    if (t === 'mint') return mintSummary(tx);
    if (t === 'transfer')
      return `${addrLink(tx.from_address)} <span class="dim">&rarr;</span> ${addrLink(tx.to_address)} &nbsp;${amt(tx.amount)}`;
    if (t === 'song_register')
      return `<b>${esc(tx.title || '')}</b> <span class="dim">by</span> ${esc(tx.artist || '')} — ${songLink(tx.content_hash)}`;
    if (t === 'node_auth') return `founder ${tx.action || 'authorizes'} node`;
    if (t === 'moderator_op')
      return `${actionChip((OP_NAME[tx.op_code] || tx.action || 'op').toLowerCase())} ` +
        `${addrLink(tx.subject)} <span class="dim">by</span> ${addrLink(tx.proposer)}`;
    if (t === 'moderation')
      return `${actionChip(tx.action || 'moderation')} ${targetHtml(tx.target)}`;
    return `<span class="dim">${esc(TX_LABEL[t] || t)}</span>`;
  }

  function txCard(tx, opts) {
    opts = opts || {};
    const t = txType(tx);
    const hash = tx.hash || tx.tx_hash || '';
    let body = '';
    if (t === 'mint') {
      const p = tx.proof || {};
      body = `<div style="padding:8px 0 2px">${mintSummary(tx)}</div><div class="kv">` +
        kvAll(p, ['node_signature', 'player_signature', 'mini_signature',
                  'serving_node_pubkey', 'player_pubkey', 'mini_pubkey']) +
        (tx.burn_amount ? kvRow('burn_amount', amt(tx.burn_amount)) : '') + `</div>` +
        (Array.isArray(tx.outputs) && tx.outputs.length
          ? `<div class="panel-title" style="margin-top:10px">Outputs — who got paid</div>
             <ul class="outputs-list">` + tx.outputs.map((o) =>
              `<li>${o.lane ? roleChip(o.lane) : ''} ${addrLink(o.address)} ${amt(o.amount)}</li>`).join('') + '</ul>'
          : '');
    } else if (t === 'transfer') {
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'from_pubkey', 'signature',
                   'block_height', 'block_hash', 'timestamp_ms']) + `</div>`;
    } else if (t === 'song_register') {
      const fp = tx.compressed_fingerprint;
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'compressed_fingerprint', 'signature',
                   'block_height', 'block_hash', 'timestamp_ms']) +
        (fp ? kvRow('compressed_fingerprint',
            `<span class="dim">${fmtInt(String(fp).length)} chars base64 — </span>` +
            `<a href="#/song/${esc(tx.content_hash)}?fp=1">view &amp; verify on the song page</a>`) : '') +
        `</div>`;
    } else if (t === 'moderator_op') {
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        kvRow('op_code', `${esc(String(tx.op_code ?? '—'))} <span class="dim">(${esc(OP_NAME[tx.op_code] || tx.action || '?')})</span>`) +
        (tx.level !== undefined ? kvRow('level', `${esc(String(tx.level))} <span class="dim">(${esc(levelName(tx.level))})</span>`) : '') +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'op_code', 'level', 'action', 'signature',
                   'subject_pubkey', 'proposer_pubkey', 'block_height', 'block_hash', 'timestamp_ms']) +
        `</div><div class="mini" style="margin-top:6px">Grants and revocations of moderator status —
          moderator identity on chain is only (address, level, pubkey), never a name.</div>`;
    } else if (t === 'moderation') {
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        (tx.moderator_address ? kvRow('signing moderator', addrLink(tx.moderator_address)) : '') +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'action', 'target', 'moderator_address',
                   'signature', 'block_height', 'block_hash', 'timestamp_ms']) +
        `</div><div class="mini" style="margin-top:6px">Hide/unhide actions propagate takedowns:
          every node applies them and the target stops surfacing in Discover, the website and
          all players. See <code>hidden</code> in the search bar for the current list.</div>`;
    } else {
      body = `<div class="kv" style="padding-top:6px">` +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'signature', 'block_height', 'block_hash', 'timestamp_ms']) + `</div>`;
    }
    const ctx = opts.blockHeight !== undefined
      ? `<span class="dim mini">in block <a href="#/block/${opts.blockHeight}">#${fmtInt(opts.blockHeight)}</a></span>` : '';
    return `<div class="txcard">
      <div class="txcard-head">${txChip(t)}
        <span class="h-link"><a href="#/tx/${esc(hash)}">${shortHex(hash, 14)}</a></span>${copyBtn(hash)}${ctx}
      </div>
      <div class="txcard-body">${body}${rawDetails(tx)}</div></div>`;
  }

  // ───────────────────── page scaffold ─────────────────────
  const main = $('ex-main');
  let renderSeq = 0;                 // stale-render guard

  function pageHead(title, subHtml, extraHtml) {
    return `<div class="page-head"><span class="page-title">${title}</span>
      ${subHtml ? `<span class="page-sub">${subHtml}</span>` : ''}
      <span class="spacer"></span>${extraHtml || ''}</div>`;
  }
  const loadingHtml = (msg) => `<div class="ex-loading">${esc(msg || 'Loading…')}</div>`;

  function pager(page, per, total, mkHref) {
    const pages = Math.max(1, Math.ceil(total / per));
    return `<div class="pager">
      <a class="btn" ${page > 0 ? `href="${mkHref(page - 1)}"` : 'disabled'}>&larr; Newer</a>
      <span class="pg-info">page ${page + 1} / ${fmtInt(pages)} &middot; ${fmtInt(total)} total</span>
      <a class="btn" ${page < pages - 1 ? `href="${mkHref(page + 1)}"` : 'disabled'}>Older &rarr;</a>
    </div>`;
  }

  // ─────────────────────── views ───────────────────────────
  const PER = 25;

  async function viewBlocks(params) {
    const page = Math.max(0, +(params.get('p') || 0));
    main.innerHTML = loadingHtml();
    let r;
    try { r = await exGet(`/api/blocks?offset=${page * PER}&limit=${PER}`); }
    catch (e) {
      main.innerHTML = pageHead('Blocks') +
        (notLive(e) ? stubPanel('The block list', '/api/blocks') : errPanel(e));
      return;
    }
    const rows = (r.blocks || []).map((b) => `<tr>
      <td><a href="#/block/${b.height}">#${fmtInt(b.height)}</a></td>
      <td>${blockLink(b.hash)}</td>
      <td class="dim" title="${fmtTime(b.timestamp_ms)}">${fmtAgo(b.timestamp_ms)}</td>
      <td class="num">${fmtInt(b.tx_count)}</td>
      <td class="num dim">${fmtBytes(b.size_bytes)}</td>
      <td class="num dim">${fmtInt(b.weight)}</td></tr>`).join('');
    main.innerHTML = pageHead('Blocks', `${fmtInt(r.total)} on chain, newest first`) +
      `<div class="tbl-wrap"><table class="tbl">
        <thead><tr><th>Height</th><th>Hash</th><th>Age</th><th class="num">Txs</th>
        <th class="num">Size</th><th class="num">Weight</th></tr></thead>
        <tbody>${rows}</tbody></table></div>` +
      pager(page, PER, r.total || 0, (p) => `#/blocks?p=${p}`);
  }

  function blockHeaderPanel(b) {
    const skip = ['transactions', 'txs'];
    return `<div class="panel"><div class="panel-title">Block header — every field</div>
      <div class="kv">${kvAll(b, skip)}</div>${rawDetails({ ...b, transactions: undefined }, 'Raw header JSON')}</div>`;
  }

  async function viewBlock(id) {
    main.innerHTML = loadingHtml();
    let b;
    try { b = await exGet(`/api/blocks/${encodeURIComponent(id)}`); }
    catch (e) {
      main.innerHTML = pageHead('Block') +
        (notLive(e) ? stubPanel(`Block ${esc(String(id))}`, '/api/blocks/:heightOrHash') : errPanel(e));
      return;
    }
    const txs = blockTxsOf(b);
    const nav = `
      ${b.height > 0 ? `<a class="btn" href="#/block/${b.height - 1}">&larr; #${fmtInt(b.height - 1)}</a>` : ''}
      <a class="btn" href="#/block/${b.height + 1}">#${fmtInt(b.height + 1)} &rarr;</a>`;
    main.innerHTML = pageHead(`Block #${fmtInt(b.height)}`,
        `${fmtTime(b.timestamp_ms)} &middot; ${fmtAgo(b.timestamp_ms)}`, nav) +
      blockHeaderPanel(b) +
      `<div class="panel-title" style="margin:14px 2px 8px">${fmtInt(txs.length)} transaction${txs.length === 1 ? '' : 's'}</div>` +
      (txs.length ? txs.map((t) => txCard(t)).join('')
                  : '<div class="note">This block carries no transactions.</div>');
  }

  async function viewTx(hash) {
    main.innerHTML = loadingHtml();
    let tx;
    try { tx = await exGet(`/api/tx/${encodeURIComponent(hash)}`); }
    catch (e) {
      main.innerHTML = pageHead('Transaction') +
        (notLive(e) ? stubPanel(`Transaction ${shortHex(hash)}`, '/api/tx/:hash') : errPanel(e));
      return;
    }
    main.innerHTML = pageHead('Transaction', shortHex(hash, 16)) +
      `<div class="panel"><div class="kv">` +
      (tx.block_height !== undefined
        ? kvRow('block', `<a href="#/block/${tx.block_height}">#${fmtInt(tx.block_height)}</a>` +
            (tx.block_hash ? ` &nbsp;${blockLink(tx.block_hash)}` : '')) : '') +
      (tx.timestamp_ms ? kvRow('time', `${fmtTime(tx.timestamp_ms)} <span class="dim">(${fmtAgo(tx.timestamp_ms)})</span>`) : '') +
      `</div></div>` + txCard(tx, { blockHeight: tx.block_height });
  }

  // Transactions tab — flattened from the most recent non-empty blocks.
  async function viewTxs() {
    main.innerHTML = loadingHtml('Reading recent blocks…');
    let list;
    try { list = await exGet(`/api/blocks?offset=0&limit=15`); }
    catch (e) {
      main.innerHTML = pageHead('Transactions') +
        (notLive(e) ? stubPanel('Recent transactions', '/api/blocks') : errPanel(e));
      return;
    }
    const withTx = (list.blocks || []).filter((b) => b.tx_count > 0).slice(0, 10);
    const seq = ++renderSeq;
    const blocks = await fetchBlocksByHeight(withTx.map((b) => b.height));
    if (seq !== renderSeq) return;
    const rows = [];
    for (const b of blocks)
      for (const t of [...blockTxsOf(b)].reverse())
        rows.push(`<tr><td>${txChip(txType(t))}</td>
          <td><a href="#/tx/${esc(t.hash || t.tx_hash)}">${shortHex(t.hash || t.tx_hash)}</a></td>
          <td><a href="#/block/${b.height}">#${fmtInt(b.height)}</a></td>
          <td class="dim">${fmtAgo(b.timestamp_ms)}</td><td>${txSummaryHtml(t)}</td></tr>`);
    main.innerHTML = pageHead('Transactions',
        `from the ${fmtInt(blocks.length)} most recent blocks that carry any — search reaches everything`) +
      (rows.length
        ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Type</th><th>Tx</th>
           <th>Block</th><th>Age</th><th>What happened</th></tr></thead>
           <tbody>${rows.join('')}</tbody></table></div>`
        : '<div class="note">No transactions in recent blocks.</div>') +
      `<div class="note">Looking for older activity? Try commands like
        <code>mints 100..200</code>, <code>transfers 0x…</code> or <code>tx &lt;hash&gt;</code> in the search bar.</div>`;
  }

  async function viewAddresses() {
    main.innerHTML = pageHead('Addresses') +
      `<div class="panel"><div class="panel-title">Look up a wallet</div>
        <div class="note" style="margin-bottom:10px">Paste any <code>0x…</code> address in the search
        bar (or below) to see its balance, nonce, lifetime earnings and burns, and its full
        history — every play it made, every play it was paid for, every transfer.</div>
        <form id="addr-form" style="display:flex;gap:10px;flex-wrap:wrap">
          <input class="mono" id="addr-input" style="flex:1 1 280px;padding:8px 12px;background:var(--bg-elev2);
            color:var(--text);border:1px solid var(--line);border-radius:8px;outline:none"
            placeholder="0x…40 hex characters" spellcheck="false"/>
          <button class="btn btn-solid" type="submit">Open</button></form></div>
      <div class="panel"><div class="panel-title">Wallet commands</div>
        <div class="note">The search bar answers wallet questions directly:</div>
        <div class="kv" style="margin-top:8px">
          ${kvRow('plays by <0x…>', '<span class="note">everything this wallet listened to</span>')}
          ${kvRow('earned <0x…>', '<span class="note">earnings split by lane — artist / listener / seeder / relay</span>')}
          ${kvRow('transfers <0x…>', '<span class="note">token movements in and out</span>')}
          ${kvRow('seeder <0x…>', '<span class="note">what this peer served and was paid</span>')}
          ${kvRow('relay <0x…>', '<span class="note">mini-node relay activity</span>')}
        </div></div>`;
    $('addr-form').onsubmit = (e) => {
      e.preventDefault();
      const v = $('addr-input').value.trim();
      if (/^0x[0-9a-fA-F]{40}$/.test(v)) location.hash = `#/address/${v}`;
      else toast('That is not a 0x… address (40 hex characters)');
    };
  }

  const ROLE_KEYS = ['listener', 'artist', 'seeder', 'relay', 'sender', 'recipient', 'node'];

  async function viewAddress(a, params) {
    const page = Math.max(0, +(params.get('p') || 0));
    const role = params.get('role') || '';
    main.innerHTML = loadingHtml();
    let info = null, infoErr = null;
    try { info = await exGet(`/api/address/${a}`); } catch (e) { infoErr = e; }
    let hist = null, histErr = null;
    try {
      hist = await exGet(`/api/address/${a}/history?offset=${page * PER}&limit=${PER}` +
                         (role ? `&role=${role}` : ''));
    } catch (e) { histErr = e; }

    let html = pageHead('Address', `<span class="h-link">${esc(a)}</span>${copyBtn(a)}`);
    if (info) {
      html += `<div class="tiles">
        <div class="tile"><div class="t-label">Balance</div><div class="t-value">${fmtAmt(info.balance)}</div><div class="t-sub">tokens${info.escrow_balance ? ` &middot; ${fmtAmt(info.escrow_balance)} in escrow` : ''}</div></div>
        <div class="tile"><div class="t-label">Earned</div><div class="t-value">${fmtAmt(info.earned_total)}</div><div class="t-sub">lifetime, all lanes</div></div>
        <div class="tile"><div class="t-label">Burned</div><div class="t-value">${fmtAmt(info.burned_total)}</div><div class="t-sub">tokens</div></div>
        <div class="tile"><div class="t-label">Transactions</div><div class="t-value">${fmtInt(info.tx_count)}</div><div class="t-sub">nonce ${fmtInt(info.nonce)}</div></div>
        <div class="tile"><div class="t-label">Plays as listener</div><div class="t-value">${fmtInt(info.plays_as_listener)}</div></div>
        <div class="tile"><div class="t-label">Plays as artist</div><div class="t-value">${fmtInt(info.plays_as_artist)}</div></div>
        <div class="tile"><div class="t-label">As seeder / relay</div><div class="t-value">${fmtInt(info.plays_as_seeder)} / ${fmtInt(info.plays_as_relay)}</div></div>
        <div class="tile"><div class="t-label">Seen</div><div class="t-value" style="font-size:14px">
          ${info.first_seen_height != null ? `#${fmtInt(info.first_seen_height)} → #${fmtInt(info.last_seen_height)}` : '—'}</div>
          <div class="t-sub">first → last block</div></div>
      </div>`;
      if (info.plays_as_artist > 0)
        html += `<div class="note" style="margin:-6px 0 12px">This wallet earns as an artist —
          see <a href="#/artist/${esc(a)}">its artist dashboard</a> for label-grade metrics.</div>`;
    } else {
      html += notLive(infoErr) ? stubPanel('The address summary', '/api/address/:addr') : errPanel(infoErr);
    }

    // role filter chips
    html += `<div class="page-head" style="margin-top:4px"><span class="panel-title" style="margin:0">History</span>
      <span class="page-sub">role-tagged — filter:</span>
      <a class="chip${role === '' ? ' t-mint' : ''}" href="#/address/${esc(a)}">all</a>` +
      ROLE_KEYS.map((r) =>
        `<a class="chip r-${r}${role === r ? ' t-mint' : ''}" href="#/address/${esc(a)}?role=${r}">${r}</a>`).join(' ') +
      `</div>`;

    if (hist) {
      let items = hist.history || hist.items || hist.txs || [];
      if (role) items = items.filter((it) => !it.role || it.role === role);
      const rows = items.map((it) => {
        const song = it.content_hash ? songLink(it.content_hash) : '<span class="dim">—</span>';
        return `<tr><td>${it.role ? roleChip(it.role) : ''}</td>
          <td>${txChip(txType(it))}</td>
          <td><a href="#/tx/${esc(it.tx_hash || it.hash)}">${shortHex(it.tx_hash || it.hash)}</a></td>
          <td><a href="#/block/${it.block_height}">#${fmtInt(it.block_height)}</a></td>
          <td class="dim" title="${fmtTime(it.timestamp_ms)}">${fmtAgo(it.timestamp_ms)}</td>
          <td>${song}</td><td class="num">${it.amount !== undefined ? amt(it.amount) : '<span class="dim">—</span>'}</td></tr>`;
      }).join('');
      html += rows
        ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Role</th><th>Type</th><th>Tx</th>
            <th>Block</th><th>Age</th><th>Song</th><th class="num">Amount</th></tr></thead>
            <tbody>${rows}</tbody></table></div>` +
          pager(page, PER, hist.total ?? items.length,
                (p) => `#/address/${a}?p=${p}${role ? `&role=${role}` : ''}`)
        : '<div class="note">No history entries for this filter.</div>';
    } else {
      html += notLive(histErr) ? stubPanel('The address history', '/api/address/:addr/history') : errPanel(histErr);
    }
    main.innerHTML = html;
  }

  async function viewSongs() {
    main.innerHTML = loadingHtml();
    // The player's catalog endpoint already exists on the live gateway; the
    // explorer reuses it as the Songs index and links through to on-chain
    // song records.
    let songs = null, e1 = null;
    try {
      const r = await exGet('/api/songs?limit=200');
      songs = Array.isArray(r) ? r : (r.songs || []);
    } catch (e) { e1 = e; }
    let html = pageHead('Songs', 'registered on chain');
    if (songs && songs.length) {
      const rows = songs.map((s) => {
        const h = s.contentHash || s.content_hash;
        return `<tr><td><a href="#/song/${esc(h)}">${esc(s.title || shortHex(h))}</a></td>
          <td>${esc(s.artist || '—')}</td><td class="dim">${esc(s.album || '—')}</td>
          <td class="dim">${esc(s.genre || '—')}</td><td class="num dim">${esc(String(s.year || '—'))}</td>
          <td class="num dim">${fmtDur(s.durationMs || s.duration_ms)}</td>
          <td class="num">${fmtInt(s.plays ?? s.play_count ?? 0)}</td></tr>`;
      }).join('');
      html += `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Title</th><th>Artist</th>
        <th>Album</th><th>Genre</th><th class="num">Year</th><th class="num">Length</th>
        <th class="num">Plays</th></tr></thead><tbody>${rows}</tbody></table></div>`;
    } else if (songs) {
      html += '<div class="note">No songs in the catalog right now.</div>';
    } else {
      html += (notLive(e1) ? stubPanel('The song catalog', '/api/songs') : errPanel(e1));
    }
    html += `<div class="panel"><div class="panel-title">Song commands</div>
      <div class="kv">
        ${kvRow('song <hash|title>', '<span class="note">the full on-chain record</span>')}
        ${kvRow('fingerprint <song>', '<span class="note">download the chromaprint and verify it yourself</span>')}
        ${kvRow('plays of <song>', '<span class="note">who listened, and when</span>')}
        ${kvRow('artist <name>', '<span class="note">label-grade dashboard for one artist</span>')}
        ${kvRow('genre <name>', '<span class="note">activity in a genre</span>')}
      </div></div>`;
    main.innerHTML = html;
  }

  function downloadText(name, text) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'application/octet-stream' }));
    a.download = name;
    document.body.appendChild(a); a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 500);
  }

  async function viewSong(hash, params) {
    main.innerHTML = loadingHtml();
    let s;
    try { s = await exGet(`/api/song/${encodeURIComponent(hash)}`); }
    catch (e) {
      main.innerHTML = pageHead('Song') +
        (notLive(e) ? stubPanel(`Song ${shortHex(hash)}`, '/api/song/:hash') : errPanel(e));
      return;
    }
    const fp = s.compressed_fingerprint || '';
    const metaSkip = ['compressed_fingerprint', 'play_count', 'unique_listeners', 'earned_total',
                      'holders', 'idx', 'reg_height', 'hidden', 'is_hidden'];
    // hidden status — a hide can target the title itself, or the whole artist/album
    const hid = (s.hidden && typeof s.hidden === 'object') ? s.hidden
              : (s.hidden === true || s.is_hidden === true) ? {} : null;
    const hiddenBanner = hid === null ? '' :
      `<div class="stub err-panel" style="margin-bottom:14px">
        <b>This song is currently HIDDEN network-wide.</b><br/>
        A verified moderator action on chain removed it from Discover, the website and all
        players${hid.kind && hid.kind !== 'title'
          ? ` — via a hide on the whole ${esc(hid.kind)} <b>${esc(hid.name || '')}</b>` : ''}.<br/>
        ${hid.hidden_at_height !== undefined
          ? `Hidden at <a href="#/block/${hid.hidden_at_height}">block #${fmtInt(hid.hidden_at_height)}</a>
             ${hid.timestamp_ms ? `<span class="mini">(${fmtAgo(hid.timestamp_ms)})</span>` : ''}
             ${hid.moderator_address ? ` by moderator ${addrLink(hid.moderator_address)}` : ''}
             ${hid.tx_hash ? ` — tx <a href="#/tx/${esc(hid.tx_hash)}">${shortHex(hid.tx_hash, 8)}</a>` : ''}.`
          : ''}
        <span class="mini" style="display:block;margin-top:4px">The record below stays public —
          hides curate playback surfaces, they never rewrite the chain.
          See <code>moderation</code> or <code>hidden</code> in the search bar.</span></div>`;
    main.innerHTML = pageHead(esc(s.title || 'Song'),
        `${esc(s.artist || '')}${s.album ? ` — ${esc(s.album)}` : ''}`) +
      hiddenBanner +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Plays</div><div class="t-value">${fmtInt(s.play_count)}</div></div>
        <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(s.unique_listeners)}</div></div>
        <div class="tile"><div class="t-label">Artist earnings</div><div class="t-value">${fmtAmt(s.earned_total)}</div><div class="t-sub">tokens</div></div>
        <div class="tile"><div class="t-label">Registered</div><div class="t-value" style="font-size:14px">
          ${s.registration_height != null ? `<a href="#/block/${s.registration_height}">block #${fmtInt(s.registration_height)}</a>` : '—'}</div></div>
      </div>
      <div class="panel"><div class="panel-title">On-chain record — every field</div>
        <div class="kv">${kvAll(s, metaSkip)}
        ${Array.isArray(s.holders) ? kvRow('holders / royalty splits',
          s.holders.map((h) => `${addrLink(h.address)} <span class="dim">${esc(String(h.percent ?? ''))}%</span>`).join('<br/>')) : ''}
        </div>${rawDetails(s, 'Raw record JSON')}</div>

      <div class="panel verify-panel" id="fp-panel">
        <div class="panel-title">Verify this registration yourself</div>
        <div class="note">You don't have to trust this site. The song's acoustic fingerprint
          (a <b>chromaprint</b>) is committed on chain inside the registration transaction in
          <a href="#/block/${esc(String(s.registration_height ?? ''))}">block #${fmtInt(s.registration_height)}</a>,
          and the audio itself is identified by its content hash:</div>
        <div class="hash-big">${esc(s.content_hash || hash)}${copyBtn(s.content_hash || hash)}</div>
        <button class="btn btn-solid" id="fp-dl"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3v10.6l3.3-3.3 1.4 1.4L12 17.4l-4.7-5.7 1.4-1.4 2.3 3.3V3h2zM5 19h14v2H5v-2z"/></svg>Download fingerprint</button>
        <span class="mini" style="margin-left:10px">${fp ? `${fmtInt(fp.length)} chars base64` : 'served by the gateway'}</span>
        <ol>
          <li>Download the fingerprint above (base64 chromaprint, exactly as stored on chain).</li>
          <li>Get the audio (stream it from the network), and hash it: <code>sha256sum song.mp3</code>
              — the digest must equal the content hash shown above.</li>
          <li>Compute the audio's own fingerprint with the standard tool:
              <code>fpcalc -raw song.mp3</code> (chromaprint), and compare it against the downloaded one.</li>
          <li>Anyone running a Bopwire node re-checks this automatically — a registration whose audio
              doesn't match its declared fingerprint is slashable on chain.</li>
        </ol>
      </div>
      <div class="note">More on this song from the search bar:
        <code>plays of ${esc(s.title || shortHex(hash))}</code> &middot;
        <code>artist ${esc(s.artist || '')}</code></div>`;

    $('fp-dl').onclick = async () => {
      try {
        const text = fp || await exText(`/api/song/${hash}/fingerprint`);
        downloadText(`${(s.title || hash).replace(/[^\w.-]+/g, '_')}.chromaprint.b64.txt`, text);
        toast('Fingerprint downloaded — compare it with fpcalc output');
      } catch (e) {
        toast(notLive(e) ? 'Fingerprint endpoint not live yet (/api/song/:hash/fingerprint)' : e.message);
      }
    };
    if (params && params.get('fp') === '1')
      $('fp-panel').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  async function viewArtist(id) {
    main.innerHTML = loadingHtml();
    let a;
    try { a = await exGet(`/api/stats/artist/${encodeURIComponent(id)}`); }
    catch (e) {
      main.innerHTML = pageHead('Artist') +
        (notLive(e) ? stubPanel(`Artist stats for ${esc(String(id))}`, '/api/stats/artist/:id') : errPanel(e));
      return;
    }
    const addr40 = a.artist_address || (/^0x[0-9a-fA-F]{40}$/.test(id) ? id : null);
    main.innerHTML = pageHead(esc(a.name || shortHex(String(id), 8)), 'artist dashboard') +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Total plays</div><div class="t-value">${fmtInt(a.total_plays)}</div></div>
        <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(a.unique_listeners)}</div></div>
        <div class="tile"><div class="t-label">Earned</div><div class="t-value">${fmtAmt(a.earned_total)}</div><div class="t-sub">tokens</div></div>
        <div class="tile"><div class="t-label">Songs with plays</div><div class="t-value">${fmtInt((a.plays_per_song || []).length)}</div></div>
      </div>
      <div class="panel"><div class="panel-title">Plays over time</div><div id="ar-chart"></div></div>
      <div class="charts-2col">
        <div class="panel"><div class="panel-title">Top songs</div><div id="ar-top"></div></div>
        <div class="panel"><div class="panel-title">Who serves this artist</div>
          <div class="note" style="margin-bottom:6px">Seeders (uploaded the bytes) and relays
            (mini-nodes that carried the stream) attested on this artist's plays:</div>
          ${(a.serving_seeders || []).map((s) => `<div>${roleChip('seeder')} ${addrLink(s)}</div>`).join('')}
          ${(a.serving_relays || []).map((s) => `<div style="margin-top:3px">${roleChip('relay')} ${addrLink(s)}</div>`).join('')}
        </div>
      </div>` +
      (addr40 ? `<div class="note">Wallet: ${addrLink(addr40)} — <a href="#/address/${esc(addr40)}">full history</a></div>` : '');
    const ot = (a.plays_over_time || []).map((p) => ({ t: Date.parse(p.date), v: p.plays ?? p.count ?? 0 }));
    lineChart($('ar-chart'), [{ name: 'plays', points: ot }], { label: 'plays over time' });
    const top = (a.top_songs || a.plays_per_song || []).slice(0, 10).map((s) => ({
      label: s.title || shortHex(s.content_hash), value: s.plays ?? s.count ?? 0,
      href: s.content_hash ? `#/song/${s.content_hash}` : undefined }));
    $('ar-top').innerHTML = top.length ? hbarList(top) : '<div class="note">No plays yet.</div>';
  }

  async function viewStats(params) {
    main.innerHTML = loadingHtml();
    let s;
    try { s = await exGet('/api/stats'); }
    catch (e) {
      main.innerHTML = pageHead('Network stats') +
        (notLive(e) ? stubPanel('The stats dashboard', '/api/stats') : errPanel(e));
      return;
    }
    const since = params.get('since');
    let series = s.per_day || s.timeseries || [];
    if (since && /^\d{4}-\d{2}-\d{2}$/.test(since)) series = series.filter((d) => d.date >= since);

    main.innerHTML = pageHead('Network stats',
        since ? `since ${esc(since)} — <a href="#/stats">clear</a>` : 'the whole chain at a glance') +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Height</div><div class="t-value">${fmtInt(s.height)}</div><div class="t-sub">${fmtInt(s.blocks)} blocks</div></div>
        <div class="tile"><div class="t-label">Total plays</div><div class="t-value">${fmtInt(s.total_plays)}</div><div class="t-sub">rewarded on chain</div></div>
        <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(s.unique_listeners)}</div></div>
        <div class="tile"><div class="t-label">Unique artists</div><div class="t-value">${fmtInt(s.unique_artists)}</div></div>
        <div class="tile"><div class="t-label">Unique seeders</div><div class="t-value">${fmtInt(s.unique_seeders)}</div></div>
        <div class="tile"><div class="t-label">Songs registered</div><div class="t-value">${fmtInt(s.songs_registered)}</div></div>
        <div class="tile"><div class="t-label">Minted</div><div class="t-value">${fmtAmt(s.total_minted)}</div><div class="t-sub">tokens, lifetime</div></div>
        <div class="tile"><div class="t-label">Burned</div><div class="t-value">${fmtAmt(s.total_burned)}</div><div class="t-sub">tokens, lifetime</div></div>
      </div>
      <div class="panel"><div class="panel-title">Plays &amp; mints per day
        <span class="pt-note">— hover for exact values</span></div><div id="st-chart"></div></div>
      <div class="charts-2col">
        <div class="panel"><div class="panel-title">Transactions by type</div><div id="st-types"></div></div>
        <div class="panel"><div class="panel-title">What a label would ask next</div>
          <div class="kv">
            ${kvRow('top songs 10', '<span class="note">most played songs on the network</span>')}
            ${kvRow('top artists 10', '<span class="note">most played artists</span>')}
            ${kvRow('top listeners 10', '<span class="note">heaviest listener wallets</span>')}
            ${kvRow('artist <name>', '<span class="note">one artist’s full dashboard</span>')}
            ${kvRow('mints 4000..4100', '<span class="note">every rewarded play in a block range</span>')}
            ${kvRow('burned', '<span class="note">deflation from listener burns</span>')}
          </div>
          <div class="mini" style="margin-top:8px">Type any of these into the search bar above.</div>
        </div>
      </div>` +
      (s.window_note ? `<div class="mini">${esc(s.window_note)}</div>` : '');

    const two = [
      { name: 'plays', points: series.map((d) => ({ t: Date.parse(d.date), v: d.plays ?? 0 })) },
      { name: 'mints', points: series.map((d) => ({ t: Date.parse(d.date), v: d.mints ?? 0 })) },
    ].filter((sr) => sr.points.some((p) => p.v > 0) || sr.points.length);
    lineChart($('st-chart'), two, { label: 'plays and mints per day' });

    const types = Object.entries(s.txs_by_type || {}).sort((a, b) => b[1] - a[1])
      .map(([k, v]) => ({ label: TX_LABEL[k] || k, value: v }));
    $('st-types').innerHTML = types.length ? hbarList(types) : '<div class="note">No data.</div>';
  }

  // free-text search results page
  async function viewSearch(q) {
    main.innerHTML = loadingHtml('Searching…');
    let hits;
    try { hits = await searchTyped(q); }
    catch (e) {
      main.innerHTML = pageHead('Search', esc(q)) +
        (notLive(e) ? stubPanel('Search', '/api/search') : errPanel(e));
      return;
    }
    const row = (h) => {
      if (h.type === 'block') return `<tr><td><span class="chip">block</span></td>
        <td><a href="#/block/${h.height ?? h.hash}">#${fmtInt(h.height)}</a> ${h.hash ? blockLink(h.hash) : ''}</td></tr>`;
      if (h.type === 'tx') return `<tr><td><span class="chip">tx</span></td>
        <td>${txLink(h.hash)}${h.block_height !== undefined ? ` <span class="dim">in #${fmtInt(h.block_height)}</span>` : ''}</td></tr>`;
      if (h.type === 'address') return `<tr><td><span class="chip">address</span></td><td>${addrLink(h.address)}</td></tr>`;
      if (h.type === 'artist') return `<tr><td><span class="chip r-artist">artist</span></td>
        <td><a href="#/artist/${esc(h.artist_address || h.name)}">${esc(h.name)}</a></td></tr>`;
      if (h.type === 'song') return `<tr><td><span class="chip t-song">song</span></td>
        <td><a href="#/song/${esc(h.content_hash)}">${esc(h.title || shortHex(h.content_hash))}</a>
        <span class="dim">${esc(h.artist ? `— ${h.artist}` : '')}${esc(h.album ? ` · ${h.album}` : '')}</span></td></tr>`;
      return `<tr><td><span class="chip">${esc(h.type || '?')}</span></td><td><code>${esc(JSON.stringify(h))}</code></td></tr>`;
    };
    main.innerHTML = pageHead('Search', `${fmtInt(hits.length)} result${hits.length === 1 ? '' : 's'} for &ldquo;${esc(q)}&rdquo;`) +
      (hits.length
        ? `<div class="tbl-wrap"><table class="tbl"><tbody>${hits.map(row).join('')}</tbody></table></div>`
        : `<div class="stub">No results for <b>${esc(q)}</b>. Search accepts a block height, a 64-hex
            block/tx/song hash, a <code>0x…</code> wallet address, song or artist text — or a command
            (open the <b>Commands</b> help under the search bar).</div>`);
  }

  // bare 64-hex: try tx → block → song in order
  async function viewHashLookup(hx) {
    main.innerHTML = loadingHtml('Resolving hash…');
    for (const [fn, route] of [
      [() => exGet(`/api/tx/${hx}`),     `#/tx/${hx}`],
      [() => exGet(`/api/blocks/${hx}`), `#/block/${hx}`],
      [() => exGet(`/api/song/${hx}`),   `#/song/${hx}`],
    ]) {
      try { await fn(); location.replace(route); return; }
      catch (e) { if (!(e instanceof ApiError)) throw e; }
    }
    main.innerHTML = pageHead('Not found', shortHex(hx, 16)) +
      `<div class="stub">No transaction, block or song matches
        <code>${esc(hx)}</code>. (Checked <code>/api/tx</code>, <code>/api/blocks</code>,
        <code>/api/song</code> — any of these may also simply not be live on the gateway yet.)</div>`;
  }

  // ───────────────── command bar: help data ────────────────
  const CMD_HELP = [
    { group: 'Blocks', items: [
      ['block <height|hash>',   'one block — every header field, every tx expanded'],
      ['blocks <x>..<y>',       'every block between two heights (also "100-200", "100 to 200")'],
      ['blocks last <n>',       'the most recent N blocks'],
      ['blocks since <date>',   'blocks from a date (YYYY-MM-DD)'],
    ]},
    { group: 'Transactions', items: [
      ['tx <hash>',             'one transaction, fully expanded'],
      ['mints <x>..<y>',        'play-reward (mint) txs in a block range'],
      ['transfers <0x…>',       'token movements in/out of an address'],
      ['burned [<x>..<y>]',     'burn totals, optionally over a block range'],
    ]},
    { group: 'Addresses / wallets', items: [
      ['address <0x…>',         'summary + role-tagged history'],
      ['plays by <0x…>',        'what a wallet listened to'],
      ['earned <0x…>',          'earnings split by lane: artist / listener / seeder / relay'],
      ['seeder <0x…>',          'what this peer served, and what it earned'],
      ['relay <0x…>',           'mini-node relay activity'],
    ]},
    { group: 'Artists & songs', items: [
      ['artist <name|0x…>',     'artist profile + label-grade metrics'],
      ['artist <name> blocks',  'which blocks that artist appears in'],
      ['song <hash|title>',     'full on-chain record incl. fingerprint'],
      ['plays of <song>',       'every play of one song, with listeners'],
      ['fingerprint <song>',    'download the chromaprint + the content hash to verify against'],
      ['genre <name>',          'songs and play activity in a genre'],
    ]},
    { group: 'Moderation', items: [
      ['moderation [<x>..<y>]', 'moderator actions in chain order, optionally over a block range'],
      ['hidden',                'everything currently hidden — when, and by which moderator'],
      ['hidden <name>',         'is this artist/album/title hidden, and its action history'],
      ['moderator <0x…>',       'every action a given moderator has taken'],
      ['moderators',            'current moderators and their levels'],
    ]},
    { group: 'Network / rankings', items: [
      ['top songs [n] [since <date>]',     'most played songs'],
      ['top artists [n] [since <date>]',   'most played artists'],
      ['top listeners [n] [since <date>]', 'heaviest listeners'],
      ['node <id>',             'a serving node’s attested plays'],
      ['stats [since <date>]',  'network overview'],
    ]},
  ];
  const CMD_WORDS = ['block', 'blocks', 'tx', 'mints', 'transfers', 'burned', 'address',
                     'plays', 'earned', 'seeder', 'relay', 'artist', 'song', 'fingerprint',
                     'genre', 'top', 'node', 'stats', 'help',
                     'moderation', 'hidden', 'moderator', 'moderators'];

  function lev(a, b) {
    const m = a.length, n = b.length;
    const d = Array.from({ length: m + 1 }, (_, i) => [i, ...Array(n).fill(0)]);
    for (let j = 0; j <= n; j++) d[0][j] = j;
    for (let i = 1; i <= m; i++)
      for (let j = 1; j <= n; j++)
        d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1,
                           d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    return d[m][n];
  }
  const nearestCmd = (w) => {
    let best = null, bd = 3;
    for (const c of CMD_WORDS) { const d = lev(w, c); if (d < bd) { bd = d; best = c; } }
    return best;
  };

  // ───────────────── command bar: parser ───────────────────
  const RANGE_RE = /^(\d+)\s*(?:\.\.+|[-–]|\s+to\s+)\s*(\d+)$/i;
  const ADDR_RE  = /^0x[0-9a-fA-F]{40}$/;
  const DATE_RE  = /^\d{4}-\d{2}-\d{2}$/;

  function parseRange(str) {
    const m = String(str).trim().match(RANGE_RE);
    if (!m) return null;
    let x = +m[1], y = +m[2];
    if (x > y) [x, y] = [y, x];
    return { x, y };
  }
  const usageErr = (msg, usage) => ({ kind: 'error', msg, usage });

  // Returns {kind:'route', hash} | {kind:'cmd', name, args} | {kind:'error', msg, usage}
  function parseQuery(raw) {
    const s = raw.trim().replace(/\s+/g, ' ');
    if (!s) return null;

    // ── bare forms (no command keyword) ──
    if (/^\d+$/.test(s))            return { kind: 'route', hash: `#/block/${s}` };
    const hx = s.replace(/^0x/i, '');
    if (/^[0-9a-fA-F]{64}$/.test(hx)) return { kind: 'route', hash: `#/hash/${hx.toLowerCase()}` };
    if (ADDR_RE.test(s))            return { kind: 'route', hash: `#/address/${s}` };

    const words = s.split(' ');
    const head = words[0].toLowerCase();
    const rest = words.slice(1);
    const restStr = rest.join(' ');

    switch (head) {
      case 'help': return { kind: 'cmd', name: 'help', args: {} };

      case 'block': {
        if (!rest.length) return usageErr('block needs a height or hash', 'block <height|hash>');
        const t = rest[0];
        if (/^\d+$/.test(t)) return { kind: 'route', hash: `#/block/${t}` };
        if (/^(0x)?[0-9a-fA-F]{64}$/.test(t)) return { kind: 'route', hash: `#/block/${t.replace(/^0x/i, '')}` };
        return usageErr(`"${t}" is neither a height nor a 64-hex hash`, 'block <height|hash>');
      }

      case 'blocks': {
        if (!rest.length) return { kind: 'route', hash: '#/blocks' };
        if (rest[0].toLowerCase() === 'last') {
          const n = +rest[1];
          if (!Number.isInteger(n) || n < 1) return usageErr('how many blocks?', 'blocks last <n>');
          return { kind: 'cmd', name: 'blocks_last', args: { n } };
        }
        if (rest[0].toLowerCase() === 'since') {
          if (!DATE_RE.test(rest[1] || '')) return usageErr('date must be YYYY-MM-DD', 'blocks since <YYYY-MM-DD>');
          return { kind: 'cmd', name: 'blocks_since', args: { date: rest[1] } };
        }
        const r = parseRange(restStr);
        if (r) return { kind: 'cmd', name: 'blocks_range', args: r };
        return usageErr(`couldn't read a range from "${restStr}"`,
                        'blocks <x>..<y>  ·  blocks last <n>  ·  blocks since <YYYY-MM-DD>');
      }

      case 'tx': {
        if (!/^(0x)?[0-9a-fA-F]{64}$/.test(rest[0] || ''))
          return usageErr('tx needs a 64-hex transaction hash', 'tx <hash>');
        return { kind: 'route', hash: `#/tx/${rest[0].replace(/^0x/i, '')}` };
      }

      case 'mints': {
        const r = parseRange(restStr);
        if (!r) return usageErr('mints needs a block range', 'mints <x>..<y>');
        return { kind: 'cmd', name: 'mints_range', args: r };
      }

      case 'transfers': {
        if (!ADDR_RE.test(rest[0] || '')) return usageErr('transfers needs a 0x… address', 'transfers <0x…>');
        return { kind: 'cmd', name: 'transfers', args: { addr: rest[0] } };
      }

      case 'burned': {
        if (!restStr) return { kind: 'cmd', name: 'burned', args: {} };
        const r = parseRange(restStr);
        if (!r) return usageErr(`couldn't read a range from "${restStr}"`, 'burned [<x>..<y>]');
        return { kind: 'cmd', name: 'burned', args: r };
      }

      case 'address': {
        if (!ADDR_RE.test(rest[0] || '')) return usageErr('address needs a 0x… address (40 hex)', 'address <0x…>');
        return { kind: 'route', hash: `#/address/${rest[0]}` };
      }

      case 'plays': {
        if ((rest[0] || '').toLowerCase() === 'by') {
          if (!ADDR_RE.test(rest[1] || '')) return usageErr('plays by needs a 0x… address', 'plays by <0x…>');
          return { kind: 'cmd', name: 'plays_by', args: { addr: rest[1] } };
        }
        if ((rest[0] || '').toLowerCase() === 'of') {
          const song = rest.slice(1).join(' ');
          if (!song) return usageErr('plays of needs a song hash or title', 'plays of <song>');
          return { kind: 'cmd', name: 'plays_of', args: { song } };
        }
        return usageErr('plays needs "by <address>" or "of <song>"', 'plays by <0x…>  ·  plays of <song>');
      }

      case 'earned': case 'seeder': case 'relay': {
        if (!ADDR_RE.test(rest[0] || '')) return usageErr(`${head} needs a 0x… address`, `${head} <0x…>`);
        return { kind: 'cmd', name: head, args: { addr: rest[0] } };
      }

      case 'artist': {
        if (!rest.length) return usageErr('artist needs a name or address', 'artist <name|0x…>');
        if (rest[rest.length - 1].toLowerCase() === 'blocks' && rest.length > 1)
          return { kind: 'cmd', name: 'artist_blocks', args: { id: rest.slice(0, -1).join(' ') } };
        return { kind: 'cmd', name: 'artist', args: { id: restStr } };
      }

      case 'song': {
        if (!rest.length) return usageErr('song needs a hash or title', 'song <hash|title>');
        if (/^(0x)?[0-9a-fA-F]{64}$/.test(rest[0]))
          return { kind: 'route', hash: `#/song/${rest[0].replace(/^0x/i, '')}` };
        return { kind: 'cmd', name: 'song_by_title', args: { title: restStr } };
      }

      case 'fingerprint': case 'fp': {
        if (!rest.length) return usageErr('fingerprint needs a song hash or title', 'fingerprint <song>');
        if (/^(0x)?[0-9a-fA-F]{64}$/.test(rest[0]))
          return { kind: 'route', hash: `#/song/${rest[0].replace(/^0x/i, '')}?fp=1` };
        return { kind: 'cmd', name: 'fingerprint', args: { title: restStr } };
      }

      case 'genre': {
        if (!restStr) return usageErr('genre needs a name', 'genre <name>');
        return { kind: 'cmd', name: 'genre', args: { name: restStr } };
      }

      case 'top': {
        const what = (rest[0] || '').toLowerCase();
        if (!['songs', 'artists', 'listeners'].includes(what))
          return usageErr('top needs songs, artists or listeners',
                          'top songs [n] [since <date>]  ·  top artists …  ·  top listeners …');
        let n = 10, since = null;
        for (let i = 1; i < rest.length; i++) {
          if (/^\d+$/.test(rest[i])) n = Math.min(50, +rest[i]);
          else if (rest[i].toLowerCase() === 'since' && DATE_RE.test(rest[i + 1] || '')) since = rest[++i];
        }
        return { kind: 'cmd', name: 'top', args: { what, n, since } };
      }

      case 'node': {
        if (!rest.length) return usageErr('node needs a node id (hex)', 'node <id>');
        return { kind: 'cmd', name: 'node', args: { id: rest[0].replace(/^0x/i, '') } };
      }

      case 'moderation': {
        if (!restStr) return { kind: 'cmd', name: 'moderation', args: {} };
        const r = parseRange(restStr);
        if (!r) return usageErr(`couldn't read a range from "${restStr}"`, 'moderation [<x>..<y>]');
        return { kind: 'cmd', name: 'moderation', args: r };
      }

      case 'hidden':
        return { kind: 'cmd', name: 'hidden', args: restStr ? { name: restStr } : {} };

      case 'moderator': {
        if (!ADDR_RE.test(rest[0] || ''))
          return usageErr('moderator needs a 0x… address (40 hex)', 'moderator <0x…>');
        return { kind: 'cmd', name: 'moderator', args: { addr: rest[0] } };
      }

      case 'moderators':
        return { kind: 'cmd', name: 'moderators', args: {} };

      case 'stats': {
        if ((rest[0] || '').toLowerCase() === 'since' && DATE_RE.test(rest[1] || ''))
          return { kind: 'route', hash: `#/stats?since=${rest[1]}` };
        if (rest.length && rest[0].toLowerCase() !== 'since')
          return usageErr(`stats doesn't take "${restStr}"`, 'stats [since <YYYY-MM-DD>]');
        if (rest.length) return usageErr('date must be YYYY-MM-DD', 'stats [since <YYYY-MM-DD>]');
        return { kind: 'route', hash: '#/stats' };
      }
    }

    // not a command — maybe a typo'd one, else free text search
    const near = nearestCmd(head);
    if (near && rest.length && near !== head)
      return { kind: 'cmd', name: 'freetext', args: { q: s, suggest: `${near} ${restStr}` } };
    return { kind: 'cmd', name: 'freetext', args: { q: s } };
  }

  // ───────────────── command bar: engine ───────────────────
  // Each command renders into main. List-shaped commands live at #/q/<cmd>
  // so results are linkable and the back button works.

  function cmdHead(title, sub) { return pageHead(title, sub); }
  const progressLine = (id) => `<div class="note" id="${id}"></div>`;

  async function heightsClamped(x, y) {
    const tip = await tipHeight();
    x = Math.max(0, Math.min(x, tip)); y = Math.max(0, Math.min(y, tip));
    if (x > y) [x, y] = [y, x];
    let capped = false;
    if (y - x + 1 > RANGE_CAP) { x = y - RANGE_CAP + 1; capped = true; }
    const hs = []; for (let h = y; h >= x; h--) hs.push(h);
    return { hs, x, y, capped };
  }

  function blocksTable(blocks) {
    return `<div class="tbl-wrap"><table class="tbl">
      <thead><tr><th>Height</th><th>Hash</th><th>Time</th><th class="num">Txs</th>
      <th class="num">Mints</th><th class="num">Size</th></tr></thead><tbody>` +
      blocks.map((b) => {
        const txs = blockTxsOf(b);
        const mints = txs.filter((t) => txType(t) === 'mint').length;
        return `<tr><td><a href="#/block/${b.height}">#${fmtInt(b.height)}</a></td>
          <td>${blockLink(b.hash)}</td><td class="dim">${fmtTime(b.timestamp_ms)}</td>
          <td class="num">${fmtInt(b.tx_count ?? txs.length)}</td>
          <td class="num">${fmtInt(mints)}</td>
          <td class="num dim">${fmtBytes(b.size_bytes)}</td></tr>`;
      }).join('') + '</tbody></table></div>';
  }

  async function renderBlockRange(x, y, title) {
    main.innerHTML = cmdHead(title, 'fetching…') + progressLine('prog');
    const { hs, x: cx, y: cy, capped } = await heightsClamped(x, y);
    const seq = ++renderSeq;
    let blocks;
    try {
      blocks = await fetchBlocksByHeight(hs, (d, t) => {
        const p = $('prog'); if (p) p.textContent = `fetching block ${d}/${t}…`;
      });
    } catch (e) {
      main.innerHTML = cmdHead(title) +
        (notLive(e) ? stubPanel('Block details', '/api/blocks/:heightOrHash') : errPanel(e));
      return;
    }
    if (seq !== renderSeq) return;
    const expand = blocks.length <= EXPAND_CAP;
    let totTx = 0, totMint = 0;
    for (const b of blocks) { const t = blockTxsOf(b); totTx += t.length;
      totMint += t.filter((z) => txType(z) === 'mint').length; }
    let html = cmdHead(title, `blocks #${fmtInt(cx)}–#${fmtInt(cy)} &middot; ${fmtInt(totTx)} txs, ${fmtInt(totMint)} plays`) +
      (capped ? `<div class="note">Range capped at ${RANGE_CAP} blocks (showing the newest end) — narrow the range for the rest.</div>` : '');
    if (expand) {
      for (const b of blocks) {
        const txs = blockTxsOf(b);
        html += `<div class="panel"><div class="panel-title">
            <a href="#/block/${b.height}">Block #${fmtInt(b.height)}</a>
            <span class="pt-note">— ${fmtTime(b.timestamp_ms)} · ${fmtInt(txs.length)} txs</span></div>
          <details><summary style="cursor:pointer;color:var(--text-dim);font-size:12px">Header fields</summary>
            <div class="kv">${kvAll(b, ['transactions', 'txs'])}</div></details>
          ${txs.map((t) => txCard(t, { blockHeight: b.height })).join('') ||
            '<div class="note">no transactions</div>'}</div>`;
      }
    } else {
      html += blocksTable(blocks) +
        `<div class="note">Ranges of ${EXPAND_CAP} blocks or fewer are shown fully expanded — narrow the range to see every transaction inline.</div>`;
    }
    main.innerHTML = html;
  }

  const CMD_RUNNERS = {
    help: async () => {
      main.innerHTML = cmdHead('Command reference') + helpPanelHtml(false);
    },

    blocks_range: (a) => renderBlockRange(a.x, a.y, 'Block range'),
    blocks_last: async (a) => {
      const tip = await tipHeight();
      return renderBlockRange(tip - a.n + 1, tip, `Last ${fmtInt(a.n)} blocks`);
    },

    blocks_since: async (a) => {
      const title = `Blocks since ${a.date}`;
      main.innerHTML = cmdHead(title, 'walking back from the tip…');
      const sinceMs = Date.parse(a.date + 'T00:00:00Z');
      const found = []; let offset = 0, pages = 0, hitEnd = false;
      while (pages++ < 12) {
        const r = await exGet(`/api/blocks?offset=${offset}&limit=100`);
        const bs = r.blocks || [];
        for (const b of bs) {
          if (tsMs(b.timestamp_ms) >= sinceMs) found.push(b);
          else { hitEnd = true; break; }
        }
        if (hitEnd || bs.length < 100) break;
        offset += bs.length;
      }
      main.innerHTML = cmdHead(title, `${fmtInt(found.length)} blocks`) +
        (found.length ? blocksTable(found) : '<div class="note">No blocks since that date.</div>') +
        (!hitEnd && pages > 12 ? '<div class="note">Stopped after 1,200 blocks — narrow the date.</div>' : '') +
        `<div class="note">Open any block for its full contents, or use <code>blocks x..y</code> on a
          sub-range to expand everything inline.</div>`;
    },

    mints_range: async (a) => {
      const title = 'Mints (rewarded plays)';
      main.innerHTML = cmdHead(title, 'fetching…') + progressLine('prog');
      const { hs, x, y, capped } = await heightsClamped(a.x, a.y);
      const blocks = await fetchBlocksByHeight(hs, (d, t) => {
        const p = $('prog'); if (p) p.textContent = `fetching block ${d}/${t}…`; });
      const rows = []; let minted = 0n, burned = 0n; const listeners = new Set();
      for (const b of blocks)
        for (const t of blockTxsOf(b))
          if (txType(t) === 'mint') {
            const p = t.proof || {};
            listeners.add(p.player_address);
            for (const o of (t.outputs || [])) minted += BigInt(o.amount || 0);
            burned += BigInt(t.burn_amount || 0);
            rows.push(`<tr><td><a href="#/block/${b.height}">#${fmtInt(b.height)}</a></td>
              <td><a href="#/tx/${esc(t.hash || t.tx_hash)}">${shortHex(t.hash || t.tx_hash, 8)}</a></td>
              <td>${songLink(p.content_hash)}</td><td>${addrLink(p.player_address)}</td>
              <td>${addrLink(p.artist_address)}</td>
              <td class="dim">${fmtDur(p.total_duration_ms)}</td>
              <td class="num">${t.burn_amount ? amt(t.burn_amount) : '<span class="dim">—</span>'}</td></tr>`);
          }
      main.innerHTML = cmdHead(title, `blocks #${fmtInt(x)}–#${fmtInt(y)}`) +
        (capped ? `<div class="note">Range capped at ${RANGE_CAP} blocks — narrow it for the rest.</div>` : '') +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Plays</div><div class="t-value">${fmtInt(rows.length)}</div></div>
          <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(listeners.size)}</div></div>
          <div class="tile"><div class="t-label">Minted</div><div class="t-value">${fmtAmt(minted)}</div><div class="t-sub">tokens (all lanes)</div></div>
          <div class="tile"><div class="t-label">Burned</div><div class="t-value">${fmtAmt(burned)}</div><div class="t-sub">tokens</div></div>
        </div>` +
        (rows.length
          ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Block</th><th>Tx</th><th>Song</th>
             <th>Listener</th><th>Artist</th><th>Length</th><th class="num">Burn</th></tr></thead>
             <tbody>${rows.join('')}</tbody></table></div>`
          : '<div class="note">No mint transactions in this range.</div>');
    },

    transfers: async (a) => {
      const title = 'Transfers';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let items;
      try { ({ items } = await pullHistory(a.addr, 500)); }
      catch (e) {
        main.innerHTML = cmdHead(title, esc(a.addr)) +
          (notLive(e) ? stubPanel('Address history', '/api/address/:addr/history') : errPanel(e));
        return;
      }
      const tr = items.filter((it) => txType(it) === 'transfer' ||
                                       it.role === 'sender' || it.role === 'recipient');
      let inn = 0n, out = 0n;
      const rows = tr.map((it) => {
        const isOut = it.role === 'sender';
        const v = BigInt(it.amount || 0);
        if (isOut) out += v; else inn += v;
        return `<tr><td>${roleChip(it.role || (isOut ? 'sender' : 'recipient'))}</td>
          <td><a href="#/tx/${esc(it.tx_hash || it.hash)}">${shortHex(it.tx_hash || it.hash)}</a></td>
          <td><a href="#/block/${it.block_height}">#${fmtInt(it.block_height)}</a></td>
          <td class="dim">${fmtAgo(it.timestamp_ms)}</td>
          <td class="num">${isOut ? '&minus;' : '+'}${fmtAmt(it.amount)}</td></tr>`;
      }).join('');
      main.innerHTML = cmdHead(title, `${addrLink(a.addr)} &middot; ${fmtInt(tr.length)} transfers (latest 500 history entries)`) +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Received</div><div class="t-value">+${fmtAmt(inn)}</div></div>
          <div class="tile"><div class="t-label">Sent</div><div class="t-value">&minus;${fmtAmt(out)}</div></div>
        </div>` +
        (rows ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Dir</th><th>Tx</th><th>Block</th>
                 <th>Age</th><th class="num">Amount</th></tr></thead><tbody>${rows}</tbody></table></div>`
              : '<div class="note">No transfers touch this address.</div>');
    },

    burned: async (a) => {
      if (a.x !== undefined) {
        // range version rides the mints scan
        const title = 'Burned in range';
        main.innerHTML = cmdHead(title, 'fetching…') + progressLine('prog');
        const { hs, x, y, capped } = await heightsClamped(a.x, a.y);
        const blocks = await fetchBlocksByHeight(hs, (d, t) => {
          const p = $('prog'); if (p) p.textContent = `fetching block ${d}/${t}…`; });
        let burned = 0n, minted = 0n, plays = 0;
        for (const b of blocks)
          for (const t of blockTxsOf(b))
            if (txType(t) === 'mint') {
              plays++;
              burned += BigInt(t.burn_amount || 0);
              for (const o of (t.outputs || [])) minted += BigInt(o.amount || 0);
            }
        main.innerHTML = cmdHead(title, `blocks #${fmtInt(x)}–#${fmtInt(y)}${capped ? ' (capped)' : ''}`) +
          `<div class="tiles">
            <div class="tile"><div class="t-label">Burned</div><div class="t-value">${fmtAmt(burned)}</div><div class="t-sub">tokens</div></div>
            <div class="tile"><div class="t-label">Minted</div><div class="t-value">${fmtAmt(minted)}</div><div class="t-sub">tokens</div></div>
            <div class="tile"><div class="t-label">Plays</div><div class="t-value">${fmtInt(plays)}</div></div>
            <div class="tile"><div class="t-label">Net issuance</div><div class="t-value">${fmtAmt(minted - burned)}</div><div class="t-sub">minted &minus; burned</div></div>
          </div>
          <div class="note">Burns come out of listener wallets past the 50k-play threshold —
            the deflationary side of the mint.</div>`;
        return;
      }
      const s = await exGet('/api/stats');
      main.innerHTML = cmdHead('Burned — lifetime') +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Total burned</div><div class="t-value">${fmtAmt(s.total_burned)}</div><div class="t-sub">tokens</div></div>
          <div class="tile"><div class="t-label">Total minted</div><div class="t-value">${fmtAmt(s.total_minted)}</div><div class="t-sub">tokens</div></div>
          <div class="tile"><div class="t-label">Net issuance</div><div class="t-value">${fmtAmt(BigInt(s.total_minted || 0) - BigInt(s.total_burned || 0))}</div></div>
        </div>
        <div class="note">Add a range for a window: <code>burned 4000..4100</code>.</div>`;
    },

    plays_by: async (a) => {
      const title = 'Plays by wallet';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let items;
      try { ({ items } = await pullHistory(a.addr, 500)); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('Address history', '/api/address/:addr/history') : errPanel(e));
        return;
      }
      const plays = items.filter((it) => it.role === 'listener');
      const bySong = new Map();
      for (const p of plays)
        bySong.set(p.content_hash, (bySong.get(p.content_hash) || 0) + 1);
      const rows = plays.slice(0, 100).map((p) => `<tr>
        <td>${songLink(p.content_hash)}</td>
        <td><a href="#/block/${p.block_height}">#${fmtInt(p.block_height)}</a></td>
        <td class="dim" title="${fmtTime(p.timestamp_ms)}">${fmtAgo(p.timestamp_ms)}</td>
        <td><a href="#/tx/${esc(p.tx_hash || p.hash)}">${shortHex(p.tx_hash || p.hash, 8)}</a></td></tr>`).join('');
      const fav = [...bySong.entries()].sort((x, y) => y[1] - x[1]).slice(0, 8)
        .map(([h, n]) => ({ label: shortHex(h), value: n, href: `#/song/${h}`, hash: h }));
      main.innerHTML = cmdHead(title,
          `${addrLink(a.addr)} &middot; ${fmtInt(plays.length)} plays in the latest 500 history entries`) +
        (fav.length ? `<div class="panel"><div class="panel-title">Most played by this wallet</div>
           <div id="fav-bars">${hbarList(fav)}</div></div>` : '') +
        (rows ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Song</th><th>Block</th>
                 <th>When</th><th>Tx</th></tr></thead><tbody>${rows}</tbody></table></div>`
              : '<div class="note">This wallet has no plays as a listener in its recent history.</div>');
      // enrich the favourite bars with real titles, best effort
      for (const f of fav) {
        const t = await songTitle(f.hash);
        if (t) {
          const el = document.querySelector(`#fav-bars a[href="#/song/${f.hash}"]`);
          if (el) el.textContent = t;
        }
      }
    },

    earned: async (a) => {
      const title = 'Earnings by lane';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let info = null, items = [];
      try { info = await exGet(`/api/address/${a.addr}`); } catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The address summary', '/api/address/:addr') : errPanel(e));
        return;
      }
      try { ({ items } = await pullHistory(a.addr, 500)); } catch (_) {}
      const lanes = { artist: 0n, listener: 0n, seeder: 0n, relay: 0n };
      for (const it of items)
        if (it.role in lanes && it.amount !== undefined) lanes[it.role] += BigInt(it.amount);
      const laneRows = Object.entries(lanes).filter(([, v]) => v > 0n)
        .map(([k, v]) => ({ label: k, value: Number(v / UNITS), valueLabel: `${fmtAmt(v)} tok` }));
      main.innerHTML = cmdHead(title, addrLink(a.addr)) +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Earned (lifetime)</div><div class="t-value">${fmtAmt(info.earned_total)}</div><div class="t-sub">tokens</div></div>
          <div class="tile"><div class="t-label">Burned</div><div class="t-value">${fmtAmt(info.burned_total)}</div><div class="t-sub">tokens</div></div>
          <div class="tile"><div class="t-label">Plays: listener</div><div class="t-value">${fmtInt(info.plays_as_listener)}</div></div>
          <div class="tile"><div class="t-label">artist / seeder / relay</div>
            <div class="t-value" style="font-size:15px">${fmtInt(info.plays_as_artist)} / ${fmtInt(info.plays_as_seeder)} / ${fmtInt(info.plays_as_relay)}</div></div>
        </div>` +
        (laneRows.length
          ? `<div class="panel"><div class="panel-title">Lane breakdown
               <span class="pt-note">— from the latest ${fmtInt(items.length)} history entries</span></div>
             ${hbarList(laneRows)}</div>`
          : '<div class="note">No lane-tagged earnings in recent history.</div>') +
        `<div class="note">Full role-tagged history: <a href="#/address/${esc(a.addr)}">address page</a>.</div>`;
    },

    seeder: (a) => roleActivity(a.addr, 'seeder',
      'Plays this peer SERVED the bytes for — each one paid a seeder-lane reward.'),
    relay: (a) => roleActivity(a.addr, 'relay',
      'Plays this mini-node RELAYED — each one paid a relay-lane reward.'),

    artist: async (a) => {
      if (ADDR_RE.test(a.id)) { location.replace(`#/artist/${a.id}`); return; }
      main.innerHTML = loadingHtml('Resolving artist…');
      const hit = await resolveArtist(a.id);
      if (hit) { location.replace(`#/artist/${encodeURIComponent(hit)}`); return; }
      main.innerHTML = cmdHead('Artist', esc(a.id)) +
        `<div class="stub">No artist matched <b>${esc(a.id)}</b> via <code>/api/search</code> —
          or search isn't returning typed artist hits yet. You can always open an artist by wallet:
          <code>artist 0x…</code>.</div>`;
    },

    artist_blocks: async (a) => {
      const title = 'Blocks containing artist';
      main.innerHTML = loadingHtml('Resolving artist…');
      let addr = ADDR_RE.test(a.id) ? a.id : await resolveArtist(a.id, true);
      if (!addr || !ADDR_RE.test(addr)) {
        main.innerHTML = cmdHead(title, esc(a.id)) +
          `<div class="stub">Couldn't resolve <b>${esc(a.id)}</b> to a wallet address.
            Try <code>artist 0x… blocks</code> with the artist's address (find it on their
            dashboard: <code>artist ${esc(a.id)}</code>).</div>`;
        return;
      }
      main.innerHTML = cmdHead(title) + loadingHtml('Reading history…');
      let items;
      try { ({ items } = await pullHistory(addr, 1000)); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('Address history', '/api/address/:addr/history') : errPanel(e));
        return;
      }
      const mine = items.filter((it) => it.role === 'artist');
      const byBlock = new Map();
      for (const it of mine) {
        const cur = byBlock.get(it.block_height) || { n: 0, ts: it.timestamp_ms };
        cur.n++; byBlock.set(it.block_height, cur);
      }
      const rows = [...byBlock.entries()].sort((x, y) => y[0] - x[0]).map(([h, v]) =>
        `<tr><td><a href="#/block/${h}">#${fmtInt(h)}</a></td>
         <td class="dim" title="${fmtTime(v.ts)}">${fmtAgo(v.ts)}</td>
         <td class="num">${fmtInt(v.n)}</td></tr>`).join('');
      main.innerHTML = cmdHead(title, `${esc(a.id)} — ${addrLink(addr)}`) +
        `<div class="note" style="margin-bottom:10px">${fmtInt(byBlock.size)} blocks carry activity
          for this artist (registrations or rewarded plays), from the latest
          ${fmtInt(items.length)} history entries.</div>` +
        (rows ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Block</th><th>Age</th>
                 <th class="num">Artist txs in block</th></tr></thead><tbody>${rows}</tbody></table></div>`
              : '<div class="note">No on-chain activity found for this artist.</div>');
    },

    song_by_title: async (a) => {
      main.innerHTML = loadingHtml('Searching songs…');
      const hits = (await searchTyped(a.title)).filter((h) => h.type === 'song');
      if (hits.length === 1) { location.replace(`#/song/${hits[0].content_hash}`); return; }
      if (!hits.length) {
        main.innerHTML = cmdHead('Song', esc(a.title)) +
          `<div class="stub">No song matched <b>${esc(a.title)}</b>. If you have the 64-hex
            content hash, <code>song &lt;hash&gt;</code> goes straight to the record.</div>`;
        return;
      }
      main.innerHTML = cmdHead('Songs matching', esc(a.title)) +
        `<div class="tbl-wrap"><table class="tbl"><tbody>` + hits.map((h) =>
          `<tr><td><span class="chip t-song">song</span></td>
           <td><a href="#/song/${esc(h.content_hash)}">${esc(h.title)}</a>
           <span class="dim">${esc(h.artist ? `— ${h.artist}` : '')}</span></td></tr>`).join('') +
        `</tbody></table></div>`;
    },

    fingerprint: async (a) => {
      main.innerHTML = loadingHtml('Searching songs…');
      const hits = (await searchTyped(a.title)).filter((h) => h.type === 'song');
      if (hits.length === 1) { location.replace(`#/song/${hits[0].content_hash}?fp=1`); return; }
      if (hits.length) {
        main.innerHTML = cmdHead('Which song?', esc(a.title)) +
          `<div class="tbl-wrap"><table class="tbl"><tbody>` + hits.map((h) =>
            `<tr><td><a href="#/song/${esc(h.content_hash)}?fp=1">${esc(h.title)}</a>
             <span class="dim">${esc(h.artist ? `— ${h.artist}` : '')}</span></td></tr>`).join('') +
          `</tbody></table></div>`;
      } else {
        main.innerHTML = cmdHead('Fingerprint', esc(a.title)) +
          `<div class="stub">No song matched <b>${esc(a.title)}</b> —
            try the 64-hex content hash: <code>fingerprint &lt;hash&gt;</code>.</div>`;
      }
    },

    plays_of: async (a) => {
      const title = 'Plays of song';
      main.innerHTML = loadingHtml('Resolving song…');
      let hash = /^[0-9a-fA-F]{64}$/.test(a.song) ? a.song.toLowerCase() : null;
      let meta = null;
      if (!hash) {
        const hits = (await searchTyped(a.song)).filter((h) => h.type === 'song');
        if (!hits.length) {
          main.innerHTML = cmdHead(title, esc(a.song)) +
            `<div class="stub">No song matched <b>${esc(a.song)}</b>.</div>`;
          return;
        }
        hash = hits[0].content_hash; meta = hits[0];
      }
      try { meta = await exGet(`/api/song/${hash}`); } catch (_) {}
      main.innerHTML = cmdHead(title, meta ? `${esc(meta.title)} — ${esc(meta.artist || '')}` : shortHex(hash)) +
        progressLine('prog');

      // primary: the dedicated endpoint (full history); fallback: client scan
      let plays = null, total = 0, full = false;
      try {
        const r = await exGet(`/api/song/${hash}/plays?offset=0&limit=200`);
        plays = r.plays || r.items || [];
        total = r.total ?? plays.length; full = true;
      } catch (e) { if (!notLive(e)) throw e; }
      if (!plays) {
        const tip = await tipHeight();
        const hs = []; for (let h = tip; h > Math.max(-1, tip - SCAN_N); h--) hs.push(h);
        const blocks = await fetchBlocksByHeight(hs, (d, t) => {
          const p = $('prog'); if (p) p.textContent = `scanning block ${d}/${t}…`; });
        plays = [];
        for (const b of blocks)
          for (const t of blockTxsOf(b))
            if (txType(t) === 'mint' && (t.proof || {}).content_hash === hash)
              plays.push({ player_address: t.proof.player_address,
                           seeder_address: t.proof.seeder_address,
                           block_height: b.height, timestamp_ms: t.proof.play_end_timestamp,
                           total_duration_ms: t.proof.total_duration_ms,
                           tx_hash: t.hash || t.tx_hash });
        total = plays.length;
      }
      const listeners = new Set(plays.map((p) => p.player_address));
      const rows = plays.map((p) => `<tr><td>${addrLink(p.player_address)}</td>
        <td><a href="#/block/${p.block_height}">#${fmtInt(p.block_height)}</a></td>
        <td class="dim" title="${fmtTime(p.timestamp_ms)}">${fmtAgo(p.timestamp_ms)}</td>
        <td class="dim">${fmtDur(p.total_duration_ms)}</td>
        <td>${p.seeder_address ? addrLink(p.seeder_address) : '<span class="dim">—</span>'}</td>
        <td><a href="#/tx/${esc(p.tx_hash)}">${shortHex(p.tx_hash, 8)}</a></td></tr>`).join('');
      main.innerHTML = cmdHead(title,
          meta ? `<a href="#/song/${hash}">${esc(meta.title || shortHex(hash))}</a> — ${esc(meta.artist || '')}` : songLink(hash)) +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Plays${full ? '' : ' (window)'}</div><div class="t-value">${fmtInt(total)}</div>
            <div class="t-sub">${full ? 'full chain history' : `last ${SCAN_N} blocks`}</div></div>
          <div class="tile"><div class="t-label">Listeners${full ? ' (listed)' : ' (window)'}</div><div class="t-value">${fmtInt(listeners.size)}</div></div>
          ${meta ? `<div class="tile"><div class="t-label">Plays (lifetime)</div><div class="t-value">${fmtInt(meta.play_count)}</div></div>
          <div class="tile"><div class="t-label">Listeners (lifetime)</div><div class="t-value">${fmtInt(meta.unique_listeners)}</div></div>` : ''}
        </div>` +
        (rows
          ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Listener</th><th>Block</th>
             <th>When</th><th>Length</th><th>Seeder</th><th>Tx</th></tr></thead>
             <tbody>${rows}</tbody></table></div>`
          : `<div class="note">No plays of this song${full ? '' : ` in the last ${SCAN_N} blocks`}.</div>`) +
        (full
          ? (total > plays.length
              ? `<div class="note">Showing the newest ${fmtInt(plays.length)} of ${fmtInt(total)} plays.</div>` : '')
          : `<div class="note">Fallback list, computed client-side from the newest ${SCAN_N} blocks —
             <code>GET /api/song/:hash/plays</code> wasn't reachable.</div>`);
    },

    genre: async (a) => {
      main.innerHTML = loadingHtml('Searching…');
      const hits = (await searchTyped(a.name)).filter((h) => h.type === 'song');
      main.innerHTML = cmdHead('Genre', esc(a.name)) +
        (hits.length
          ? `<div class="tbl-wrap"><table class="tbl"><tbody>` + hits.map((h) =>
              `<tr><td><span class="chip t-song">song</span></td>
               <td><a href="#/song/${esc(h.content_hash)}">${esc(h.title)}</a>
               <span class="dim">${esc(h.artist ? `— ${h.artist}` : '')}</span></td></tr>`).join('') +
            `</tbody></table></div>
            <div class="note">Open a song for its play metrics, or <code>plays of &lt;title&gt;</code> for listeners.</div>`
          : `<div class="stub">Nothing matched genre <b>${esc(a.name)}</b> via <code>/api/search</code>.</div>`);
    },

    top: async (a) => {
      const title = `Top ${a.what}`;

      // primary: the dedicated ranking endpoint (full history)
      try {
        const r = await exGet(`/api/top?kind=${a.what}&n=${a.n}${a.since ? `&since=${a.since}` : ''}`);
        const entries = r.entries || r.results || [];
        const rows = entries.map((e) => {
          const id = e.id || e.content_hash || e.address;
          return { label: e.name || shortHex(id, 8), value: e.plays ?? e.count ?? 0, hash: id,
                   href: a.what === 'songs' ? `#/song/${id}`
                       : a.what === 'artists' ? `#/artist/${id}` : `#/address/${id}` };
        });
        main.innerHTML = cmdHead(title,
            `${a.since ? `since ${esc(a.since)} — ` : ''}full chain history`) +
          (rows.length ? `<div class="panel" id="top-bars">${hbarList(rows)}</div>`
                       : '<div class="note">No plays recorded.</div>');
        if (a.what === 'songs')
          for (const r2 of rows)
            if (!r2.label || /…/.test(r2.label)) {
              const t = await songTitle(r2.hash);
              if (t) {
                const el = document.querySelector(`#top-bars a[href="#/song/${r2.hash}"]`);
                if (el) el.textContent = t;
              }
            }
        return;
      } catch (e) { if (!notLive(e)) throw e; }

      // fallback: client-side scan of recent blocks
      main.innerHTML = cmdHead(title, 'scanning recent blocks…') + progressLine('prog');
      const sinceMs = a.since ? Date.parse(a.since + 'T00:00:00Z') : null;
      const tip = await tipHeight();
      const hs = []; for (let h = tip; h > Math.max(-1, tip - SCAN_N); h--) hs.push(h);
      const blocks = await fetchBlocksByHeight(hs, (d, t) => {
        const p = $('prog'); if (p) p.textContent = `scanning block ${d}/${t}…`; });
      const counts = new Map();
      let scanned = 0;
      for (const b of blocks) {
        if (sinceMs && tsMs(b.timestamp_ms) < sinceMs) continue;
        scanned++;
        for (const t of blockTxsOf(b))
          if (txType(t) === 'mint') {
            const p = t.proof || {};
            const key = a.what === 'songs' ? p.content_hash
                      : a.what === 'artists' ? p.artist_address : p.player_address;
            if (key) counts.set(key, (counts.get(key) || 0) + 1);
          }
      }
      const top = [...counts.entries()].sort((x, y) => y[1] - x[1]).slice(0, a.n);
      const rows = top.map(([k, v]) => ({
        label: shortHex(k, 8), value: v,
        href: a.what === 'songs' ? `#/song/${k}` : a.what === 'artists' ? `#/artist/${k}` : `#/address/${k}`,
        hash: k }));
      main.innerHTML = cmdHead(title,
          `${a.since ? `since ${esc(a.since)}, ` : ''}from the newest ${fmtInt(scanned)} blocks`) +
        (rows.length ? `<div class="panel" id="top-bars">${hbarList(rows)}</div>`
                     : '<div class="note">No plays found in the scan window.</div>') +
        `<div class="note">Fallback ranking, computed client-side over the most recent
          ${SCAN_N} blocks — <code>GET /api/top?kind=${esc(a.what)}</code> wasn't reachable.</div>`;
      if (a.what === 'songs')
        for (const r of rows) {
          const t = await songTitle(r.hash);
          if (t) {
            const el = document.querySelector(`#top-bars a[href="#/song/${r.hash}"]`);
            if (el) el.textContent = t;
          }
        }
    },

    node: async (a) => {
      const title = 'Serving node';
      // try a dedicated endpoint first, in case the gateway grows one
      try {
        const r = await exGet(`/api/node/${a.id}`);
        const plays = r.plays || r.attested_plays || [];
        main.innerHTML = cmdHead(title, `<span class="h-link">${esc(a.id)}</span>${copyBtn(a.id)}`) +
          `<div class="tiles"><div class="tile"><div class="t-label">Attested plays</div>
            <div class="t-value">${fmtInt(r.total ?? plays.length)}</div><div class="t-sub">full chain history</div></div></div>` +
          (plays.length
            ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Song</th><th>Listener</th>
               <th>Block</th><th>When</th></tr></thead><tbody>` +
              plays.map((p) => `<tr><td>${songLink(p.content_hash)}</td>
                <td>${addrLink(p.player_address)}</td>
                <td><a href="#/block/${p.block_height}">#${fmtInt(p.block_height)}</a></td>
                <td class="dim">${fmtAgo(p.timestamp_ms)}</td></tr>`).join('') +
              '</tbody></table></div>'
            : '<div class="note">No plays attested by this node.</div>');
        return;
      } catch (e) { if (!notLive(e)) throw e; }
      main.innerHTML = cmdHead(title, shortHex(a.id, 10)) + progressLine('prog');
      const tip = await tipHeight();
      const hs = []; for (let h = tip; h > Math.max(-1, tip - SCAN_N); h--) hs.push(h);
      const blocks = await fetchBlocksByHeight(hs, (d, t) => {
        const p = $('prog'); if (p) p.textContent = `scanning block ${d}/${t}…`; });
      const rows = [];
      for (const b of blocks)
        for (const t of blockTxsOf(b))
          if (txType(t) === 'mint' && (t.proof || {}).serving_node_id?.toLowerCase() === a.id.toLowerCase())
            rows.push(`<tr><td>${songLink(t.proof.content_hash)}</td>
              <td>${addrLink(t.proof.player_address)}</td>
              <td><a href="#/block/${b.height}">#${fmtInt(b.height)}</a></td>
              <td class="dim">${fmtAgo(b.timestamp_ms)}</td></tr>`);
      main.innerHTML = cmdHead(title, `<span class="h-link">${esc(a.id)}</span>${copyBtn(a.id)}`) +
        `<div class="tiles"><div class="tile"><div class="t-label">Attested plays (window)</div>
          <div class="t-value">${fmtInt(rows.length)}</div><div class="t-sub">last ${SCAN_N} blocks</div></div></div>` +
        (rows.length
          ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Song</th><th>Listener</th>
             <th>Block</th><th>Age</th></tr></thead><tbody>${rows.join('')}</tbody></table></div>`
          : `<div class="note">No plays attested by this node in the last ${SCAN_N} blocks.</div>`) +
        `<div class="note">Fallback list from the newest ${SCAN_N} blocks —
          full node history needs a <code>GET /api/node/:id</code> endpoint.</div>`;
    },

    moderation: async (a) => {
      const ranged = a.x !== undefined;
      const title = 'Moderation log';
      main.innerHTML = cmdHead(title) + loadingHtml();
      let actions;
      try { actions = await pullModeration(1000); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The moderation log', '/api/moderation') : errPanel(e));
        return;
      }
      if (ranged) actions = actions.filter((x) => x.block_height >= a.x && x.block_height <= a.y);
      main.innerHTML = cmdHead(title,
          ranged ? `blocks #${fmtInt(a.x)}–#${fmtInt(a.y)} &middot; ${fmtInt(actions.length)} actions`
                 : `${fmtInt(actions.length)} actions, in chain order`) +
        `<div class="note" style="margin-bottom:10px">Hides are how takedowns propagate: a verified
          moderator action goes on chain, every node applies it, and the target stops surfacing
          anywhere. This log is public chain data.</div>` +
        modActionsTable(actions) +
        `<div class="note">Also: <code>hidden</code> for what's hidden right now,
          <code>moderators</code> for who holds moderator status.</div>`;
    },

    hidden: async (a) => {
      const title = a.name ? `Hidden? — ${a.name}` : 'Currently hidden';
      main.innerHTML = cmdHead(esc(title)) + loadingHtml();
      let list;
      try { list = (await exGet('/api/moderation/hidden')).hidden || []; }
      catch (e) {
        main.innerHTML = cmdHead(esc(title)) +
          (notLive(e) ? stubPanel('The hidden list', '/api/moderation/hidden') : errPanel(e));
        return;
      }
      const hiddenRows = (items) => `<div class="tbl-wrap"><table class="tbl"><thead><tr>
          <th>Kind</th><th>Name</th><th>Hidden at</th><th>When</th><th>By moderator</th><th>Tx</th>
        </tr></thead><tbody>` + items.map((h) => `<tr>
          <td><span class="chip">${esc(h.kind)}</span></td>
          <td>${h.content_hash
              ? `<a href="#/song/${esc(h.content_hash)}">${esc(h.name)}</a>` : `<b>${esc(h.name)}</b>`}
            ${h.artist ? `<span class="dim"> by ${esc(h.artist)}</span>` : ''}</td>
          <td><a href="#/block/${h.hidden_at_height}">#${fmtInt(h.hidden_at_height)}</a></td>
          <td class="dim" title="${fmtTime(h.timestamp_ms)}">${fmtAgo(h.timestamp_ms)}</td>
          <td>${addrLink(h.moderator_address)}</td>
          <td><a href="#/tx/${esc(h.tx_hash)}">${shortHex(h.tx_hash, 8)}</a></td></tr>`).join('') +
        '</tbody></table></div>';

      if (!a.name) {
        main.innerHTML = cmdHead('Currently hidden',
            `${fmtInt(list.length)} target${list.length === 1 ? '' : 's'} — artists, albums, titles`) +
          `<div class="note" style="margin-bottom:10px">Everything a moderator action currently
            hides from Discover, the website and all players. Each entry links to the block and
            transaction that hid it — public transparency, straight from the chain.</div>` +
          (list.length ? hiddenRows(list) : '<div class="note">Nothing is hidden right now.</div>') +
          `<div class="note"><code>hidden &lt;name&gt;</code> checks one artist/album/title and
            shows its full action history.</div>`;
        return;
      }

      const low = a.name.toLowerCase();
      const matches = list.filter((h) =>
        String(h.name || '').toLowerCase().includes(low) ||
        String(h.artist || '').toLowerCase().includes(low));
      let history = [];
      try {
        history = (await pullModeration(1000)).filter((x) => x.target &&
          (String(x.target.name || '').toLowerCase().includes(low) ||
           String(x.target.artist || '').toLowerCase().includes(low)));
      } catch (_) {}
      main.innerHTML = cmdHead(`Hidden? — ${esc(a.name)}`) +
        (matches.length
          ? `<div class="stub err-panel" style="margin-bottom:14px"><b>Yes — currently hidden.</b><br/>
              ${fmtInt(matches.length)} matching target${matches.length === 1 ? '' : 's'}:</div>` +
            hiddenRows(matches)
          : `<div class="panel verify-panel"><b style="color:var(--good)">Not currently hidden.</b>
             <span class="note">No hide action matching &ldquo;${esc(a.name)}&rdquo; is in force.</span></div>`) +
        (history.length
          ? `<div class="panel-title" style="margin:14px 2px 8px">Action history for this name</div>` +
            modActionsTable(history)
          : '<div class="note">No moderation actions reference this name.</div>');
    },

    moderator: async (a) => {
      const title = 'Moderator';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let r;
      try { r = await exGet(`/api/moderation/moderator/${a.addr}`); }
      catch (e) {
        main.innerHTML = cmdHead(title, esc(a.addr)) +
          (notLive(e) ? stubPanel('Per-moderator actions', '/api/moderation/moderator/:addr') : errPanel(e));
        return;
      }
      const mod = r.moderator || {};
      const actions = r.actions || [];
      main.innerHTML = cmdHead(title, addrLink(a.addr)) +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Current level</div>
            <div class="t-value" style="font-size:16px">${esc(mod.level_name || levelName(mod.level))}</div></div>
          <div class="tile"><div class="t-label">Actions on chain</div>
            <div class="t-value">${fmtInt(r.total ?? actions.length)}</div></div>
        </div>` +
        modActionsTable(actions, { noModerator: true }) +
        `<div class="note">This wallet's non-moderation activity:
          <a href="#/address/${esc(a.addr)}">address page</a>.</div>`;
    },

    moderators: async () => {
      const title = 'Moderators';
      main.innerHTML = cmdHead(title) + loadingHtml();
      let actions;
      try { actions = await pullModeration(1000); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The moderation log', '/api/moderation') : errPanel(e));
        return;
      }
      // replay grants/revokes → who currently holds status (public chain data)
      const cur = new Map();
      for (const x of actions) {
        const who = x.target?.address;
        if (!who) continue;
        if (x.action === 'grant')
          cur.set(who, { address: who, level: x.level ?? x.target.level,
                         granted_at: x.block_height, granted_by: x.moderator_address,
                         tx_hash: x.tx_hash, timestamp_ms: x.timestamp_ms });
        if (x.action === 'revoke') cur.delete(who);
      }
      const mods = [...cur.values()].sort((p, q) => (q.level || 0) - (p.level || 0));
      main.innerHTML = cmdHead(title, `${fmtInt(mods.length)} active — derived from the on-chain grant/revoke log`) +
        (mods.length
          ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Level</th><th>Address</th>
             <th>Granted at</th><th>When</th><th>Granted by</th><th>Tx</th></tr></thead><tbody>` +
            mods.map((mo) => `<tr>
              <td><span class="chip t-nodeauth">${esc(levelName(mo.level))}</span></td>
              <td>${addrLink(mo.address)}</td>
              <td><a href="#/block/${mo.granted_at}">#${fmtInt(mo.granted_at)}</a></td>
              <td class="dim" title="${fmtTime(mo.timestamp_ms)}">${fmtAgo(mo.timestamp_ms)}</td>
              <td>${addrLink(mo.granted_by)}</td>
              <td><a href="#/tx/${esc(mo.tx_hash)}">${shortHex(mo.tx_hash, 8)}</a></td></tr>`).join('') +
            '</tbody></table></div>'
          : '<div class="note">No active moderators found in the log.</div>') +
        `<div class="note">FOUNDER can grant/revoke; OP proposes hides and votes; VOICE observes.
          Moderator identity on chain is only (address, level, pubkey) — never a name.
          <code>moderator &lt;0x…&gt;</code> lists one moderator's actions.</div>`;
    },

    freetext: async (a) => {
      if (a.suggest)
        main.innerHTML = cmdHead('Search', esc(a.q)) +
          `<div class="note" style="margin-bottom:10px">Did you mean
            <a href="#/q/${encodeURIComponent(a.suggest)}"><code>${esc(a.suggest)}</code></a>?
            Running it as free-text search instead:</div>`;
      await viewSearch(a.q);
      if (a.suggest) {
        // viewSearch replaced main — re-prepend the suggestion line
        main.insertAdjacentHTML('afterbegin',
          `<div class="note" style="margin-bottom:10px">Did you mean
            <a href="#/q/${encodeURIComponent(a.suggest)}"><code>${esc(a.suggest)}</code></a>?</div>`);
      }
    },
  };

  async function roleActivity(addr40, role, blurb) {
    const title = role === 'seeder' ? 'Seeder activity' : 'Relay activity';
    main.innerHTML = cmdHead(title, esc(addr40)) + loadingHtml();
    let items;
    try { ({ items } = await pullHistory(addr40, 500)); }
    catch (e) {
      main.innerHTML = cmdHead(title) +
        (notLive(e) ? stubPanel('Address history', '/api/address/:addr/history') : errPanel(e));
      return;
    }
    const mine = items.filter((it) => it.role === role);
    let earned = 0n;
    for (const it of mine) if (it.amount !== undefined) earned += BigInt(it.amount);
    const rows = mine.slice(0, 100).map((it) => `<tr>
      <td>${it.content_hash ? songLink(it.content_hash) : '<span class="dim">—</span>'}</td>
      <td><a href="#/block/${it.block_height}">#${fmtInt(it.block_height)}</a></td>
      <td class="dim" title="${fmtTime(it.timestamp_ms)}">${fmtAgo(it.timestamp_ms)}</td>
      <td class="num">${it.amount !== undefined ? amt(it.amount) : '—'}</td>
      <td><a href="#/tx/${esc(it.tx_hash || it.hash)}">${shortHex(it.tx_hash || it.hash, 8)}</a></td></tr>`).join('');
    main.innerHTML = cmdHead(title, addrLink(addr40)) +
      `<div class="note" style="margin-bottom:10px">${esc(blurb)}</div>
      <div class="tiles">
        <div class="tile"><div class="t-label">Plays as ${role}</div><div class="t-value">${fmtInt(mine.length)}</div><div class="t-sub">latest 500 history entries</div></div>
        <div class="tile"><div class="t-label">Earned in lane</div><div class="t-value">${fmtAmt(earned)}</div><div class="t-sub">tokens</div></div>
      </div>` +
      (rows ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Song</th><th>Block</th>
               <th>When</th><th class="num">Reward</th><th>Tx</th></tr></thead><tbody>${rows}</tbody></table></div>`
            : `<div class="note">No ${role} activity in recent history.</div>`);
  }

  async function resolveArtist(text, wantAddress) {
    try {
      const hits = await searchTyped(text);
      const art = hits.find((h) => h.type === 'artist');
      if (art) return art.artist_address || (!wantAddress && art.name) || art.artist_address;
      const sg = hits.find((h) => h.type === 'song' && h.artist &&
        h.artist.toLowerCase() === text.toLowerCase());
      if (sg && sg.artist_address) return sg.artist_address;
      if (!wantAddress && sg) return sg.artist;
    } catch (_) {}
    return null;
  }

  async function viewCommand(cmdStr) {
    const parsed = parseQuery(cmdStr);
    if (!parsed) { location.hash = '#/blocks'; return; }
    if (parsed.kind === 'route') { location.replace(parsed.hash); return; }
    if (parsed.kind === 'error') {
      main.innerHTML = pageHead('Command error') +
        `<div class="stub err-panel"><b>${esc(parsed.msg)}</b><br/>
          Usage: <code>${esc(parsed.usage)}</code><br/>
          <span class="mini">Open the <b>Commands</b> reference under the search bar for all 24 commands.</span></div>` +
        helpPanelHtml(false);
      return;
    }
    const run = CMD_RUNNERS[parsed.name];
    if (!run) { main.innerHTML = errPanel(new Error(`unknown command ${parsed.name}`)); return; }
    try { await run(parsed.args); }
    catch (e) {
      if (e instanceof ApiError)
        main.innerHTML = pageHead('Command failed', esc(cmdStr)) +
          (notLive(e) ? stubPanel('A required endpoint', e.path || '') : errPanel(e));
      else throw e;
    }
  }

  // ─────────────── command help panel (UI) ─────────────────
  function helpPanelHtml(compact) {
    return CMD_HELP.map((g) =>
      `<div class="cmd-group"><div class="cmd-group-title">${esc(g.group)}</div>` +
      g.items.map(([u, d]) =>
        `<div class="cmd-row" data-fill="${esc(u)}"><code>${esc(u)}</code>
         <span class="cmd-desc">${esc(d)}</span></div>`).join('') + '</div>').join('');
  }

  function buildHelpUI() {
    const form = $('search-form');
    const toggle = document.createElement('button');
    toggle.type = 'button'; toggle.className = 'cmdhelp-toggle'; toggle.textContent = 'CMDS';
    toggle.title = 'Command reference';
    toggle.setAttribute('aria-expanded', 'false');
    form.appendChild(toggle);

    const panel = document.createElement('div');
    panel.className = 'cmdhelp'; panel.hidden = true;
    panel.innerHTML = `<div class="cmdhelp-head"><span class="cmdhelp-title">Commands</span>
      <span class="cmdhelp-hint">The search bar answers questions: bare heights, hashes and
        0x addresses jump straight to their page; these commands do the rest. Ranges accept
        <code>100..200</code>, <code>100-200</code> or <code>100 to 200</code>.</span>
      <button type="button" class="cmdhelp-close" aria-label="Close">close</button></div>` +
      helpPanelHtml(true);
    form.appendChild(panel);

    const open = (o) => { panel.hidden = !o; toggle.setAttribute('aria-expanded', String(o)); };
    toggle.addEventListener('click', () => open(panel.hidden));
    panel.querySelector('.cmdhelp-close').addEventListener('click', () => open(false));
    $('search').addEventListener('focus', () => { if (!$('search').value.trim()) open(true); });
    document.addEventListener('click', (e) => {
      if (!form.contains(e.target)) open(false);
    });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') open(false); });
    panel.addEventListener('click', (e) => {
      const row = e.target.closest('.cmd-row');
      if (!row) return;
      const tmpl = row.dataset.fill;
      const inp = $('search');
      inp.value = tmpl.replace(/\s*\[.*?\]/g, '').replace(/<[^>]*>/g, '').trimEnd() + ' ';
      inp.focus();
      // keep only the keyword part; the user fills the argument
    });
  }

  // ─────────────────────── router ──────────────────────────
  function currentRoute() {
    const h = location.hash.replace(/^#\/?/, '');
    const [pathPart, qsPart] = h.split('?');
    const seg = pathPart.split('/').filter(Boolean).map(decodeURIComponent);
    return { seg, params: new URLSearchParams(qsPart || '') };
  }

  const TAB_OF = { blocks: 'blocks', block: 'blocks', hash: 'blocks',
                   txs: 'txs', tx: 'txs',
                   addresses: 'addresses', address: 'addresses',
                   songs: 'songs', song: 'songs', artist: 'songs',
                   stats: 'stats' };

  async function render() {
    const { seg, params } = currentRoute();
    const page = seg[0] || 'blocks';
    document.querySelectorAll('#viewnav .vn-btn').forEach((b) =>
      b.setAttribute('aria-selected', String(b.dataset.tab === (TAB_OF[page] || ''))));
    window.scrollTo(0, 0);
    const seq = ++renderSeq;
    const guard = (fn) => fn().catch((e) => {
      if (seq !== renderSeq) return;
      main.innerHTML = errPanel(e instanceof ApiError ? e : new ApiError(-1, e.message));
      console.error(e);
    });
    switch (page) {
      case 'blocks':    return guard(() => viewBlocks(params));
      case 'block':     return guard(() => viewBlock(seg[1]));
      case 'hash':      return guard(() => viewHashLookup(seg[1]));
      case 'txs':       return guard(() => viewTxs());
      case 'tx':        return guard(() => viewTx(seg[1]));
      case 'addresses': return guard(() => viewAddresses());
      case 'address':   return guard(() => viewAddress(seg[1], params));
      case 'songs':     return guard(() => viewSongs());
      case 'song':      return guard(() => viewSong(seg[1], params));
      case 'artist':    return guard(() => viewArtist(seg[1]));
      case 'stats':     return guard(() => viewStats(params));
      case 'search':    return guard(() => viewSearch(seg.slice(1).join('/')));
      case 'q':         return guard(() => viewCommand(seg.slice(1).join('/')));
      default:          location.replace('#/blocks');
    }
  }

  // ─────────────────────── wiring ──────────────────────────
  function wire() {
    $('search-form').addEventListener('submit', (e) => {
      e.preventDefault();
      const q = $('search').value.trim();
      if (!q) return;
      const parsed = parseQuery(q);
      document.querySelector('.cmdhelp')?.setAttribute('hidden', '');
      if (parsed && parsed.kind === 'route') location.hash = parsed.hash;
      else location.hash = `#/q/${encodeURIComponent(q)}`;
    });
    window.addEventListener('hashchange', render);
    $('foot-gateway').textContent = MOCK ? 'the built-in demo dataset (?mock=1)' : CFG.gateway;
  }

  buildHelpUI();
  wire();
  if (!location.hash) location.replace('#/blocks');
  render();
})();
