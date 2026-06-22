// Discover-style shell for the web player's Library tab.
//
// Direct port of the Android player's local library screen at
//   musicchain_player/lib/src/screens/local_library_screen.dart.
//
// The Dart screen presents a faceted drill: a Mode toolbar (Artist | Genre)
// switches the root facet, a top "pill" pane shows the values for the
// current facet (counts in parentheses), tapping a value drills into the
// next level (Genre → Artist → Album), and once an Album is selected a
// bottom pane reveals the tracks in that album. A breadcrumb above the
// top pane lets the user pop back up the stack one level at a time.
//
// We mirror that structure in pure DOM here. The data source is the
// chain's `songs.list` RPC (SongRow[]); bucketing happens client-side via
// the same _artistKey / _albumKey / case-insensitive normalization the
// Dart side does, so spellings like "FIDLAR" / "Fidlar" or "Rock" / "rock"
// coalesce onto a single chip.
//
// The browser doesn't have a real filesystem scanner, so the AppBar
// actions (Folders, Scan-now) are present for parity but kick the host's
// existing local-file picker via a hook on `ctx` (if provided) or no-op
// with a status message otherwise. The local-only "My files" sublist
// from the previous implementation has been dropped — those flows lived
// in the on-disk library and the new shell is chain-driven.
//
// Render contract:
//   renderLibrary(container, ctx)
//     - Replaces container's children.
//     - Mounts the three-pane shell: mode-toolbar, breadcrumb, top
//       pill / chip pane, and a bottom track pane that appears when an
//       album is selected.
//     - Re-renders in place as state changes; no external state container.
//     - Tap-to-play calls ctx.player.load(songRow) and forwards errors
//       through ctx.onError.

import type { NodeClient } from '../node_client.js';
import { RpcError } from '../node_client.js';
import type { Wallet } from '../wallet.js';
import type { SongRow } from '../verbs.js';
import { listSongs } from '../verbs.js';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** What the library screen needs out of the host's player wrapper. The
 *  shell only ever hands back SongRow values from the chain — the host
 *  decides how to resolve a stream URL and start a play session. */
export interface LibraryPlayer {
  load(song: SongRow): void | Promise<void>;
}

/** Subset of NodeClient the screen calls. Narrow on purpose so tests can
 *  hand in a fake without instantiating a WebSocket. */
export interface LibraryNode {
  request<T = unknown>(
    type: string,
    body: unknown,
    timeoutMs?: number,
  ): Promise<T>;
}

/** App-wide context passed through from main.ts / the home shell. The
 *  wallet is required so the renderer can echo the player address back
 *  into the host's session.start, if it wants. */
export interface LibraryCtx {
  wallet: Pick<Wallet, 'address'>;
  node: LibraryNode | NodeClient;
  player: LibraryPlayer;
  /** Optional error sink — defaults to console.error. */
  onError?: (err: Error) => void;
  /** Optional hook fired by the AppBar's "Folders" action. The Android
   *  side opens a folder-picker screen here; the web has nothing
   *  comparable in the chain shell, so the host can wire this to its
   *  own File System Access flow or leave it unset to disable the
   *  affordance. */
  onOpenFolders?: () => void | Promise<void>;
  /** Optional hook fired by the AppBar's "Scan now" action. Same
   *  rationale as onOpenFolders — the chain doesn't scan; the host can
   *  trigger a refetch + a fingerprint walk over the user's local set. */
  onScanNow?: () => void | Promise<void>;
}

// ---------------------------------------------------------------------------
// Facet / drill state
// ---------------------------------------------------------------------------

/** Mirror of Dart's `_FacetMode` — Artist or Genre at the root. */
type FacetMode = 'artist' | 'genre';

/** Mirror of Dart's `_DrillLevel`. The current drill level is a function
 *  of (mode, drillGenre, drillArtist) — see currentLevel() below. */
type DrillLevel = 'genre' | 'artist' | 'album';

interface FacetMeta {
  label: string;
  rootLabel: string;
  // Single-glyph icon stand-in for the Material IconData the Dart side
  // attaches. Pure DOM here means we lean on Unicode rather than an icon
  // font — these match the spirit of person_outline / style_outlined.
  icon: string;
}

