/* Bopwire block explorer — vanilla JS, no framework, no build step.
 *
 * Talks only to the gateway (window.BOPWIRE.gateway) against the explorer
 * API contract:
 *   /api/blocks, /api/blocks/:id, /api/tx/:hash, /api/address/:addr[,/history,/ratings],
 *   /api/search, /api/stats, /api/stats/artist/:id, /api/song/:hash[,/fingerprint,/ratings],
 *   /api/moderation[,/hidden,/moderator/:addr], /api/ratings/threshold, /api/top
 *
 * The backend lands route-by-route, so every view degrades to a labelled
 * "endpoint not live yet" panel instead of breaking, and ?mock=1 swaps the
 * whole fetch layer for the deterministic in-browser mock (explorer-mock.js).
 *
 * The search box is a COMMAND BAR: bare heights / hashes / addresses / text
 * still route straight to the right page, but it also understands ~32
 * commands ("blocks 100..200", "hidden <name>", "ratings by 0x…", …) documented
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
                     rating: 'Rating (thumbs up/down)',
                     moderator_proposal: 'Moderator proposal', username_register: 'Username',
                     slash: 'Slash', relay_reward: 'Relay reward', checkpoint: 'Checkpoint',
                     settlement_mint: 'Settlement mint' };
  const TX_NUM = { 0x01: 'transfer', 0x10: 'mint', 0x20: 'moderator_op', 0x30: 'moderator_proposal',
                   0x40: 'username_register', 0x50: 'slash', 0x60: 'relay_reward',
                   0x70: 'node_auth', 0x71: 'checkpoint', 0x72: 'settlement_mint',
                   0x80: 'rating' };
  // ModeratorOpTx.op_code and ProposalTx.kind, for gateways that omit the
  // *_name companions.
  const MOD_OP_NAME = { 1: 'grant', 2: 'revoke', 3: 'tag_label_edit' };
  const PROPOSAL_KIND_NAME = { 1: 'hide_content', 2: 'release_escrow', 3: 'vote_yes',
                               4: 'grant_moderator', 5: 'set_rating_threshold' };
  const txType = (tx) => {
    let t = tx.type ?? tx.tx_type ?? tx.type_byte ?? tx.kind;
    if (typeof t === 'number') t = TX_NUM[t] || String(t);
    t = String(t || 'unknown').toLowerCase();
    if (t === 'song' || t === 'song_section' || t === 'songregister') t = 'song_register';
    return t;
  };
  const txChip = (t) => {
    const cls = t === 'mint' ? 't-mint' : t === 'transfer' ? 't-transfer'
              : t === 'song_register' ? 't-song' : t === 'node_auth' ? 't-nodeauth'
              : t === 'rating' ? 't-rating'
              : t === 'moderator_proposal' ? 't-proposal' : 't-mod';
    return `<span class="chip ${cls}">${esc(TX_LABEL[t] || t)}</span>`;
  };
  const ratingVerdictChip = (v) => {
    const up = v === 'up' || v === 1 || v === '1';
    const dn = v === 'down' || v === 2 || v === '2';
    return `<span class="chip ${up ? 'k-unhide' : dn ? 'k-hide' : ''}">` +
      `${up ? THUMB_UP_SVG : dn ? THUMB_DN_SVG : ''} ${esc(up ? 'UP' : dn ? 'DOWN' : String(v ?? '?'))}</span>`;
  };
  const roleChip = (r) => `<span class="chip r-${esc(r)}">${esc(r)}</span>`;

  // ═══════════════ moderation vocabulary ═══════════════
  //
  // Shapes come straight from the gateway's transparency routes
  // (/api/moderation, /api/moderation/hidden, /api/moderation/moderator/:addr).
  // One action row is:
  //   {kind, category, value, moderator, signer_is_founder,
  //    signer_mod_level_now, source:"block"|"gossip"|"rating",
  //    height, ts_ms, tx_hash?, sig?, level?, id, proposal?}
  const LEVEL_NAME = { 0: 'NONE', 1: 'VOICE', 2: 'OP', 3: 'FOUNDER' };
  const levelName = (l) => LEVEL_NAME[l] ?? String(l ?? '?');

  const ZERO_ADDR_RE = /^(0x)?0{40}$/i;
  const isZeroAddr = (a) => !a || ZERO_ADDR_RE.test(String(a));

  const MOD_KIND_LABEL = {
    hide:   'HIDE', unhide: 'UNHIDE',
    grant:  'GRANT MODERATOR', revoke: 'REVOKE MODERATOR',
    label_edit: 'LABEL EDIT', moderator_op: 'MODERATOR OP',
    proposal_hide:  'PROPOSAL — HIDE CONTENT',
    proposal_release: 'PROPOSAL — RELEASE ESCROW',
    proposal_grant: 'PROPOSAL — GRANT MODERATOR',
    proposal_rating_threshold: 'PROPOSAL — SET RATING THRESHOLD',
    proposal: 'PROPOSAL', vote_yes: 'VOTE YES',
    forgery_report: 'FORGERY REPORT',
  };
  const modKindCls = (k) =>
      k === 'hide' ? 'k-hide' : k === 'unhide' ? 'k-unhide'
    : k === 'revoke' ? 'k-revoke' : k === 'grant' ? 'k-grant'
    : k === 'vote_yes' ? 'k-vote' : k === 'forgery_report' ? 'k-forgery'
    : /^proposal/.test(k || '') ? 'k-proposal' : '';
  const modKindChip = (a) =>
    `<span class="chip ${modKindCls(a.kind)}">${esc(MOD_KIND_LABEL[a.kind] || a.kind || '?')}</span>`;

  // Where the row came from. "block" = a signed tx in a block; "gossip" = a
  // signed moderator envelope replicated in the mod log; "rating" = nobody
  // signed it, the downvote rule fired.
  const MOD_SOURCE_NOTE = {
    block:  'signed transaction, in a block',
    gossip: 'signed moderator envelope, replicated in the mod log',
    rating: 'no signer — consensus applied the downvote auto-hide rule',
  };
  const modSourceChip = (s) =>
    `<span class="chip src" title="${esc(MOD_SOURCE_NOTE[s] || '')}">${esc(s || '?')}</span>`;

  // WHO stands behind an action. `signer_mod_level_now` is the signer's level
  // RIGHT NOW — the chain does not record what it was at signing time, and we
  // say so rather than implying otherwise.
  function modSigner(a) {
    if (a.source === 'rating' || isZeroAddr(a.moderator))
      return { cls: 's-rule', label: 'DOWNVOTE RULE', none: true,
               note: 'No signer: consensus applied the auto-hide rule.' };
    if (a.signer_is_founder)
      return { cls: 's-founder', label: 'FOUNDER',
               note: 'Signed by the founder key.' };
    if ((a.signer_mod_level_now | 0) > 0)
      return { cls: 's-moderator',
               label: `MODERATOR · ${levelName(a.signer_mod_level_now)}`,
               note: `Level ${esc(levelName(a.signer_mod_level_now))} as of now — the chain does not record the level held at signing time.` };
    return { cls: 's-former', label: 'NOT A MODERATOR NOW',
             note: 'This signer holds no moderator level today; it may have when it signed. The chain does not record historical levels.' };
  }
  const modSignerChip = (a) => {
    const s = modSigner(a);
    return `<span class="chip ${s.cls}" title="${esc(s.note)}">${esc(s.label)}</span>`;
  };
  const modSignerHtml = (a) => {
    const s = modSigner(a);
    return s.none
      ? `${modSignerChip(a)} <span class="dim mini">no signing wallet</span>`
      : `${modSignerChip(a)} ${addrLink(a.moderator)}`;
  };

  // A SET_RATING_THRESHOLD proposal packs (min_ratings | bps<<32) into amount.
  function unpackRatingPolicy(v) {
    let n; try { n = BigInt(v); } catch (_) { return null; }
    if (n < 0n) return null;
    return { min_ratings: Number(n & 0xffffffffn),
             down_ratio_bps: Number((n >> 32n) & 0xffffffffn) };
  }

  // What an action points AT, rendered per category.
  function modTargetHtml(a) {
    const cat = a.category || '', v = a.value == null ? '' : String(a.value);
    const catChip = `<span class="chip">${esc(cat || '?')}</span>`;
    if (cat === 'moderator' || cat === 'escrow')
      return `${catChip} ${isZeroAddr(v) ? '<span class="dim">—</span>' : addrLink(v)}` +
        (a.level !== undefined ? ` <span class="dim mini">level ${esc(levelName(a.level))}</span>` : '');
    if (cat === 'hash' || cat === 'content')
      return `${catChip} ${HEX64.test(v) ? songLink(v) : `<code>${esc(v)}</code>`}`;
    if (cat === 'proposal')
      return `${catChip} ${HEX64.test(v) ? txLink(v) : `<code>${esc(v)}</code>`}`;
    if (cat === 'artist')
      return `${catChip} <a href="#/artist/${encodeURIComponent(v)}"><b>${esc(v)}</b></a>`;
    if (cat === 'album' || cat === 'title')
      return `${catChip} <a href="#/q/${encodeURIComponent('hidden ' + v)}"><b>${esc(v)}</b></a>`;
    if (cat === 'rating_policy') {
      const p = unpackRatingPolicy(v);
      return `${catChip} ` + (p
        ? `<b>min ${fmtInt(p.min_ratings)} ratings</b>, <b>${(p.down_ratio_bps / 100).toFixed(2)}%</b> downvotes`
        : `<code>${esc(v)}</code>`);
    }
    if (cat === 'label') return `${catChip} <code>${esc(v)}</code>`;
    return v ? `${catChip} <code>${esc(v)}</code>` : catChip;
  }

  const modWhen = (a) =>
    `<span class="dim" title="${fmtTime(a.ts_ms)}">${fmtAgo(a.ts_ms)}</span>`;
  const modBlockLink = (a) => a.height === undefined || a.height === null
    ? '<span class="dim">—</span>'
    : `<a href="#/block/${a.height}">#${fmtInt(a.height)}</a>`;
  const modTxLink = (a) => a.tx_hash
    ? `<a href="#/tx/${esc(a.tx_hash)}">${shortHex(a.tx_hash, 8)}</a>`
    : '<span class="dim mini" title="Gossip envelope — signed by a moderator and replicated in the mod log, but not itself a block transaction.">gossip envelope</span>';

  // Vote provenance that rides along on proposal rows.
  function proposalHtml(p) {
    if (!p) return '';
    const voters = p.voters || [];
    return `<div class="kv" style="margin-top:6px">
      ${kvRow('proposal', p.proposal_tx_hash ? txLink(p.proposal_tx_hash) : '—')}
      ${kvRow('kind', esc(p.kind_name || String(p.kind ?? '?')))}
      ${kvRow('proposer', p.proposer ? addrLink(p.proposer) : '—')}
      ${kvRow('proposed at', p.proposed_height !== undefined
        ? `<a href="#/block/${p.proposed_height}">#${fmtInt(p.proposed_height)}</a>` : '—')}
      ${kvRow('yes votes', `${fmtInt(p.yes_votes)} <span class="dim mini">of ${fmtInt(p.threshold_now)} needed today (${fmtInt(p.active_moderators_now)} active moderators now — the set size at execution is not recorded on chain)</span>`)}
      ${kvRow('executed', p.executed
        ? `<b style="color:var(--good)">yes</b>${p.executed_height !== undefined
            ? ` — <a href="#/block/${p.executed_height}">block #${fmtInt(p.executed_height)}</a>` : ''}`
        : '<span class="dim">not yet</span>')}
      ${p.target_content_hash ? kvRow('target song', songLink(p.target_content_hash)) : ''}
      ${p.target_address ? kvRow('target wallet', addrLink(p.target_address)) : ''}
      ${voters.length ? kvRow('voters', voters.map((v) =>
        `${addrLink(v.voter)} <span class="dim mini">at <a href="#/block/${v.height}">#${fmtInt(v.height)}</a></span>`).join('<br/>')) : ''}
    </div>`;
  }

  // A type-aware moderator-action card — same idiom as the mint / transfer /
  // song-registration tx cards.
  function modActionCard(a) {
    const cls = a.source === 'rating' ? 'is-rule'
              : a.kind === 'hide' ? 'is-hide' : a.kind === 'unhide' ? 'is-unhide' : '';
    const s = modSigner(a);
    let what;
    if (a.kind === 'hide')
      what = `${modTargetHtml(a)} <span class="dim">was hidden network-wide</span>`;
    else if (a.kind === 'unhide')
      what = `${modTargetHtml(a)} <span class="dim">was restored</span>`;
    else if (a.kind === 'grant')
      what = `${modTargetHtml(a)} <span class="dim">was granted moderator status</span>`;
    else if (a.kind === 'revoke')
      what = `${modTargetHtml(a)} <span class="dim">had moderator status revoked</span>`;
    else if (a.kind === 'vote_yes')
      what = `<span class="dim">voted YES on</span> ${modTargetHtml(a)}`;
    else if (/^proposal/.test(a.kind || ''))
      what = `<span class="dim">proposed:</span> ${modTargetHtml(a)}`;
    else if (a.kind === 'forgery_report')
      what = `<span class="dim">reported a forged registration for</span> ${modTargetHtml(a)}`;
    else what = modTargetHtml(a);

    return `<div class="modcard ${cls}">
      <div class="modcard-head">${modKindChip(a)}${modSignerChip(a)}
        <span class="spacer"></span>
        <span class="dim mini">${modBlockLink(a)} &middot; ${modWhen(a)}</span>
        ${modSourceChip(a.source)}</div>
      <div class="modcard-body">
        <div class="modcard-what">${what}</div>
        <div class="kv">
          ${kvRow('signed by', s.none
            ? '<span class="dim">nobody — the downvote rule fired automatically</span>'
            : `${addrLink(a.moderator)} <span class="dim mini">(${esc(s.note)})</span>`)}
          ${kvRow('block', modBlockLink(a))}
          ${kvRow('when', `${fmtTime(a.ts_ms)} <span class="dim">(${fmtAgo(a.ts_ms)})</span>`)}
          ${kvRow('transaction', modTxLink(a))}
          ${a.level !== undefined ? kvRow('level', `${esc(String(a.level))} <span class="dim">(${esc(levelName(a.level))})</span>`) : ''}
        </div>
        ${a.kind === 'hide' ? reasonUnrecordedHtml() : ''}
        ${proposalHtml(a.proposal)}
        ${rawDetails(a, 'Raw action JSON')}
      </div></div>`;
  }

  // Shared table for lists of moderator actions (scrolls sideways on a phone).
  function modActionsTable(actions, opts) {
    opts = opts || {};
    if (!actions || !actions.length) return '<div class="note">No moderation actions.</div>';
    return `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Action</th><th>Target</th>` +
      (opts.noModerator ? '' : '<th>Signed by</th>') +
      `<th>Source</th><th>Block</th><th>When</th><th>Tx</th></tr></thead><tbody>` +
      actions.map((a) => `<tr><td>${modKindChip(a)}</td>
        <td>${modTargetHtml(a)}</td>` +
        (opts.noModerator ? '' : `<td>${modSignerHtml(a)}</td>`) +
        `<td>${modSourceChip(a.source)}</td>
        <td>${modBlockLink(a)}</td>
        <td>${modWhen(a)}</td>
        <td>${modTxLink(a)}</td></tr>`).join('') +
      '</tbody></table></div>';
  }

  // ═══════════════ takedown provenance ═══════════════
  //
  // HONESTY RULE. A takedown notice is submitted ECIES-encrypted to the
  // shared moderation key; the node never sees the plaintext and this site
  // holds no key and must never hold one. So a DMCA-driven hide is
  // byte-identical on chain to any other manual moderator hide. We therefore
  // NEVER label a hide "DMCA". Where the chain records no reason we say so,
  // in as many words. A future on-chain `reason` field drops straight into
  // reasonHtml() below without touching anything else.
  function reasonUnrecordedHtml() {
    return `<span class="reason-unrec"><b>Reason: not recorded on chain.</b>
      The hide envelope carries no reason field, so this record does not say why it
      was applied. Takedown notices are submitted encrypted to the moderators' shared
      key — the chain, the nodes and this explorer never see their contents — which
      means a copyright takedown and any other manual hide look identical here.
      This page will not guess between them.</span>`;
  }
  function reasonHtml(prov) {
    const r = prov && prov.reason;
    if (r && r !== 'unrecorded' && String(r).trim())
      return `<span class="reason-unrec"><b>Reason on chain:</b> ${esc(String(r))}</span>`;
    // A rule-driven hide states its own reason by construction — no person
    // chose it, so there is no missing motive to disclose.
    const m = prov && MECHANISM_REASON[prov.by];
    if (m) return `<span class="reason-unrec"><b>Reason: ${esc(m)}.</b>
      Nobody signed this hide, so there is no undisclosed human reason behind it —
      the numbers above are the whole record.</span>`;
    return reasonUnrecordedHtml();
  }

  // Renders `hidden_provenance` (song.detail) / `provenance` (hidden list).
  // `by` is one of founder | moderator | vote | ratings | forgery_quorum |
  // unknown. Nothing here is inferred beyond what the node states.
  const PROV_TITLE = {
    founder:        'Hidden by the FOUNDER',
    moderator:      'Hidden by a MODERATOR',
    vote:           'Hidden by MODERATOR VOTE',
    ratings:        'Auto-hidden by DOWNVOTES',
    forgery_quorum: 'Hidden by FORGERY REPORTS',
    unknown:        'Hidden — origin not recorded on chain',
  };
  function provenanceBody(prov) {
    if (!prov) return '';
    const by = prov.by || 'unknown';
    let rows = '';
    if (by === 'founder' || by === 'moderator') {
      rows = kvRow('signed by', `${addrLink(prov.moderator)} ` +
          `<span class="chip ${by === 'founder' ? 's-founder' : 's-moderator'}">${by === 'founder' ? 'FOUNDER' : 'MODERATOR'}</span>`) +
        (prov.mod_level_now !== undefined
          ? kvRow('signer level now', `${esc(levelName(prov.mod_level_now))} <span class="dim mini">— current level; the chain does not record the level held at signing time</span>`) : '') +
        (prov.height !== undefined ? kvRow('hidden at', `<a href="#/block/${prov.height}">block #${fmtInt(prov.height)}</a>`) : '') +
        (prov.ts_ms ? kvRow('when', `${fmtTime(prov.ts_ms)} <span class="dim">(${fmtAgo(prov.ts_ms)})</span>`) : '') +
        (prov.sig ? kvRow('envelope signature', `<span class="h-link" title="${esc(prov.sig)}">${shortHex(prov.sig, 10)}</span>${copyBtn(prov.sig)}`) : '');
    } else if (by === 'vote') {
      rows = kvRow('mechanism',
        'a HIDE_CONTENT proposal that reached moderator quorum — proposer and voters below');
    } else if (by === 'ratings') {
      const t = prov.threshold_in_force || {};
      const rr = prov.ratings || {};
      rows = kvRow('mechanism',
          'listeners downvoted past the auto-hide rule in force — no moderator signed this') +
        kvRow('counts when it fired',
          `${ratingFig('up', rr.up)} ${ratingFig('down', rr.down)} <span class="dim">of ${fmtInt(rr.total ?? ((rr.up | 0) + (rr.down | 0)))} ratings</span>`) +
        kvRow('threshold in force then', thresholdSentence(t)) +
        (prov.height !== undefined ? kvRow('hidden at', `<a href="#/block/${prov.height}">block #${fmtInt(prov.height)}</a>`) : '') +
        (prov.trigger_tx ? kvRow('triggering rating tx', txLink(prov.trigger_tx)) : '');
    } else if (by === 'forgery_quorum') {
      rows = kvRow('mechanism',
          'independent nodes reported the registered audio does not match its declared fingerprint') +
        kvRow('forgery reports', fmtInt(prov.forgery_reports));
    } else {
      rows = kvRow('mechanism',
        '<span class="dim">The chain shows this item hidden, but carries no matching hide record — ' +
        'no signer, proposal, rating event or forgery quorum could be attributed. ' +
        'Nothing is being guessed here.</span>');
    }
    return `<div class="kv">${rows}</div>` +
      (by === 'vote' ? proposalHtml(prov.proposal) : '') +
      reasonHtml(prov);
  }

  // The big banner. `prov` may be null/absent — we still say it's hidden.
  function takedownBanner(prov, opts) {
    opts = opts || {};
    const by = (prov && prov.by) || 'unknown';
    const chip = by === 'founder' ? '<span class="chip s-founder">FOUNDER</span>'
               : by === 'moderator' ? '<span class="chip s-moderator">MODERATOR</span>'
               : by === 'vote' ? '<span class="chip s-vote">MODERATOR VOTE</span>'
               : by === 'ratings' ? '<span class="chip s-rule">DOWNVOTES</span>'
               : by === 'forgery_quorum' ? '<span class="chip k-forgery">FORGERY QUORUM</span>'
               : '<span class="chip">ORIGIN UNRECORDED</span>';
    return `<div class="takedown${by === 'ratings' ? ' by-rating' : ''}">
      <div class="takedown-head">
        <span class="takedown-title">${esc(opts.title || PROV_TITLE[by] || PROV_TITLE.unknown)}</span>
        ${chip}</div>
      <div class="note">${opts.lead || `This ${esc(opts.what || 'item')} is <b>hidden network-wide</b>:
        every node applies the hide, and it stops surfacing in Discover, on the website and in
        all players. The chain record below stays public — hides curate playback surfaces,
        they never rewrite history.`}</div>
      ${provenanceBody(prov)}
      ${opts.footer || ''}
    </div>`;
  }
  const notHiddenPanel = (what) =>
    `<div class="takedown ok"><div class="takedown-head">
      <span class="takedown-title">Not hidden</span>
      <span class="chip k-unhide">VISIBLE</span></div>
      <div class="note">No hide is in force for this ${esc(what || 'item')} — it surfaces
      normally in Discover, on the website and in every player.</div></div>`;

  // Summary tiles for the moderation log's `counts` block.
  function modCountTiles(meta) {
    const c = (meta && meta.counts) || {};
    return `<div class="tiles">
      <div class="tile"><div class="t-label">Hides</div><div class="t-value">${fmtInt(c.hides ?? 0)}</div></div>
      <div class="tile"><div class="t-label">Unhides</div><div class="t-value">${fmtInt(c.unhides ?? 0)}</div></div>
      <div class="tile"><div class="t-label">Grants / revokes</div>
        <div class="t-value">${fmtInt(c.grants ?? 0)} / ${fmtInt(c.revokes ?? 0)}</div></div>
      <div class="tile"><div class="t-label">Active moderators</div>
        <div class="t-value">${fmtInt(meta && meta.active_moderators)}</div></div>
      <div class="tile"><div class="t-label">Actions on chain</div>
        <div class="t-value">${fmtInt(meta && meta.total)}</div></div>
    </div>`;
  }

  // A hidden-list row's human label. `value` is the artist / album / title
  // string, or a 64-hex content hash for an individually hidden track.
  const hiddenLabel = (h) =>
    h.category === 'hash' ? (h.title || shortHex(h.value)) : String(h.value || '');
  const hiddenLinkHtml = (h) =>
      h.category === 'hash'   ? `${songLink(h.value, h.title || undefined)}${h.artist ? ` <span class="dim">by ${esc(h.artist)}</span>` : ''}`
    : h.category === 'artist' ? `<a href="#/artist/${encodeURIComponent(h.value)}"><b>${esc(h.value)}</b></a>`
    : `<a href="#/q/${encodeURIComponent('hidden ' + h.value)}"><b>${esc(h.value)}</b></a>`;

  // One-line "how was this hidden" summary, for table rows.
  function provSummaryHtml(prov) {
    if (!prov) return '<span class="chip">ORIGIN UNRECORDED</span>';
    const by = prov.by || 'unknown';
    if (by === 'founder')
      return `<span class="chip s-founder">FOUNDER</span> ${addrLink(prov.moderator)}`;
    if (by === 'moderator')
      return `<span class="chip s-moderator">MODERATOR</span> ${addrLink(prov.moderator)}` +
        `<span class="dim mini"> level ${esc(levelName(prov.mod_level_now))} now</span>`;
    if (by === 'vote') {
      const p = prov.proposal || {};
      return `<span class="chip s-vote">MODERATOR VOTE</span> ` +
        `<span class="dim mini">${fmtInt(p.yes_votes)} yes, proposed by </span>${addrLink(p.proposer)}`;
    }
    if (by === 'ratings') {
      const r = prov.ratings || {};
      return `<span class="chip s-rule">DOWNVOTES</span> ` +
        `<span class="dim mini">${fmtInt(r.down)} down of ${fmtInt(r.total)}</span>`;
    }
    if (by === 'forgery_quorum')
      return `<span class="chip k-forgery">FORGERY QUORUM</span> ` +
        `<span class="dim mini">${fmtInt(prov.forgery_reports)} reports</span>`;
    return '<span class="chip">ORIGIN UNRECORDED</span>';
  }

  // The Reason column. A rating or forgery hide HAS a mechanism recorded, so
  // saying "not recorded" there would be wrong; only a human-signed hide (or
  // an unattributable one) genuinely carries no reason.
  const MECHANISM_REASON = {
    ratings: 'downvote auto-hide rule',
    forgery_quorum: 'fingerprint mismatch reports',
  };
  function reasonCell(prov) {
    const r = prov && prov.reason;
    if (r && r !== 'unrecorded' && String(r).trim()) return esc(String(r));
    const m = prov && MECHANISM_REASON[prov.by];
    return m ? esc(m) : 'not recorded on chain';
  }
  // Does any row in a hidden list carry no reason at all? Only then is the
  // "reason not recorded" explainer worth printing under it.
  const anyReasonUnrecorded = (rows) => rows.some((h) => {
    const p = h.provenance || {};
    return !(p.reason && p.reason !== 'unrecorded' && String(p.reason).trim()) &&
           !MECHANISM_REASON[p.by];
  });

  // A vote-driven hide carries its coordinates on the proposal, not the row.
  const provHeight = (p) => p && (p.height !== undefined ? p.height
    : (p.proposal && p.proposal.executed_height));
  const provTs = (p) => p && (p.ts_ms !== undefined ? p.ts_ms
    : (p.proposal && p.proposal.timestamp_ms));

  function hiddenTable(rows) {
    return `<div class="tbl-wrap"><table class="tbl"><thead><tr>
      <th>Kind</th><th>What</th><th>Hidden how</th><th>Reason</th><th>At block</th><th>When</th>
      </tr></thead><tbody>` + rows.map((h) => {
        const p = h.provenance || {};
        return `<tr>
          <td><span class="chip">${esc(h.category)}</span></td>
          <td>${hiddenLinkHtml(h)}</td>
          <td>${provSummaryHtml(h.provenance)}</td>
          <td class="dim">${reasonCell(p)}</td>
          <td>${provHeight(p) !== undefined ? `<a href="#/block/${provHeight(p)}">#${fmtInt(provHeight(p))}</a>` : '<span class="dim">—</span>'}</td>
          <td class="dim" title="${fmtTime(provTs(p))}">${provTs(p) ? fmtAgo(provTs(p)) : '<span title="The chain records the height this fired at, not a wall-clock time.">—</span>'}</td></tr>`;
      }).join('') + '</tbody></table></div>';
  }

  // A hide can target the individual track, or the whole TITLE, ALBUM or
  // ARTIST. song.detail's `hidden` flag only covers the per-track hash hide,
  // so both the song and the artist page cross-check the hidden list too —
  // otherwise a track silenced by an artist-wide hide would look visible.
  function hideMatchesFor(hidRows, what) {
    if (!hidRows) return [];
    const eq = (a, b) => a && b && String(a).toLowerCase() === String(b).toLowerCase();
    return hidRows.filter((h) =>
      (h.category === 'hash'   && eq(h.value, what.hash)) ||
      (h.category === 'artist' && eq(h.value, what.artist)) ||
      (h.category === 'album'  && eq(h.value, what.album)) ||
      (h.category === 'title'  && eq(h.value, what.title)));
  }
  const HIDE_SCOPE_NOTE = {
    hash:   'this exact track',
    title:  'every track with this title',
    album:  'this whole album',
    artist: 'this whole artist',
  };

  // ═══════════════ ratings (on-chain thumbs up/down) ═══════════════
  const THUMB_UP_SVG = '<svg class="rate-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M9 21H5a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1h4v11zm2-11.6L14.2 3a2 2 0 0 1 2.7-.8 2.6 2.6 0 0 1 1.3 2.9L17.6 8h2.9a2 2 0 0 1 2 2.5l-1.7 8A2 2 0 0 1 18.8 20H11V9.4z"/></svg>';
  const THUMB_DN_SVG = '<svg class="rate-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M15 3h4a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-4V3zm-2 11.6L9.8 21a2 2 0 0 1-2.7.8A2.6 2.6 0 0 1 5.8 19L6.4 16H3.5a2 2 0 0 1-2-2.5l1.7-8A2 2 0 0 1 5.2 4H13v10.6z"/></svg>';
  const ratingFig = (dir, n) =>
    `<span class="rate-fig ${dir === 'up' ? 'up' : 'down'}">${dir === 'up' ? THUMB_UP_SVG : THUMB_DN_SVG}` +
    `<span class="rf-n">${fmtInt(n ?? 0)}</span><span class="rf-l">${dir === 'up' ? 'up' : 'down'}</span></span>`;

  // Normalizes both rating shapes: ratings.get (/api/song/:h/ratings) and the
  // `ratings` object song.detail carries inline.
  function normRatings(r) {
    if (!r || typeof r !== 'object') return null;
    const up = Number(r.up || 0), down = Number(r.down || 0);
    const hide = r.rating_hide || r.auto_hide || null;
    return {
      up, down,
      total: r.total !== undefined ? Number(r.total) : up + down,
      score: r.score !== undefined ? Number(r.score) : up - down,
      my: r.my_rating ?? null,
      canRate: r.can_rate,
      hidden: r.hidden,
      hide,
      modUnhidden: r.moderator_unhidden ?? (hide ? hide.moderator_unhidden : undefined),
      threshold: r.threshold || r.threshold_in_force || null,
    };
  }

  // Plain-English restatement of the rule, straight from the node's numbers —
  // the explorer hardcodes no constant.
  function thresholdSentence(t) {
    if (!t) return '<span class="dim">not reported</span>';
    const pct = t.down_ratio_pct !== undefined
      ? t.down_ratio_pct : (Number(t.down_ratio_bps || 0) / 100).toFixed(2);
    return `hide once a track has at least <b>${fmtInt(t.min_ratings)}</b> ratings ` +
      `and <b>${esc(String(pct))}%</b> or more of them are downvotes`;
  }
  function thresholdOriginHtml(t) {
    if (!t) return '';
    if (t.source === 'chain' || (t.set_height && t.source === undefined))
      return `Set on chain at <a href="#/block/${t.set_height}">block #${fmtInt(t.set_height)}</a>` +
        (t.set_by ? ` by ${addrLink(t.set_by)}` : '') +
        (t.proposal_tx ? ` — proposal ${txLink(t.proposal_tx)}` : '') + '.';
    return 'No SET_RATING_THRESHOLD proposal has ever executed on this chain, so the ' +
      'compiled-in default is in force.';
  }

  // Hand-rolled SVG: an up/down split bar with the auto-hide threshold marked.
  function ratingBarSvg(up, down, bps) {
    const total = (up | 0) + (down | 0);
    const W = 600, H = 34, barY = 4, barH = 14, r = 4;
    if (!total)
      return `<div class="ratebar"><svg viewBox="0 0 ${W} ${H}" role="img"
        aria-label="no ratings yet"><rect x="0" y="${barY}" width="${W}" height="${barH}"
        rx="${r}" fill="var(--chip)"/><text x="0" y="${H - 3}" fill="var(--text-dim)"
        style="font-size:11px">no ratings yet</text></svg></div>`;
    const upW = Math.round(W * (up / total));
    const dPct = down / total * 100;
    const tPct = bps === undefined || bps === null ? null : Number(bps) / 100;
    const tX = tPct === null ? null : W * (1 - tPct / 100);
    return `<div class="ratebar"><svg viewBox="0 0 ${W} ${H}" role="img"
      aria-label="${fmtInt(up)} up, ${fmtInt(down)} down${tPct === null ? '' : `, auto-hide at ${tPct}% down`}">
      <rect x="0" y="${barY}" width="${W}" height="${barH}" rx="${r}" fill="var(--chip)"/>
      ${up ? `<rect x="0" y="${barY}" width="${upW}" height="${barH}" rx="${r}" fill="var(--s1)"/>` : ''}
      ${down ? `<rect x="${upW}" y="${barY}" width="${W - upW}" height="${barH}"
        ${up ? '' : `rx="${r}"`} fill="#b4433f"/>` : ''}
      ${tX === null ? '' : `<line x1="${tX.toFixed(1)}" x2="${tX.toFixed(1)}" y1="${barY - 3}"
        y2="${barY + barH + 3}" stroke="var(--warn)" stroke-width="2"/>
        <text x="${Math.min(W - 4, tX + 5).toFixed(1)}" y="${H - 3}" fill="var(--warn)"
        text-anchor="${tX > W - 150 ? 'end' : 'start'}" style="font-size:11px">auto-hide at ${tPct}% down</text>`}
      <text x="0" y="${H - 3}" fill="var(--text-dim)" style="font-size:11px">${dPct.toFixed(1)}% down</text>
    </svg></div>`;
  }

  // The song/track ratings panel. `n` is a normRatings() result.
  function ratingsPanel(n, opts) {
    opts = opts || {};
    if (!n) return '';
    const t = n.threshold;
    return `<div class="panel"><div class="panel-title">Ratings
        <span class="pt-note">— on-chain thumbs up/down, one per wallet that actually played it</span></div>
      <div class="rate-row">${ratingFig('up', n.up)}${ratingFig('down', n.down)}
        <span class="rate-fig"><span class="rf-n">${n.score > 0 ? '+' : ''}${fmtInt(n.score)}</span>
          <span class="rf-l">score</span></span>
        <span class="rate-fig"><span class="rf-n">${fmtInt(n.total)}</span>
          <span class="rf-l">total</span></span>
        ${n.my ? `<span class="rate-mine">this wallet rated it <b>${esc(n.my)}</b></span>` : ''}
        ${n.canRate === false ? '<span class="rate-mine">this wallet has no play credit, so it cannot rate</span>' : ''}
      </div>
      ${ratingBarSvg(n.up, n.down, t && t.down_ratio_bps)}
      <div class="mini" style="margin-top:8px">Auto-hide rule in force: ${thresholdSentence(t)}.
        ${thresholdOriginHtml(t)}
        ${opts.linkThreshold === false ? '' : ' <a href="#/q/threshold">Full rule &amp; bounds</a>.'}</div>
      ${opts.extra || ''}</div>`;
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

  // Pull the moderation log up to `max` actions. The gateway serves it
  // NEWEST-FIRST and pages with offset/limit; `counts` and `active_moderators`
  // ride along on every page, so we keep the first page's copy.
  async function pullModeration(max) {
    const items = []; let offset = 0, total = Infinity, meta = null;
    while (items.length < max && offset < total) {
      const r = await exGet(`/api/moderation?offset=${offset}&limit=100`);
      const page = r.actions || r.items || [];
      if (!meta) meta = { total: r.total ?? page.length, counts: r.counts || {},
                          active_moderators: r.active_moderators };
      total = r.total ?? (page.length < 100 ? offset + page.length : Infinity);
      items.push(...page);
      if (!page.length) break;
      offset += page.length;
    }
    const actions = items.slice(0, max);
    actions.meta = meta || { total: actions.length, counts: {} };
    return actions;
  }

  // /api/moderation/hidden returns four category buckets; flatten them into
  // one list of rows, each keeping its category and provenance.
  const HIDDEN_BUCKETS = [['artists', 'artist'], ['albums', 'album'],
                          ['titles', 'title'], ['hashes', 'hash']];
  function flattenHidden(r) {
    const rows = [];
    for (const [key, cat] of HIDDEN_BUCKETS)
      for (const it of (r && r[key]) || [])
        rows.push({ category: it.category || cat, value: it.value,
                    title: it.title, artist: it.artist,
                    provenance: it.provenance || null });
    return rows;
  }
  async function pullHidden() {
    const r = await exGet('/api/moderation/hidden');
    return { rows: flattenHidden(r), counts: r.counts || {}, raw: r };
  }

  // Ratings for one track. song.detail already carries a `ratings` object on
  // gateways that have it; /api/song/:hash/ratings is the dedicated route.
  async function songRatings(hash) {
    try { return normRatings(await exGet(`/api/song/${hash}/ratings`)); }
    catch (e) { if (notLive(e)) return null; throw e; }
  }
  async function ratingThreshold() {
    try { return await exGet('/api/ratings/threshold'); }
    catch (e) { if (notLive(e)) return null; throw e; }
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

  // "What this wallet rated" — the /api/address/:addr/ratings shape.
  function addressRatingsHtml(r) {
    const rows = (r && (r.ratings || r.items)) || [];
    let up = 0, down = 0;
    for (const x of rows) { if (x.value === 'up') up++; else if (x.value === 'down') down++; }
    if (!rows.length)
      return `<div class="panel"><div class="panel-title">Ratings by this wallet</div>
        <div class="note">This wallet has not rated anything on chain.</div></div>`;
    return `<div class="panel"><div class="panel-title">Ratings by this wallet
        <span class="pt-note">— thumbs it cast, signed on chain</span></div>
      <div class="rate-row">${ratingFig('up', up)}${ratingFig('down', down)}
        <span class="rate-fig"><span class="rf-n">${fmtInt(r.total ?? rows.length)}</span>
          <span class="rf-l">rated</span></span></div>
      <div class="tbl-wrap" style="margin-top:10px"><table class="tbl"><thead><tr>
        <th>Verdict</th><th>Song</th><th>Artist</th>
        <th class="num">Up</th><th class="num">Down</th><th class="num">Score</th>
      </tr></thead><tbody>` + rows.map((x) => `<tr>
        <td>${ratingVerdictChip(x.value)}</td>
        <td>${songLink(x.content_hash, x.title || undefined)}</td>
        <td class="dim">${esc(x.artist || '—')}</td>
        <td class="num">${fmtInt(x.up)}</td>
        <td class="num">${fmtInt(x.down)}</td>
        <td class="num">${(x.up | 0) - (x.down | 0) > 0 ? '+' : ''}${fmtInt((x.up | 0) - (x.down | 0))}</td>
      </tr>`).join('') + `</tbody></table></div>` +
      (r.total !== undefined && r.total > rows.length
        ? `<div class="note">Showing ${fmtInt(rows.length)} of ${fmtInt(r.total)}.</div>` : '') +
      `<div class="mini" style="margin-top:8px">Up/Down are the track's CURRENT network totals,
        not this wallet's — one wallet is one vote.</div></div>`;
  }

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
    // `type_byte` duplicates the type chip on every card — always noise.
    skip = new Set([...(skip || []), 'type_byte']);
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
    if (t === 'moderator_op') {
      const op = String(tx.op_name || MOD_OP_NAME[tx.op_code] || 'op');
      return `<span class="chip ${modKindCls(op)}">${esc(op.toUpperCase())}</span> ` +
        `${addrLink(tx.subject)}` +
        (tx.level !== undefined ? ` <span class="dim mini">level ${esc(levelName(tx.level))}</span>` : '') +
        ` <span class="dim">by</span> ${addrLink(tx.proposer)}`;
    }
    if (t === 'moderator_proposal') {
      const kn = String(tx.kind_name || PROPOSAL_KIND_NAME[tx.kind] || 'proposal');
      const tgt = kn === 'hide_content' ? songLink(tx.target_hash)
                : kn === 'vote_yes'     ? txLink(tx.target_hash)
                : kn === 'set_rating_threshold' && tx.rating_threshold
                  ? `<b>min ${fmtInt(tx.rating_threshold.min_ratings)} ratings, ` +
                    `${(Number(tx.rating_threshold.down_ratio_bps) / 100).toFixed(2)}% down</b>`
                : addrLink(tx.target_addr);
      return `<span class="chip ${kn === 'vote_yes' ? 'k-vote' : 'k-proposal'}">${esc(kn.replace(/_/g, ' ').toUpperCase())}</span> ` +
        `${tgt} <span class="dim">by</span> ${addrLink(tx.proposer)}`;
    }
    if (t === 'rating')
      return `${addrLink(tx.rater)} <span class="dim">rated</span> ` +
        `${ratingVerdictChip(tx.value_name || tx.value)} ${songLink(tx.content_hash)}`;
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
        kvRow('op_code', `${esc(String(tx.op_code ?? '—'))} <span class="dim">(${esc(tx.op_name || MOD_OP_NAME[tx.op_code] || '?')})</span>`) +
        (tx.level !== undefined ? kvRow('level', `${esc(String(tx.level))} <span class="dim">(${esc(levelName(tx.level))})</span>`) : '') +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'op_code', 'op_name', 'level', 'action', 'signature',
                   'subject_pubkey', 'proposer_pubkey', 'block_height', 'block_hash', 'timestamp_ms']) +
        `</div><div class="mini" style="margin-top:6px">Grants and revocations of moderator status —
          moderator identity on chain is only (address, level, pubkey), never a name.
          See the <a href="#/moderation">Moderation</a> tab for the full log.</div>`;
    } else if (t === 'moderator_proposal') {
      const kn = String(tx.kind_name || PROPOSAL_KIND_NAME[tx.kind] || '?');
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        kvRow('proposal kind', `${esc(String(tx.kind ?? '?'))} <span class="dim">(${esc(kn)})</span>`) +
        (tx.rating_threshold ? kvRow('proposed rule',
          thresholdSentence({ min_ratings: tx.rating_threshold.min_ratings,
                              down_ratio_bps: tx.rating_threshold.down_ratio_bps })) : '') +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'kind', 'kind_name', 'rating_threshold',
                   'signature', 'subject_pubkey', 'proposer_pubkey',
                   'block_height', 'block_hash', 'timestamp_ms']) +
        `</div>` +
        (kn === 'hide_content'
          ? `<div class="mini" style="margin-top:6px">A HIDE_CONTENT proposal hides the target once
             it reaches moderator quorum. If it executed, the song page shows it as
             <b>hidden by moderator vote</b>, with the proposer and every voter.</div>
             ${reasonUnrecordedHtml()}`
          : '');
    } else if (t === 'rating') {
      body = `<div style="padding:8px 0 2px">${txSummaryHtml(tx)}</div><div class="kv">` +
        kvRow('verdict', `${ratingVerdictChip(tx.value_name || tx.value)} <span class="dim mini">value ${esc(String(tx.value ?? '?'))}</span>`) +
        kvRow('rater', addrLink(tx.rater)) +
        kvRow('song', songLink(tx.content_hash)) +
        kvAll(tx, ['type', 'hash', 'tx_hash', 'value', 'value_name', 'rater', 'content_hash',
                   'rater_pubkey', 'signature', 'block_height', 'block_hash', 'timestamp_ms']) +
        `</div><div class="mini" style="margin-top:6px">One rating per wallet per track, and only
          from a wallet that actually played it. Ratings are consensus state: enough downvotes
          past the rule in force auto-hides the track network-wide.
          <a href="#/song/${esc(tx.content_hash || '')}">Current counts on the song page</a>.</div>`;
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

    // What this wallet has rated — on-chain thumbs, one per track it played.
    try {
      const rr = await exGet(`/api/address/${a}/ratings?offset=0&limit=100`);
      html += addressRatingsHtml(rr);
    } catch (e) {
      if (!notLive(e)) html += errPanel(e);
      // 404/503 here is the pre-ratings gateway: stay quiet rather than
      // implying the wallet rated nothing.
    }

    // Moderation actions signed by this wallet, if any — a moderator's
    // address page should not hide what they did with the key.
    try {
      const mr = await exGet(`/api/moderation/moderator/${a}?limit=25`);
      const macts = mr.actions || [];
      // Never attribute someone else's actions to this wallet: only render if
      // the reply is actually about the address we asked for.
      const sameAddr = !mr.address ||
        String(mr.address).toLowerCase() === String(a).toLowerCase();
      if (sameAddr && (macts.length || mr.is_moderator || mr.is_founder)) {
        const rank = mr.is_founder ? 'FOUNDER'
                   : mr.is_moderator ? `MODERATOR · ${levelName(mr.mod_level)}` : 'FORMER MODERATOR';
        html += `<div class="panel"><div class="panel-title">Moderation
            <span class="pt-note">— this wallet's actions on the public record</span></div>
          <div style="margin-bottom:8px"><span class="chip ${mr.is_founder ? 's-founder' : mr.is_moderator ? 's-moderator' : 's-former'}">${esc(rank)}</span>
            <span class="dim mini">${fmtInt(mr.total ?? macts.length)} actions on chain</span></div>` +
          (macts.length ? modActionsTable(macts, { noModerator: true })
                        : '<div class="note">No actions signed with this key.</div>') +
          `<div class="note" style="margin-top:8px">Full record:
            <a href="#/q/${encodeURIComponent('moderator ' + a)}">moderator ${esc(shortHex(a, 8))}</a>.</div></div>`;
      }
    } catch (_) { /* not a moderator, or the route isn't live — say nothing */ }

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
    // The catalog endpoint says nothing about hides, so cross-check the
    // hidden list rather than listing a silenced track as if it were live.
    let hidRows = null;
    try { hidRows = (await pullHidden()).rows; } catch (_) {}
    let html = pageHead('Songs', 'registered on chain');
    if (songs && songs.length) {
      let nHidden = 0;
      const rows = songs.map((s) => {
        const h = s.contentHash || s.content_hash;
        const hides = hideMatchesFor(hidRows, { hash: h, artist: s.artist,
                                                album: s.album, title: s.title });
        if (hides.length) nHidden++;
        const mark = hides.length
          ? ` <span class="chip k-hide" title="Hidden via a ${esc(hides[0].category)} hide — open the song for how and by whom.">HIDDEN</span>` : '';
        return `<tr><td><a href="#/song/${esc(h)}">${esc(s.title || shortHex(h))}</a>${mark}</td>
          <td>${esc(s.artist || '—')}</td><td class="dim">${esc(s.album || '—')}</td>
          <td class="dim">${esc(s.genre || '—')}</td><td class="num dim">${esc(String(s.year || '—'))}</td>
          <td class="num dim">${fmtDur(s.durationMs || s.duration_ms)}</td>
          <td class="num">${fmtInt(s.plays ?? s.play_count ?? 0)}</td></tr>`;
      }).join('');
      html += (hidRows
          ? (nHidden ? `<div class="note" style="margin-bottom:8px">${fmtInt(nHidden)} of the
               ${fmtInt(songs.length)} listed are under a hide — open one for how and by whom, or
               see <a href="#/moderation">Moderation</a>.</div>` : '')
          : `<div class="note" style="margin-bottom:8px">Hide status unavailable
               (<code>/api/moderation/hidden</code> unreachable) — this list is not asserting that
               everything below is visible.</div>`) +
        `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Title</th><th>Artist</th>
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
        ${kvRow('ratings <song>', '<span class="note">thumbs up/down, and the auto-hide rule</span>')}
        ${kvRow('hidden <name>', '<span class="note">is this artist/album/title hidden, and how</span>')}
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

  // song.detail nests the on-chain SongSection under `song` on the live
  // gateway; older shapes were flat. Normalize both.
  function normSong(s, hash) {
    const m = (s.song && typeof s.song === 'object') ? s.song : s;
    return {
      raw: s, meta: m || {},
      content_hash: s.content_hash || (m && m.content_hash) || hash,
      title: (m && m.title) || s.title || '',
      artist: (m && m.artist) || s.artist || '',
      album: (m && m.album) || s.album || '',
      artist_address: (m && m.artist_address) || s.artist_address || null,
      fingerprint: (m && m.compressed_fingerprint) || s.compressed_fingerprint || '',
      registration_height: s.registration_height ?? s.reg_height ?? null,
      registration_block: s.registration_block_hash || s.registration_block || null,
      play_count: s.play_count, unique_listeners: s.unique_listeners,
      earned_total: s.earned_total,
      holders: Array.isArray(s.holders) ? s.holders : [],
      royalty_splits: (m && m.royalty_splits) || s.royalty_splits || [],
      hashHidden: s.hidden === true || s.is_hidden === true ||
                  (s.hidden && typeof s.hidden === 'object'),
      provenance: s.hidden_provenance ||
                  (s.hidden && typeof s.hidden === 'object' ? s.hidden : null),
      ratings: s.ratings || null,
    };
  }

  async function viewSong(hash, params) {
    main.innerHTML = loadingHtml();
    let raw;
    try { raw = await exGet(`/api/song/${encodeURIComponent(hash)}`); }
    catch (e) {
      main.innerHTML = pageHead('Song') +
        (notLive(e) ? stubPanel(`Song ${shortHex(hash)}`, '/api/song/:hash') : errPanel(e));
      return;
    }
    const s = normSong(raw, hash);
    const ch = s.content_hash;

    // Takedown state comes from two places: the per-track hash hide that
    // song.detail reports, and the artist / album / title hides that only the
    // hidden list knows about. Both are shown; neither is inferred.
    let hidRows = null, hidErr = null;
    try { hidRows = (await pullHidden()).rows; } catch (e) { hidErr = e; }
    const scoped = hideMatchesFor(hidRows, {
      hash: ch, artist: s.artist, album: s.album, title: s.title });
    const hashRow = scoped.find((h) => h.category === 'hash');

    // Ratings: prefer the object song.detail carries; fall back to the
    // dedicated route on gateways that predate it.
    let r = normRatings(s.ratings);
    if (!r) r = await songRatings(ch);
    if (r && r.threshold === null) {
      const t = await ratingThreshold();
      if (t) r.threshold = t;
    }

    // ── takedown panel ──
    let takedown = '';
    if (s.hashHidden || scoped.length) {
      const parts = [];
      // The per-track hide first: it is the most specific statement.
      if (s.hashHidden) {
        const prov = s.provenance || (hashRow && hashRow.provenance) || null;
        parts.push(takedownBanner(prov, {
          what: 'track',
          lead: `<b>This track is hidden network-wide.</b> Every node applies the hide, so it
            stops surfacing in Discover, on the website and in all players. The on-chain record
            below stays public — a hide curates playback surfaces, it never rewrites the chain.`,
        }));
      }
      for (const h of scoped) {
        if (h.category === 'hash' && s.hashHidden) continue;   // already shown
        parts.push(takedownBanner(h.provenance, {
          title: `${PROV_TITLE[(h.provenance || {}).by || 'unknown']} — ${h.category} hide`,
          what: h.category,
          lead: `This track is hidden because a hide is in force on the <b>${esc(h.category)}</b>
            &ldquo;<b>${esc(h.value)}</b>&rdquo;, which covers ${esc(HIDE_SCOPE_NOTE[h.category] || 'it')}.`,
        }));
      }
      takedown = parts.join('');
    } else if (hidRows) {
      takedown = notHiddenPanel('track');
    } else {
      takedown = `<div class="stub"><b>Takedown status unknown.</b>
        <code>GET /api/moderation/hidden</code> was unreachable${hidErr && hidErr.message ? ` (${esc(hidErr.message)})` : ''},
        so this page cannot confirm whether a hide is in force. It is not asserting that the
        track is visible.</div>`;
    }
    // A rating-driven hide a moderator has since reviewed and cleared.
    if (r && r.hide && r.modUnhidden)
      takedown += `<div class="takedown ok"><div class="takedown-head">
        <span class="takedown-title">Auto-hidden by downvotes, then restored by a moderator</span>
        <span class="chip k-unhide">REVIEWED</span></div>
        <div class="note">Downvotes tripped the auto-hide rule at
          <a href="#/block/${r.hide.height}">block #${fmtInt(r.hide.height)}</a>
          (${fmtInt(r.hide.up)} up / ${fmtInt(r.hide.down)} down). A moderator reviewed it and
          restored the track; that decision is final — ratings never re-apply the hide, even
          after a chain replay.</div></div>`;

    const metaSkip = ['compressed_fingerprint', 'content_hash', 'royalty_splits'];
    main.innerHTML = pageHead(esc(s.title || 'Song'),
        `${esc(s.artist || '')}${s.album ? ` — ${esc(s.album)}` : ''}`) +
      takedown +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Plays</div><div class="t-value">${fmtInt(s.play_count)}</div></div>
        <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(s.unique_listeners)}</div></div>
        <div class="tile"><div class="t-label">Artist earnings</div><div class="t-value">${fmtAmt(s.earned_total)}</div><div class="t-sub">tokens</div></div>
        ${r ? `<div class="tile"><div class="t-label">Rating</div>
          <div class="t-value">${r.score > 0 ? '+' : ''}${fmtInt(r.score)}</div>
          <div class="t-sub">${fmtInt(r.up)} up / ${fmtInt(r.down)} down</div></div>` : ''}
        <div class="tile"><div class="t-label">Registered</div><div class="t-value" style="font-size:14px">
          ${s.registration_height != null ? `<a href="#/block/${s.registration_height}">block #${fmtInt(s.registration_height)}</a>` : '—'}</div></div>
      </div>
      <div id="ratings-panel">${r ? ratingsPanel(r) : `<div class="stub">
        <b>Ratings aren't available for this track yet.</b>
        This needs <code>GET /api/song/:hash/ratings</code> on the gateway, or a
        <code>ratings</code> object on <code>/api/song/:hash</code> — neither answered.</div>`}</div>
      ${r && r.hide && !r.modUnhidden ? ratingHideDetail(r) : ''}
      <div class="panel"><div class="panel-title">On-chain record — every field</div>
        <div class="kv">
        ${kvRow('content_hash', `<span class="h-link" title="${esc(ch)}">${shortHex(ch)}</span>${copyBtn(ch)}`)}
        ${s.registration_height != null ? kvRow('registration_height', `<a href="#/block/${s.registration_height}">#${fmtInt(s.registration_height)}</a>`) : ''}
        ${s.registration_block ? kvRow('registration_block', blockLink(s.registration_block)) : ''}
        ${kvAll(s.meta, metaSkip)}
        ${s.royalty_splits.length ? kvRow('royalty splits',
          s.royalty_splits.map((h) => `${addrLink(h.address)} <span class="dim">${esc(String(h.percent ?? h.bps ?? ''))}</span>`).join('<br/>')) : ''}
        ${s.holders.length ? kvRow('holders (live network)',
          s.holders.map((h) => `${addrLink(h.address || h)}${h.online ? ' <span class="chip k-unhide">online</span>' : ''}`).join('<br/>')) : ''}
        </div>${rawDetails(raw, 'Raw record JSON')}</div>

      <div class="panel verify-panel" id="fp-panel">
        <div class="panel-title">Verify this registration yourself</div>
        <div class="note">You don't have to trust this site. The song's acoustic fingerprint
          (a <b>chromaprint</b>) is committed on chain inside the registration transaction in
          <a href="#/block/${esc(String(s.registration_height ?? ''))}">block #${fmtInt(s.registration_height)}</a>,
          and the audio itself is identified by its content hash:</div>
        <div class="hash-big">${esc(ch)}${copyBtn(ch)}</div>
        <button class="btn btn-solid" id="fp-dl"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3v10.6l3.3-3.3 1.4 1.4L12 17.4l-4.7-5.7 1.4-1.4 2.3 3.3V3h2zM5 19h14v2H5v-2z"/></svg>Download fingerprint</button>
        <span class="mini" style="margin-left:10px">${s.fingerprint ? `${fmtInt(s.fingerprint.length)} chars base64` : 'served by the gateway'}</span>
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
        <code>plays of ${esc(s.title || shortHex(ch))}</code> &middot;
        <code>ratings ${esc(shortHex(ch))}</code> &middot;
        <code>artist ${esc(s.artist || '')}</code></div>`;

    $('fp-dl').onclick = async () => {
      try {
        const text = s.fingerprint || await exText(`/api/song/${ch}/fingerprint`);
        downloadText(`${(s.title || ch).replace(/[^\w.-]+/g, '_')}.chromaprint.b64.txt`, text);
        toast('Fingerprint downloaded — compare it with fpcalc output');
      } catch (e) {
        toast(notLive(e) ? 'Fingerprint endpoint not live yet (/api/song/:hash/fingerprint)' : e.message);
      }
    };
    if (params && params.get('fp') === '1')
      $('fp-panel').scrollIntoView({ behavior: 'smooth', block: 'start' });
    if (params && params.get('r') === '1')
      $('ratings-panel').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  // The frozen record of a rating-driven auto-hide: the counts AND the rule
  // that were in force the moment it fired.
  function ratingHideDetail(r) {
    const h = r.hide, t = (h && h.threshold_in_force) || null;
    if (!h) return '';
    return `<div class="panel"><div class="panel-title">The auto-hide, exactly as recorded</div>
      <div class="note">Consensus wrote this record in the block the rule fired in. It freezes both
        the counts and the threshold then in force, so retuning the rule later can never make this
        hide unexplainable.</div>
      <div class="kv" style="margin-top:8px">
        ${kvRow('fired at', `<a href="#/block/${h.height}">block #${fmtInt(h.height)}</a>`)}
        ${kvRow('counts then', `${ratingFig('up', h.up)} ${ratingFig('down', h.down)}
          <span class="dim">of ${fmtInt(h.total ?? ((h.up | 0) + (h.down | 0)))}</span>`)}
        ${kvRow('rule then in force', thresholdSentence(t))}
        ${t && t.set_height ? kvRow('that rule was set at',
          `<a href="#/block/${t.set_height}">block #${fmtInt(t.set_height)}</a>` +
          (t.set_by ? ` by ${addrLink(t.set_by)}` : '')) : ''}
        ${h.trigger_tx ? kvRow('rating tx that tipped it', txLink(h.trigger_tx)) : ''}
        ${kvRow('moderator review', r.modUnhidden
          ? '<b style="color:var(--good)">reviewed and restored</b>'
          : '<span class="dim">none recorded</span>')}
      </div>
      <div class="mini" style="margin-top:8px">This hide has a mechanism on chain — the
        downvote rule — so it is not one of the hides whose reason is unrecorded.</div></div>`;
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
    const addr40 = a.artist_address || (ADDR_RE.test(String(id)) ? id : null);
    const name = a.name || (Array.isArray(a.names) ? a.names[0] : '') || '';
    const songs = a.top_songs && a.top_songs.length ? a.top_songs : (a.songs || a.plays_per_song || []);

    // ── takedown status for the artist, and for their catalogue ──
    let hidRows = null, hidErr = null;
    try { hidRows = (await pullHidden()).rows; } catch (e) { hidErr = e; }
    const artistHides = hideMatchesFor(hidRows, { artist: name });
    // Albums the artist has (from their song rows) that carry an album hide,
    // plus individual tracks of theirs that are hidden.
    const songByHash = new Map((a.songs || songs).map((x) => [String(x.content_hash || '').toLowerCase(), x]));
    const trackHides = (hidRows || []).filter((h) =>
      (h.category === 'hash' && songByHash.has(String(h.value || '').toLowerCase())) ||
      (h.category === 'title' && (a.songs || songs).some((x) =>
        String(x.title || '').toLowerCase() === String(h.value || '').toLowerCase())));

    let takedown = '';
    if (artistHides.length) {
      takedown = artistHides.map((h) => takedownBanner(h.provenance, {
        title: `${PROV_TITLE[(h.provenance || {}).by || 'unknown']} — whole artist`,
        what: 'artist',
        lead: `<b>This artist's entire catalogue is hidden network-wide.</b> Every node applies the
          hide on the artist name &ldquo;<b>${esc(h.value)}</b>&rdquo;, so none of their tracks
          surface in Discover, on the website or in any player. The chain records below stay
          public — a hide curates playback surfaces, it never rewrites history.`,
      })).join('');
    } else if (hidRows) {
      takedown = notHiddenPanel('artist') +
        (trackHides.length
          ? `<div class="note" style="margin:-6px 0 14px">…but ${fmtInt(trackHides.length)} of their
             track${trackHides.length === 1 ? ' is' : 's are'} individually hidden — see below.</div>` : '');
    } else {
      takedown = `<div class="stub"><b>Takedown status unknown.</b>
        <code>GET /api/moderation/hidden</code> was unreachable${hidErr && hidErr.message ? ` (${esc(hidErr.message)})` : ''},
        so this page cannot confirm whether a hide is in force on this artist. It is not asserting
        that the catalogue is visible.</div>`;
    }

    const seeders = a.seeders || a.serving_seeders || [];
    const relays  = a.relays  || a.serving_relays  || [];
    const peerRow = (x, role) => {
      const ad = typeof x === 'string' ? x : x.address;
      const n  = typeof x === 'string' ? null : x.plays;
      return `<div style="padding:2px 0">${roleChip(role)} ${addrLink(ad)}` +
        (n != null ? ` <span class="dim mini">${fmtInt(n)} plays</span>` : '') + '</div>';
    };

    main.innerHTML = pageHead(esc(name || shortHex(String(id), 8)), 'artist dashboard') +
      takedown +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Total plays</div><div class="t-value">${fmtInt(a.total_plays)}</div></div>
        <div class="tile"><div class="t-label">Unique listeners</div><div class="t-value">${fmtInt(a.unique_listeners)}</div></div>
        <div class="tile"><div class="t-label">Earned</div><div class="t-value">${fmtAmt(a.earned_total)}</div><div class="t-sub">tokens</div></div>
        <div class="tile"><div class="t-label">Songs on chain</div><div class="t-value">${fmtInt((a.songs || songs).length)}</div></div>
        <div class="tile"><div class="t-label">Hidden tracks</div>
          <div class="t-value" ${artistHides.length || trackHides.length ? 'style="color:var(--danger)"' : ''}>${
            artistHides.length ? 'ALL' : fmtInt(hidRows ? trackHides.length : undefined)}</div>
          <div class="t-sub">${hidRows ? 'in force right now' : 'unknown — endpoint down'}</div></div>
      </div>` +
      (trackHides.length && !artistHides.length
        ? `<div class="panel"><div class="panel-title">Hidden tracks by this artist
             <span class="pt-note">— and how each one was hidden</span></div>
           ${hiddenTable(trackHides)}${anyReasonUnrecorded(trackHides) ? reasonUnrecordedHtml() : ''}</div>`
        : '') +
      `<div class="panel"><div class="panel-title">Plays over time</div><div id="ar-chart"></div></div>
      <div class="charts-2col">
        <div class="panel"><div class="panel-title">Top songs</div><div id="ar-top"></div></div>
        <div class="panel"><div class="panel-title">Who serves this artist</div>
          <div class="note" style="margin-bottom:6px">Seeders (uploaded the bytes) and relays
            (mini-nodes that carried the stream) attested on this artist's plays:</div>
          ${seeders.map((x) => peerRow(x, 'seeder')).join('')}
          ${relays.map((x) => peerRow(x, 'relay')).join('')}
          ${!seeders.length && !relays.length ? '<div class="note">None recorded.</div>' : ''}
        </div>
      </div>` +
      (Array.isArray(a.blocks) && a.blocks.length
        ? `<div class="note">Appears in ${fmtInt(a.blocks.length)} blocks — newest:
           ${a.blocks.slice(-8).reverse().map((h) => `<a href="#/block/${h}">#${fmtInt(h)}</a>`).join(', ')}.</div>`
        : '') +
      (addr40 && !isZeroAddr(addr40)
        ? `<div class="note">Wallet: ${addrLink(addr40)} — <a href="#/address/${esc(addr40)}">full history</a></div>` : '');

    const ot = (a.plays_over_time || []).map((p) => ({ t: Date.parse(p.date), v: p.plays ?? p.count ?? 0 }));
    lineChart($('ar-chart'), [{ name: 'plays', points: ot }], { label: 'plays over time' });
    const hidHashes = new Set((hidRows || []).filter((h) => h.category === 'hash')
                                             .map((h) => String(h.value).toLowerCase()));
    const top = songs.slice(0, 10).map((s2) => ({
      label: (s2.title || shortHex(s2.content_hash)) +
             (hidHashes.has(String(s2.content_hash || '').toLowerCase()) ? '  [hidden]' : ''),
      value: s2.plays ?? s2.count ?? 0,
      href: s2.content_hash ? `#/song/${s2.content_hash}` : undefined }));
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
    // Field names follow the node's actual /api/stats response
    // (recent_activity / total_blocks / tx_type_counts). The older
    // per_day / blocks / txs_by_type spellings are kept as fallbacks so
    // the view still renders against an older gateway.
    let series = s.recent_activity || s.per_day || s.timeseries || [];
    if (since && /^\d{4}-\d{2}-\d{2}$/.test(since)) series = series.filter((d) => d.date >= since);

    main.innerHTML = pageHead('Network stats',
        since ? `since ${esc(since)} — <a href="#/stats">clear</a>` : 'the whole chain at a glance') +
      `<div class="tiles">
        <div class="tile"><div class="t-label">Height</div><div class="t-value">${fmtInt(s.height)}</div><div class="t-sub">${fmtInt(s.total_blocks ?? s.blocks)} blocks</div></div>
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

    const types = Object.entries(s.tx_type_counts || s.txs_by_type || {}).sort((a, b) => b[1] - a[1])
      .map(([k, v]) => ({ label: TX_LABEL[k] || k, value: v }));
    $('st-types').innerHTML = types.length ? hbarList(types) : '<div class="note">No data.</div>';
  }

  // ─────────────── Moderation tab ───────────────
  // The transparency dashboard: what is hidden right now and HOW, who holds
  // moderator status, and the newest actions. Every panel degrades on its own
  // if its endpoint isn't live.
  async function viewModeration() {
    main.innerHTML = loadingHtml('Reading moderation state…');
    const seq = ++renderSeq;
    let hid = null, hidErr = null, acts = null, actErr = null;
    try { hid = await pullHidden(); } catch (e) { hidErr = e; }
    try { acts = await pullModeration(200); } catch (e) { actErr = e; }
    if (seq !== renderSeq) return;
    const meta = (acts && acts.meta) || {};

    let html = pageHead('Moderation',
      'takedowns, hides and moderator status — read straight off the public chain');

    html += `<div class="note" style="margin-bottom:14px">A hide is how a takedown propagates:
      a signed action goes on chain (or into the signed, replicated moderator log), every node
      applies it, and the target stops surfacing in Discover, on the website and in every player.
      Nothing is deleted — the chain records stay public and verifiable. This page shows what is
      hidden, when, and by which mechanism.</div>`;

    html += acts ? modCountTiles(meta)
                 : (notLive(actErr) ? stubPanel('The moderation log', '/api/moderation') : errPanel(actErr));

    // ── what is hidden right now ──
    html += `<div class="panel"><div class="panel-title">Hidden right now
      <span class="pt-note">— and how each one was hidden</span></div>`;
    if (hid) {
      html += hid.rows.length
        ? hiddenTable(hid.rows) + (anyReasonUnrecorded(hid.rows) ? reasonUnrecordedHtml() : '')
        : `<div class="note">Nothing is hidden on this chain right now — no artist, album, title
            or individual track is under a hide.</div>`;
      html += `<div class="note" style="margin-top:10px">Check one name:
        <code>hidden &lt;artist|album|title&gt;</code>.</div>`;
    } else {
      html += notLive(hidErr) ? stubPanel('The hidden list', '/api/moderation/hidden') : errPanel(hidErr);
    }
    html += `</div>`;

    // ── moderators ──
    if (acts) {
      const chrono = [...acts].reverse();
      const cur = new Map();
      for (const x of chrono) {
        if (x.category !== 'moderator' || isZeroAddr(x.value)) continue;
        const k = String(x.value).toLowerCase();
        if (x.kind === 'grant') cur.set(k, { address: x.value, level: x.level, at: x.height });
        if (x.kind === 'revoke') cur.delete(k);
      }
      const mods = [...cur.values()].sort((a, b) => (b.level || 0) - (a.level || 0));
      html += `<div class="panel"><div class="panel-title">Moderators
        <span class="pt-note">— identity on chain is only (address, level, pubkey), never a name</span></div>` +
        (mods.length
          ? mods.map((m) => `<div style="padding:4px 0">
              <span class="chip ${m.level >= 3 ? 's-founder' : 's-moderator'}">${esc(levelName(m.level))}</span>
              ${addrLink(m.address)}
              <span class="dim mini">since <a href="#/block/${m.at}">#${fmtInt(m.at)}</a></span>
              &middot; <a href="#/q/${encodeURIComponent('moderator ' + m.address)}">their actions</a></div>`).join('')
          : '<div class="note">No moderator grants in the log window.</div>') +
        `<div class="note" style="margin-top:8px">Replayed from the newest ${fmtInt(acts.length)}
          grant/revoke entries; the node reports ${fmtInt(meta.active_moderators)} active.</div></div>`;

      // ── newest actions ──
      html += `<div class="panel-title" style="margin:16px 2px 8px">Newest actions
        <span class="pt-note">— <a href="#/q/moderation">full log</a></span></div>` +
        (acts.length ? acts.slice(0, 10).map(modActionCard).join('')
                     : '<div class="note">No moderation actions on this chain yet.</div>');
    }

    html += `<div class="panel"><div class="panel-title">Moderation commands</div>
      <div class="kv">
        ${kvRow('moderation [x..y]', '<span class="note">every action, optionally over a block range</span>')}
        ${kvRow('hidden', '<span class="note">everything hidden right now, and how</span>')}
        ${kvRow('hidden &lt;name&gt;', '<span class="note">is one artist/album/title hidden?</span>')}
        ${kvRow('moderator &lt;0x…&gt;', '<span class="note">one moderator\u2019s full record</span>')}
        ${kvRow('moderators', '<span class="note">who holds moderator status, and at what level</span>')}
        ${kvRow('threshold', '<span class="note">the downvote auto-hide rule in force</span>')}
      </div></div>`;
    main.innerHTML = html;
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
    { group: 'Moderation & takedowns', items: [
      ['moderation [<x>..<y>]', 'moderator actions in chain order, optionally over a block range'],
      ['hidden',                'everything currently hidden — and HOW each one was hidden'],
      ['hidden <name>',         'is this artist/album/title hidden, and its action history'],
      ['moderator <0x…>',       'every action a given moderator has taken'],
      ['moderators',            'current moderators and their levels'],
    ]},
    { group: 'Ratings', items: [
      ['ratings <song>',        'thumbs up/down on one track, and the auto-hide rule it is measured against'],
      ['ratings by <0x…>',      'every track a wallet has rated'],
      ['threshold',             'the downvote auto-hide rule in force, where it came from, and its bounds'],
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
                     'moderation', 'hidden', 'moderator', 'moderators',
                     'ratings', 'threshold'];

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

      case 'ratings': case 'rating': {
        if (!rest.length)
          return usageErr('ratings needs a song, or "by <address>"',
                          'ratings <song>  ·  ratings by <0x…>');
        if (rest[0].toLowerCase() === 'by') {
          if (!ADDR_RE.test(rest[1] || ''))
            return usageErr('ratings by needs a 0x… address', 'ratings by <0x…>');
          return { kind: 'cmd', name: 'ratings_by', args: { addr: rest[1] } };
        }
        if (ADDR_RE.test(rest[0]))
          return { kind: 'cmd', name: 'ratings_by', args: { addr: rest[0] } };
        return { kind: 'cmd', name: 'ratings_of', args: { song: restStr } };
      }

      case 'threshold':
        return { kind: 'cmd', name: 'threshold', args: {} };

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
        // The node returns {kind, n, rows}; rows carry
        // {content_hash|address, title, artist, plays, unique_listeners}.
        // entries/results/name/id are kept as fallbacks for older gateways.
        const entries = r.rows || r.entries || r.results || [];
        const rows = entries.map((e) => {
          const id = e.content_hash || e.address || e.id;
          return { label: e.title || e.name || e.artist || shortHex(id, 8),
                   value: e.plays ?? e.count ?? 0, hash: id,
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

    // ── Moderation: the five transparency commands ──────────────────────
    //
    // Every one of these reads PUBLIC chain data through the gateway's
    // transparency routes. No moderator key is involved anywhere, and none
    // ever should be.

    moderation: async (a) => {
      const ranged = a.x !== undefined;
      const title = 'Moderation log';
      main.innerHTML = cmdHead(title) + loadingHtml('Reading the moderation log…');
      let actions;
      try { actions = await pullModeration(500); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The moderation log', '/api/moderation') : errPanel(e));
        return;
      }
      const meta = actions.meta || { counts: {} };
      let shown = actions;
      if (ranged) shown = actions.filter((x) => x.height >= a.x && x.height <= a.y);
      main.innerHTML = cmdHead(title,
          ranged ? `blocks #${fmtInt(a.x)}–#${fmtInt(a.y)} &middot; ${fmtInt(shown.length)} actions`
                 : `${fmtInt(shown.length)} of ${fmtInt(meta.total)} actions, newest first`) +
        modCountTiles(meta) +
        `<div class="note" style="margin-bottom:12px">Hides are how takedowns propagate: a
          verified action goes on chain (or into the signed, replicated moderator log), every
          node applies it, and the target stops surfacing anywhere. This log is public chain
          data — it is not filtered, and it never carries a takedown notice's contents.</div>` +
        (shown.length ? shown.map(modActionCard).join('')
                      : '<div class="note">No moderation actions in this range.</div>') +
        (!ranged && actions.length < meta.total
          ? `<div class="note">Showing the newest ${fmtInt(actions.length)} of
             ${fmtInt(meta.total)} — narrow with <code>moderation ${fmtInt(Math.max(0, (shown[0] || {}).height - 100))}..${fmtInt((shown[0] || {}).height)}</code>.</div>` : '') +
        `<div class="note">Also: <code>hidden</code> for what is hidden right now,
          <code>moderators</code> for who holds moderator status.</div>`;
    },

    hidden: async (a) => {
      const title = a.name ? `Hidden? — ${a.name}` : 'Currently hidden';
      main.innerHTML = cmdHead(esc(title)) + loadingHtml();
      let hid;
      try { hid = await pullHidden(); }
      catch (e) {
        main.innerHTML = cmdHead(esc(title)) +
          (notLive(e) ? stubPanel('The hidden list', '/api/moderation/hidden') : errPanel(e));
        return;
      }

      if (!a.name) {
        const c = hid.counts;
        main.innerHTML = cmdHead('Currently hidden',
            `${fmtInt(hid.rows.length)} target${hid.rows.length === 1 ? '' : 's'} in force`) +
          `<div class="tiles">
            <div class="tile"><div class="t-label">Artists</div><div class="t-value">${fmtInt(c.artists ?? 0)}</div></div>
            <div class="tile"><div class="t-label">Albums</div><div class="t-value">${fmtInt(c.albums ?? 0)}</div></div>
            <div class="tile"><div class="t-label">Titles</div><div class="t-value">${fmtInt(c.titles ?? 0)}</div></div>
            <div class="tile"><div class="t-label">Individual tracks</div><div class="t-value">${fmtInt(c.hashes ?? 0)}</div></div>
          </div>
          <div class="note" style="margin-bottom:12px">Everything a hide currently keeps out of
            Discover, the website and every player — with <b>how</b> each one was hidden. A hide
            never rewrites the chain: the underlying records stay public and verifiable.</div>` +
          (hid.rows.length ? hiddenTable(hid.rows)
                           : '<div class="note">Nothing is hidden right now.</div>') +
          (anyReasonUnrecorded(hid.rows) ? `<div class="note">${reasonUnrecordedHtml()}</div>` : '') +
          `<div class="note"><code>hidden &lt;name&gt;</code> checks one artist, album or title
            and shows its full action history.</div>`;
        return;
      }

      const low = a.name.toLowerCase();
      const hit = (v) => String(v || '').toLowerCase().includes(low);
      const matches = hid.rows.filter((h) => hit(h.value) || hit(h.title) || hit(h.artist));
      let history = [];
      try {
        history = (await pullModeration(500)).filter((x) =>
          hit(x.value) || (x.category === 'hash' && hit(x.value)));
      } catch (_) {}
      main.innerHTML = cmdHead(`Hidden? — ${esc(a.name)}`) +
        (matches.length
          ? matches.map((m) => takedownBanner(m.provenance, {
              title: `${(PROV_TITLE[(m.provenance || {}).by || 'unknown'])} — ${hiddenLabel(m)}`,
              what: m.category,
              lead: `<b>${esc(hiddenLabel(m))}</b> (${esc(m.category)}) is <b>hidden network-wide</b>.`,
            })).join('')
          : notHiddenPanel(`"${a.name}"`) +
            '<div class="note">Matching is a case-insensitive substring over hidden artists, ' +
            'albums, titles and track metadata. An exact name that is not listed is not hidden.</div>') +
        (history.length
          ? `<div class="panel-title" style="margin:16px 2px 8px">Action history referencing this name</div>` +
            modActionsTable(history)
          : '<div class="note" style="margin-top:12px">No moderation actions in the recent log reference this name.</div>');
    },

    moderator: async (a) => {
      const title = 'Moderator';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let r;
      try { r = await exGet(`/api/moderation/moderator/${a.addr}?limit=200`); }
      catch (e) {
        main.innerHTML = cmdHead(title, esc(a.addr)) +
          (notLive(e) ? stubPanel('Per-moderator actions', '/api/moderation/moderator/:addr') : errPanel(e));
        return;
      }
      const actions = r.actions || [];
      const rank = r.is_founder ? 'FOUNDER'
                 : r.is_moderator ? `MODERATOR · ${levelName(r.mod_level)}`
                 : 'NOT A MODERATOR';
      const rankCls = r.is_founder ? 's-founder' : r.is_moderator ? 's-moderator' : 's-former';
      const byKind = {};
      for (const x of actions) byKind[x.kind] = (byKind[x.kind] || 0) + 1;
      main.innerHTML = cmdHead(title, addrLink(r.address || a.addr)) +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Status today</div>
            <div class="t-value" style="font-size:15px"><span class="chip ${rankCls}">${esc(rank)}</span></div>
            <div class="t-sub">level ${esc(levelName(r.mod_level))}</div></div>
          <div class="tile"><div class="t-label">Actions on chain</div>
            <div class="t-value">${fmtInt(r.total ?? actions.length)}</div></div>
          <div class="tile"><div class="t-label">Hides / unhides</div>
            <div class="t-value">${fmtInt(byKind.hide || 0)} / ${fmtInt(byKind.unhide || 0)}</div>
            <div class="t-sub">in the ${fmtInt(actions.length)} listed</div></div>
          <div class="tile"><div class="t-label">Moderator since</div>
            <div class="t-value" style="font-size:15px">${r.active_since_height != null
              ? `<a href="#/block/${r.active_since_height}">#${fmtInt(r.active_since_height)}</a>` : '—'}</div>
            <div class="t-sub">block height</div></div>
        </div>
        <div class="note" style="margin-bottom:12px">Moderator identity on chain is only
          (address, level, pubkey) — never a name. Levels shown are <b>current</b>; the chain does
          not record what level a signer held at the moment it signed.</div>` +
        (actions.length ? actions.map(modActionCard).join('')
                        : '<div class="note">This address has taken no moderation actions.</div>') +
        `<div class="note">This wallet's non-moderation activity:
          <a href="#/address/${esc(a.addr)}">address page</a>.</div>`;
    },

    moderators: async () => {
      const title = 'Moderators';
      main.innerHTML = cmdHead(title) + loadingHtml();
      let actions;
      try { actions = await pullModeration(500); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The moderation log', '/api/moderation') : errPanel(e));
        return;
      }
      const meta = actions.meta || {};
      // Replay grant/revoke in CHAIN order (the log arrives newest-first).
      const chrono = [...actions].reverse();
      const cur = new Map();
      for (const x of chrono) {
        if (x.category !== 'moderator') continue;
        const who = String(x.value || '');
        if (isZeroAddr(who)) continue;
        if (x.kind === 'grant')
          cur.set(who.toLowerCase(), { address: who, level: x.level,
            granted_at: x.height, granted_by: x.moderator, tx_hash: x.tx_hash,
            ts_ms: x.ts_ms, by_founder: x.signer_is_founder });
        if (x.kind === 'revoke') cur.delete(who.toLowerCase());
      }
      const mods = [...cur.values()].sort((p, q) => (q.level || 0) - (p.level || 0));
      const derivedNote = meta.active_moderators !== undefined && meta.active_moderators !== mods.length
        ? `<div class="note">The node reports <b>${fmtInt(meta.active_moderators)}</b> active
           moderators; replaying the newest ${fmtInt(actions.length)} log entries yields
           ${fmtInt(mods.length)}. The difference is grants older than this window — the node's
           count is authoritative.</div>` : '';
      main.innerHTML = cmdHead(title,
          `${fmtInt(meta.active_moderators ?? mods.length)} active — replayed from the on-chain grant/revoke log`) +
        (mods.length
          ? `<div class="tbl-wrap"><table class="tbl"><thead><tr><th>Level</th><th>Address</th>
             <th>Granted at</th><th>When</th><th>Granted by</th><th>Tx</th></tr></thead><tbody>` +
            mods.map((mo) => `<tr>
              <td><span class="chip ${mo.level >= 3 ? 's-founder' : 's-moderator'}">${esc(levelName(mo.level))}</span></td>
              <td>${addrLink(mo.address)}</td>
              <td>${mo.granted_at != null ? `<a href="#/block/${mo.granted_at}">#${fmtInt(mo.granted_at)}</a>` : '—'}</td>
              <td class="dim" title="${fmtTime(mo.ts_ms)}">${fmtAgo(mo.ts_ms)}</td>
              <td>${addrLink(mo.granted_by)}${mo.by_founder ? ' <span class="chip s-founder">FOUNDER</span>' : ''}</td>
              <td>${mo.tx_hash ? `<a href="#/tx/${esc(mo.tx_hash)}">${shortHex(mo.tx_hash, 8)}</a>` : '<span class="dim">—</span>'}</td></tr>`).join('') +
            '</tbody></table></div>'
          : '<div class="note">No active moderators found in the log window.</div>') +
        derivedNote +
        `<div class="note">FOUNDER can grant and revoke; OP proposes hides and votes; VOICE observes.
          <code>moderator &lt;0x…&gt;</code> lists one moderator's actions.</div>`;
    },

    // ── Ratings ─────────────────────────────────────────────────────────
    ratings_of: async (a) => {
      const title = 'Ratings';
      main.innerHTML = loadingHtml('Resolving song…');
      let hash = /^[0-9a-fA-F]{64}$/.test(a.song) ? a.song.toLowerCase() : null;
      if (!hash) {
        let hits = [];
        try { hits = (await searchTyped(a.song)).filter((h) => h.type === 'song'); } catch (_) {}
        if (!hits.length) {
          main.innerHTML = cmdHead(title, esc(a.song)) +
            `<div class="stub">No song matched <b>${esc(a.song)}</b>. With the 64-hex content
              hash, <code>ratings &lt;hash&gt;</code> goes straight there.</div>`;
          return;
        }
        hash = hits[0].content_hash;
      }
      location.replace(`#/song/${hash}?r=1`);
    },

    ratings_by: async (a) => {
      const title = 'Ratings by wallet';
      main.innerHTML = cmdHead(title, esc(a.addr)) + loadingHtml();
      let r;
      try { r = await exGet(`/api/address/${a.addr}/ratings?offset=0&limit=200`); }
      catch (e) {
        main.innerHTML = cmdHead(title, esc(a.addr)) +
          (notLive(e) ? stubPanel('Per-wallet ratings', '/api/address/:addr/ratings') : errPanel(e));
        return;
      }
      main.innerHTML = cmdHead(title, addrLink(a.addr)) + addressRatingsHtml(r);
    },

    threshold: async () => {
      const title = 'Rating auto-hide threshold';
      main.innerHTML = cmdHead(title) + loadingHtml();
      let t;
      try { t = await exGet('/api/ratings/threshold'); }
      catch (e) {
        main.innerHTML = cmdHead(title) +
          (notLive(e) ? stubPanel('The rating threshold', '/api/ratings/threshold') : errPanel(e));
        return;
      }
      const b = t.bounds || {}, d = t.defaults || {};
      main.innerHTML = cmdHead(title, 'the rule listeners are measured against') +
        `<div class="tiles">
          <div class="tile"><div class="t-label">Minimum ratings</div>
            <div class="t-value">${fmtInt(t.min_ratings)}</div>
            <div class="t-sub">before the rule can fire</div></div>
          <div class="tile"><div class="t-label">Downvote share</div>
            <div class="t-value">${esc(String(t.down_ratio_pct ?? (Number(t.down_ratio_bps || 0) / 100).toFixed(2)))}%</div>
            <div class="t-sub">${fmtInt(t.down_ratio_bps)} bps</div></div>
          <div class="tile"><div class="t-label">In force from</div>
            <div class="t-value" style="font-size:15px">${t.source === 'chain'
              ? `<a href="#/block/${t.set_height}">#${fmtInt(t.set_height)}</a>` : 'built-in default'}</div>
            <div class="t-sub">${esc(t.source || '?')}</div></div>
          <div class="tile"><div class="t-label">Proposal kind</div>
            <div class="t-value">${fmtInt(t.proposal_kind)}</div>
            <div class="t-sub">SET_RATING_THRESHOLD</div></div>
        </div>
        <div class="panel"><div class="panel-title">The rule, in plain English</div>
          <div class="note">${thresholdSentence(t)}.</div>
          <div class="note" style="margin-top:6px">${thresholdOriginHtml(t)}</div>
          ${t.rule ? `<div class="mini" style="margin-top:8px">Node's own statement of the rule:
            <code>${esc(t.rule)}</code></div>` : ''}
          <div class="mini" style="margin-top:8px">A rating-driven hide freezes the counts AND the
            threshold that were in force the moment it fired, so retuning the rule later can never
            make an old hide unexplainable. A moderator who reviews and unhides makes that final —
            ratings never re-apply the hide.</div>
        </div>
        <div class="panel"><div class="panel-title">Settable range &amp; defaults</div>
          <div class="kv">
            ${kvRow('min_ratings', `${fmtInt(t.min_ratings)} <span class="dim mini">(default ${fmtInt(d.min_ratings)}; settable ${fmtInt(b.min_ratings_min)}–${fmtInt(b.min_ratings_max)})</span>`)}
            ${kvRow('down_ratio_bps', `${fmtInt(t.down_ratio_bps)} <span class="dim mini">(default ${fmtInt(d.down_ratio_bps)}; settable ${fmtInt(b.down_ratio_bps_min)}–${fmtInt(b.down_ratio_bps_max)})</span>`)}
            ${t.set_by ? kvRow('set by', addrLink(t.set_by)) : ''}
            ${t.proposal_tx ? kvRow('proposal tx', txLink(t.proposal_tx)) : ''}
          </div>
          <div class="mini" style="margin-top:8px">Only a moderator proposal of kind
            ${fmtInt(t.proposal_kind)} that reaches quorum can move these, and only inside the
            bounds above — so no single key can silence a track by moving the goalposts.</div>
        </div>` + rawDetails(t, 'Raw threshold JSON');
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
          <span class="mini">Open the <b>Commands</b> reference under the search bar for every command.</span></div>` +
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
                   moderation: 'moderation',
                   stats: 'stats' };
  // Commands that belong to the Moderation section keep that tab lit.
  const MOD_CMDS = /^(moderation|hidden|moderator|moderators)\b/;

  async function render() {
    const { seg, params } = currentRoute();
    const page = seg[0] || 'blocks';
    let tab = TAB_OF[page] || '';
    if (page === 'q' && MOD_CMDS.test(seg.slice(1).join('/'))) tab = 'moderation';
    document.querySelectorAll('#viewnav .vn-btn').forEach((b) =>
      b.setAttribute('aria-selected', String(b.dataset.tab === tab)));
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
      case 'moderation':return guard(() => viewModeration());
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