const FACET_META: Record<FacetMode, FacetMeta> = {
  artist: { label: 'Artist', rootLabel: 'Artists', icon: '\u{1F464}' },  // bust silhouette
  genre:  { label: 'Genre',  rootLabel: 'Genres',  icon: '\u{1F3B5}' },  // musical note
};

// Pill-icon for the album drill level. The Dart side uses
// Icons.album_outlined; Unicode optical disc is close enough.
const ALBUM_ICON = '\u{1F4BF}';

// ---------------------------------------------------------------------------
// Bucket / sort helpers — direct ports of the Dart helpers.
// ---------------------------------------------------------------------------

function artistKey(s: SongRow): string {
  const t = (s.artist ?? '').trim();
  return t.length === 0 ? 'Unknown Artist' : t;
}

function genreKey(s: SongRow): string {
  const t = (s.genre ?? '').trim();
  return t.length === 0 ? 'Unknown Genre' : t;
}

function albumKey(s: SongRow): string {
  const t = (s.album ?? '').trim();
  return t.length === 0 ? 'Singles' : t;
}

function norm(s: string): string {
  return s.toLowerCase();
}

/** Sort comparator that pushes "Unknown …" and "Singles" buckets to the
 *  bottom of an otherwise case-insensitive alphabetical sort. Matches
 *  Dart's `_sortAlpha`. */
function sortAlpha(a: string, b: string): number {
  const ua = a.startsWith('Unknown') || a === 'Singles';
  const ub = b.startsWith('Unknown') || b === 'Singles';
  if (ua && !ub) return 1;
  if (ub && !ua) return -1;
  const al = a.toLowerCase();
  const bl = b.toLowerCase();
  if (al < bl) return -1;
  if (al > bl) return 1;
  return 0;
}

/** Earliest year > 0 across `tracks`, or 0 if none has a year. */
function earliestYear(tracks: SongRow[]): number {
  let y = 0;
  for (const e of tracks) {
    const ey = e.year ?? 0;
    if (ey > 0 && (y === 0 || ey < y)) y = ey;
  }
  return y;
}

/** Distinct-track count by dedup key (fingerprint > content_hash). The
 *  Dart side has a `canonicalHash` field too; the chain's SongRow doesn't
 *  separate canonical vs raw, so fingerprint + content_hash is all we
 *  can use. Same rule: prefer the perceptual hash when available, fall
 *  back to bytes-hash. */
function dedupKey(s: SongRow): string {
  const fp = (s.fingerprint_hash ?? '').trim();
  if (fp.length > 0) return `fp:${fp}`;
  return `ch:${s.content_hash}`;
}

function distinctTrackCount(entries: SongRow[]): number {
  const seen = new Set<string>();
  for (const e of entries) seen.add(dedupKey(e));
  return seen.size;
}

/** Group `items` by `normFn(item)` and pick the most-common spelling of
 *  `displayFn(item)` as the bucket key. Direct port of Dart's
 *  `_bucketByNorm` so "FIDLAR" + "Fidlar" coalesce to one chip with
 *  whichever spelling dominates. */
function bucketByNorm(
  items: SongRow[],
  displayFn: (s: SongRow) => string,
  normFn: (s: SongRow) => string,
): Map<string, SongRow[]> {
  // Per-normalized-key: { variant spelling -> count } and the bucket
  // contents list itself.
  const normToVariants = new Map<string, Map<string, number>>();
  const normToTracks   = new Map<string, SongRow[]>();
  for (const e of items) {
    const n = normFn(e);
    const d = displayFn(e);
    let tracks = normToTracks.get(n);
    if (!tracks) {
      tracks = [];
      normToTracks.set(n, tracks);
    }
    tracks.push(e);
    let variants = normToVariants.get(n);
    if (!variants) {
      variants = new Map<string, number>();
      normToVariants.set(n, variants);
    }
    variants.set(d, (variants.get(d) ?? 0) + 1);
  }
  const out = new Map<string, SongRow[]>();
  normToVariants.forEach((variants, n) => {
    let best = '';
    let bestCount = -1;
    variants.forEach((c, d) => {
      // Tiebreak on longer display string — matches Dart's
      // `(c == bestCount && d.length > best.length)` branch.
      if (c > bestCount || (c === bestCount && d.length > best.length)) {
        best = d;
        bestCount = c;
      }
    });
    const tracks = normToTracks.get(n);
    if (tracks !== undefined) out.set(best, tracks);
  });
  return out;
}

// ---------------------------------------------------------------------------
// DOM helpers (small + colocated)
// ---------------------------------------------------------------------------

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  cls?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const n = document.createElement(tag);
  if (cls !== undefined && cls.length > 0) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

function formatTime(ms: number | undefined): string {
  if (ms === undefined || !Number.isFinite(ms) || ms <= 0) return '--:--';
  const total = Math.floor(ms / 1000);
  const min = Math.floor(total / 60);
  const sec = total % 60;
  return `${min}:${String(sec).padStart(2, '0')}`;
}

function songDisplayTitle(s: SongRow): string {
  const t = (s.title ?? '').trim();
  return t.length === 0 ? '(untitled)' : t;
}

// ---------------------------------------------------------------------------
// Public render
// ---------------------------------------------------------------------------

interface CrumbSeg {
  label: string;
  onTap: (() => void) | null;
}

/** Mount the library screen into `container`, using `ctx` for RPC,
 *  playback, and wallet metadata. Replaces any existing children. */
export function renderLibrary(
  container: HTMLElement,
  ctx: LibraryCtx,
): void {
  container.replaceChildren();

  // -- State -----------------------------------------------------------

  let mode: FacetMode = 'artist';
  let drillGenre:    string | null = null;
  let drillArtist:   string | null = null;
  let selectedAlbum: string | null = null;

  let entries: SongRow[] = [];
  let status: 'loading' | 'loaded' | 'error' = 'loading';
  let errorText = '';

  let scanning = false;

  // -- Skeleton --------------------------------------------------------

  const root = el('div', 'main-pane mc-library col');
  root.setAttribute('data-screen', 'library');

  // App bar: title + Folders + Scan-now.
  const appBar = el('div', 'mc-library__appbar row');
  const appBarTitle = el('h2', 'mc-library__title', 'My Library');
  const appBarActions = el('div', 'row');
  const foldersBtn = el('button', undefined, 'Folders');
  foldersBtn.type = 'button';
  foldersBtn.setAttribute('aria-label', 'Folders');
  foldersBtn.title = 'Folders';
  const scanBtn = el('button', undefined, 'Scan now');
  scanBtn.type = 'button';
  scanBtn.setAttribute('aria-label', 'Scan now');
  scanBtn.title = 'Scan now';
  appBarActions.append(foldersBtn, scanBtn);
  appBar.append(appBarTitle, appBarActions);

  // Mode toolbar: Artist / Genre segmented control.
  const modeBar = el('div', 'mc-library__modebar row');
  const modeArtistBtn = el('button', 'mc-seg', `${FACET_META.artist.icon} ${FACET_META.artist.label}`);
  modeArtistBtn.type = 'button';
  modeArtistBtn.setAttribute('aria-pressed', 'true');
  const modeGenreBtn = el('button', 'mc-seg', `${FACET_META.genre.icon} ${FACET_META.genre.label}`);
  modeGenreBtn.type = 'button';
  modeGenreBtn.setAttribute('aria-pressed', 'false');
  modeBar.append(modeArtistBtn, modeGenreBtn);

  // Breadcrumb strip.
  const crumb = el('div', 'mc-library__crumb row');

  // Status / error / loading strip just below the crumb.
  const statusStrip = el('div', 'mc-library__status muted');

  // Top pane: the pill / chip wrap.
  const topPane = el('div', 'mc-library__top');

  // Bottom pane: selected album's track list (initially absent).
  const bottomPane = el('div', 'mc-library__bottom');
  bottomPane.style.display = 'none';

  root.append(appBar, modeBar, crumb, statusStrip, topPane, bottomPane);
  container.appendChild(root);

  // -- Filtering / pill building --------------------------------------

  function drillFilter(all: SongRow[]): SongRow[] {
    const wantedGenre  = drillGenre  === null ? null : drillGenre.toLowerCase();
    const wantedArtist = drillArtist === null ? null : drillArtist.toLowerCase();
    if (wantedGenre === null && wantedArtist === null) return all;
    return all.filter((e) => {
      if (wantedGenre  !== null && norm(genreKey(e))  !== wantedGenre)  return false;
      if (wantedArtist !== null && norm(artistKey(e)) !== wantedArtist) return false;
      return true;
    });
  }

  function currentLevel(): DrillLevel {
    if (drillArtist !== null) return 'album';
    if (drillGenre  !== null) return 'artist';
    return mode === 'artist' ? 'artist' : 'genre';
  }

  function buildCrumb(): CrumbSeg[] {
    const out: CrumbSeg[] = [
      { label: FACET_META[mode].rootLabel, onTap: () => crumbBack(0) },
    ];
    if (drillGenre !== null) {
      out.push({ label: drillGenre, onTap: () => crumbBack(1) });
    }
    if (drillArtist !== null) {
      out.push({ label: drillArtist, onTap: () => crumbBack(2) });
    }
    if (selectedAlbum !== null) {
      out.push({ label: selectedAlbum, onTap: null });
    }
    return out;
  }

  function crumbBack(targetDepth: number): void {
    if (targetDepth < 3) selectedAlbum = null;
    if (targetDepth < 2) drillArtist   = null;
    if (targetDepth < 1) drillGenre    = null;
    paint();
  }

  function selectMode(m: FacetMode): void {
    mode = m;
    drillGenre    = null;
    drillArtist   = null;
    selectedAlbum = null;
    paint();
  }

  function onPillTapped(key: string, level: DrillLevel): void {
    switch (level) {
      case 'genre':
        drillGenre    = drillGenre === key ? null : key;
        drillArtist   = null;
        selectedAlbum = null;
        break;
      case 'artist':
        drillArtist   = drillArtist === key ? null : key;
        selectedAlbum = null;
        break;
      case 'album':
        selectedAlbum = selectedAlbum === key ? null : key;
        break;
    }
    paint();
  }

  function formatAlbumLabel(name: string, tracks: SongRow[]): string {
    const year = earliestYear(tracks);
    const n = distinctTrackCount(tracks);
    if (year > 0) return `${name}  (${year} · ${n})`;
    return `${name}  (${n})`;
  }

  function sortEntries(tracks: SongRow[]): SongRow[] {
    const out = [...tracks];
    out.sort((a, b) => {
      // Inside an album already? Track number is enough.
      const inAlbum = drillArtist !== null && drillGenre !== null;
      if (!inAlbum) {
        const aa = (a.album ?? '').trim();
        const ab = (b.album ?? '').trim();
        if (aa.length === 0 && ab.length > 0) return 1;
        if (ab.length === 0 && aa.length > 0) return -1;
        const ay = a.year ?? 0;
        const by = b.year ?? 0;
        if (ay > 0 && by > 0 && ay !== by) return ay - by;
        const aal = aa.toLowerCase();
        const abl = ab.toLowerCase();
        if (aal < abl) return -1;
        if (aal > abl) return 1;
      }
      const at = a.track_number ?? 0;
      const bt = b.track_number ?? 0;
      if (at > 0 && bt > 0 && at !== bt) return at - bt;
      if (at > 0 && bt === 0) return -1;
      if (bt > 0 && at === 0) return 1;
      const atit = songDisplayTitle(a).toLowerCase();
      const btit = songDisplayTitle(b).toLowerCase();
      if (atit < btit) return -1;
      if (atit > btit) return 1;
      return 0;
    });
    return out;
  }

  // -- Renderers ------------------------------------------------------

  function renderCrumb(): void {
    crumb.replaceChildren();
    const segs = buildCrumb();
    const frag = document.createDocumentFragment();
    for (let i = 0; i < segs.length; i++) {
      const seg = segs[i];
      const isLast = i === segs.length - 1;
      if (i > 0) {
        const sep = el('span', 'mc-library__crumb-sep', '›');
        frag.appendChild(sep);
      }
      if (seg.onTap === null) {
        const span = el('span', 'mc-library__crumb-cur', seg.label);
        if (isLast) span.setAttribute('aria-current', 'page');
        frag.appendChild(span);
      } else {
        const btn = el('button', 'mc-library__crumb-link', seg.label);
        btn.type = 'button';
        const tap = seg.onTap;
        btn.addEventListener('click', () => tap());
        frag.appendChild(btn);
      }
    }
    crumb.appendChild(frag);
  }

  function renderStatus(): void {
    if (status === 'loading') {
      statusStrip.textContent = 'Loading songs from the home node…';
      statusStrip.className = 'mc-library__status muted';
      return;
    }
    if (status === 'error') {
      statusStrip.textContent = `Couldn't load library: ${errorText}`;
      statusStrip.className = 'mc-library__status err';
      return;
    }
    if (scanning) {
      statusStrip.textContent = 'Scanning…';
      statusStrip.className = 'mc-library__status muted';
      return;
    }
    if (entries.length === 0) {
      statusStrip.textContent =
        'No songs yet. Use Folders to add a music folder, then Scan now.';
      statusStrip.className = 'mc-library__status muted';
      return;
    }
    statusStrip.textContent = '';
    statusStrip.className = 'mc-library__status muted';
  }

  function renderModeBar(): void {
    const isArtist = mode === 'artist';
    modeArtistBtn.classList.toggle('mc-seg--active', isArtist);
    modeGenreBtn.classList.toggle('mc-seg--active', !isArtist);
    modeArtistBtn.setAttribute('aria-pressed', isArtist ? 'true' : 'false');
    modeGenreBtn.setAttribute('aria-pressed', isArtist ? 'false' : 'true');
  }

  function renderTopPane(): void {
    topPane.replaceChildren();
    if (status !== 'loaded' || entries.length === 0) {
      // Status strip is carrying the message already; leave the pane empty
      // so we don't compete for the user's attention.
      return;
    }
    const level = currentLevel();
    const scoped = drillFilter(entries);
    let pills: HTMLElement[];
    switch (level) {
      case 'genre':
        pills = buildGenrePills(scoped);
        break;
      case 'artist':
        pills = buildArtistPills(scoped);
        break;
      case 'album':
        pills = buildAlbumPills(scoped);
        break;
    }
    if (pills.length === 0) {
      const crumbs = buildCrumb();
      const last = crumbs[crumbs.length - 1];
      const m = el(
        'div',
        'mc-library__empty muted',
        `Nothing under "${last?.label ?? ''}" yet.`,
      );
      topPane.appendChild(m);
      return;
    }
    const wrap = el('div', 'mc-library__pills');
    for (const p of pills) wrap.appendChild(p);
    topPane.appendChild(wrap);
  }

  function buildGenrePills(scoped: SongRow[]): HTMLElement[] {
    const buckets = bucketByNorm(scoped, genreKey, (e) => norm(genreKey(e)));
    const keys = Array.from(buckets.keys()).sort(sortAlpha);
    const out: HTMLElement[] = [];
    for (const k of keys) {
      const ts = buckets.get(k) ?? [];
      out.push(buildChip({
        icon: FACET_META.genre.icon,
        label: `${k}  (${distinctTrackCount(ts)})`,
        selected: false,
        onTap: () => onPillTapped(k, 'genre'),
      }));
    }
    return out;
  }

  function buildArtistPills(scoped: SongRow[]): HTMLElement[] {
    const buckets = bucketByNorm(scoped, artistKey, (e) => norm(artistKey(e)));
    const keys = Array.from(buckets.keys()).sort(sortAlpha);
    const out: HTMLElement[] = [];
    for (const k of keys) {
      const ts = buckets.get(k) ?? [];
      out.push(buildChip({
        icon: FACET_META.artist.icon,
        label: `${k}  (${distinctTrackCount(ts)})`,
        selected: false,
        onTap: () => onPillTapped(k, 'artist'),
      }));
    }
    return out;
  }

  function buildAlbumPills(scoped: SongRow[]): HTMLElement[] {
    const buckets = bucketByNorm(scoped, albumKey, (e) => norm(albumKey(e)));
    const keys = Array.from(buckets.keys()).sort((a, b) => {
      const ya = earliestYear(buckets.get(a) ?? []);
      const yb = earliestYear(buckets.get(b) ?? []);
      if (ya > 0 && yb > 0 && ya !== yb) return ya - yb;
      if (ya > 0 && yb === 0) return -1;
      if (yb > 0 && ya === 0) return 1;
      return sortAlpha(a, b);
    });
    const out: HTMLElement[] = [];
    const sel = selectedAlbum === null ? null : selectedAlbum.toLowerCase();
    for (const k of keys) {
      const ts = buckets.get(k) ?? [];
      out.push(buildChip({
        icon: ALBUM_ICON,
        label: formatAlbumLabel(k, ts),
        selected: sel !== null && k.toLowerCase() === sel,
        onTap: () => onPillTapped(k, 'album'),
      }));
    }
    return out;
  }

  function buildChip(opts: {
    icon: string;
    label: string;
    selected: boolean;
    onTap: () => void;
  }): HTMLElement {
    const chip = el('button', 'mc-chip');
    chip.type = 'button';
    if (opts.selected) chip.classList.add('mc-chip--selected');
    const iconSpan = el('span', 'mc-chip__icon', opts.icon);
    const labelSpan = el('span', 'mc-chip__label', opts.label);
    chip.append(iconSpan, labelSpan);
    chip.addEventListener('click', () => opts.onTap());
    return chip;
  }

  function renderBottomPane(): void {
    if (selectedAlbum === null) {
      bottomPane.replaceChildren();
      bottomPane.style.display = 'none';
      return;
    }
    bottomPane.replaceChildren();
    bottomPane.style.display = '';

    const wantedAlbum = selectedAlbum.toLowerCase();
    const scoped = drillFilter(entries).filter(
      (e) => norm(albumKey(e)) === wantedAlbum,
    );
    const tracks = sortEntries(scoped);

    // Header row: album name + meta + close button.
    const head = el('div', 'mc-library__bottom-head row');
    const headIcon = el('span', 'mc-library__bottom-icon', ALBUM_ICON);
    const headTextWrap = el('div', 'col grow');
    headTextWrap.style.gap = '2px';
    const headTitle = el('div', 'mc-library__bottom-title', selectedAlbum);
    const firstArtist = tracks.length > 0
      ? (tracks[0].artist ?? '').trim()
      : '';
    const artist = firstArtist.length > 0 ? firstArtist : (drillArtist ?? '');
    const yearVal = (() => {
      for (const t of tracks) {
        const y = t.year ?? 0;
        if (y > 0) return y;
      }
      return 0;
    })();
    const parts: string[] = [];
    if (artist.length > 0) parts.push(artist);
    if (yearVal > 0) parts.push(String(yearVal));
    parts.push(`${tracks.length} track${tracks.length === 1 ? '' : 's'}`);
    const headSub = el(
      'div',
      'mc-library__bottom-sub muted',
      parts.join('  ·  '),
    );
    headTextWrap.append(headTitle, headSub);

    const closeBtn = el('button', 'mc-library__close', '×');
    closeBtn.type = 'button';
    closeBtn.setAttribute('aria-label', 'Close');
    closeBtn.title = 'Close';
    closeBtn.addEventListener('click', () => {
      selectedAlbum = null;
      paint();
    });

    head.append(headIcon, headTextWrap, closeBtn);
    bottomPane.appendChild(head);

    // Track list.
    const list = el('div', 'mc-library__tracks col');
    list.setAttribute('role', 'list');
    for (let i = 0; i < tracks.length; i++) {
      list.appendChild(renderTrackRow(tracks[i], i));
    }
    bottomPane.appendChild(list);
  }

  function renderTrackRow(track: SongRow, idx: number): HTMLElement {
    const row = el('div', 'mc-library__track row');
    row.setAttribute('role', 'listitem');

    const numCell = el(
      'div',
      'mc-library__track-num muted',
      String((track.track_number ?? 0) > 0 ? track.track_number : idx + 1),
    );

    const meta = el('div', 'col grow');
    meta.style.gap = '2px';
    const title = el('div', 'mc-library__track-title', songDisplayTitle(track));
    const subParts: string[] = [];
    const albumStr = (track.album ?? '').trim();
    if (albumStr.length > 0) subParts.push(albumStr);
    const artistStr = (track.artist ?? '').trim();
    if (artistStr.length > 0) subParts.push(artistStr);
    subParts.push(formatTime(track.duration_ms));
    const sub = el(
      'div',
      'mc-library__track-sub muted',
      subParts.join('  •  '),
    );
    meta.append(title, sub);

    // Per-row Play button removed. Click the row to play; the loading
    // state is conveyed via a `.loading` class so styling can swap in a
    // small spinner without us having to mutate button text.
    row.classList.add('clickable');
    row.setAttribute('role', 'button');
    row.setAttribute('tabindex', '0');
    row.setAttribute('aria-label', `Play ${songDisplayTitle(track)}`);

    let inFlight = false;
    async function fire(): Promise<void> {
      if (inFlight) return;
      inFlight = true;
      row.classList.add('loading');
      try {
        await ctx.player.load(track);
      } catch (e) {
        const err = e instanceof Error ? e : new Error(String(e));
        reportError(err);
        sub.textContent = `Couldn't play: ${err.message}`;
        sub.className = 'mc-library__track-sub err';
      } finally {
        inFlight = false;
        row.classList.remove('loading');
      }
    }
    row.addEventListener('click', () => { void fire(); });
    row.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter' || ev.key === ' ') {
        ev.preventDefault();
        void fire();
      }
    });

    row.append(numCell, meta);
    return row;
  }

  function paint(): void {
    renderCrumb();
    renderStatus();
    renderModeBar();
    renderTopPane();
    renderBottomPane();
  }

  // -- AppBar wiring --------------------------------------------------

  function reportError(err: Error): void {
    if (ctx.onError) {
      try { ctx.onError(err); } catch { /* swallow */ }
    } else {
      // eslint-disable-next-line no-console
      console.error('library:', err);
    }
  }

  async function refresh(): Promise<void> {
    status = 'loading';
    paint();
    try {
      const songs = await listSongs(ctx.node as NodeClient);
      // Drop malformed rows: anything without a content_hash can't be
      // played and would crash the dedup/sort steps.
      entries = songs.filter(
        (s) => s != null && typeof s.content_hash === 'string'
          && s.content_hash.length > 0,
      );
      status = 'loaded';
      errorText = '';
    } catch (e) {
      status = 'error';
      errorText = e instanceof RpcError
        ? `${e.status}: ${e.message}`
        : (e instanceof Error ? e.message : String(e));
      entries = [];
    }
    paint();
  }

  foldersBtn.addEventListener('click', () => {
    if (!ctx.onOpenFolders) {
      // Surface in the status strip rather than silently swallowing so
      // the user can see "we heard you, but there's no handler wired".
      statusStrip.textContent = 'Folders picker not available in this build.';
      statusStrip.className = 'mc-library__status muted';
      return;
    }
    try {
      const r = ctx.onOpenFolders();
      if (r && typeof (r as Promise<void>).then === 'function') {
        void (r as Promise<void>).catch((err) => {
          reportError(err instanceof Error ? err : new Error(String(err)));
        });
      }
    } catch (err) {
      reportError(err instanceof Error ? err : new Error(String(err)));
    }
  });

  scanBtn.addEventListener('click', () => {
    if (scanning) return;
    scanning = true;
    scanBtn.disabled = true;
    paint();
    void (async () => {
      try {
        if (ctx.onScanNow) {
          await ctx.onScanNow();
        }
        // After scan, re-fetch the chain list so any new fingerprints
        // surface. Even if onScanNow is unset we still want refresh()
        // so the user gets a "Scan now" button that at least syncs the
        // chain view.
        await refresh();
      } catch (err) {
        reportError(err instanceof Error ? err : new Error(String(err)));
      } finally {
        scanning = false;
        scanBtn.disabled = false;
        paint();
      }
    })();
  });

  modeArtistBtn.addEventListener('click', () => {
    if (mode !== 'artist') selectMode('artist');
  });
  modeGenreBtn.addEventListener('click', () => {
    if (mode !== 'genre') selectMode('genre');
  });

  // -- Initial paint + first fetch ------------------------------------

  // Wire up the wallet hint into a data-attribute so future extensions
  // (e.g. mint counters) can read it without us threading another ctx
  // field; mirrors the same pattern search.ts uses for player addresses.
  root.setAttribute('data-wallet', ctx.wallet.address);

  paint();
  void refresh();
}
