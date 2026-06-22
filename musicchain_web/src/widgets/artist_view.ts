// Artist detail view for the musicchain web player.
//
// Browser counterpart of the artist drill-down section in
// musicchain_player/lib/src/screens/local_library_screen.dart. The Dart
// screen drills Artist -> Album -> Tracks inside the same Scaffold via
// nested state; here we render the artist-level view as a self-contained
// widget that the host shell mounts (and unmounts) in place of the
// library/search panel when the user picks an artist.
//
// Layout mirrors the Dart artist screen:
//   * Header strip — artist name (large), totals (N albums · M tracks)
//     plus a Back button that calls ctx.onClose. Equivalent to the
//     AppBar + breadcrumb in the Dart view.
//   * Album grid — one card per album with an art placeholder, title,
//     year, and a track-count chip. Tap fires ctx.onAlbumTap so the
//     host can push the album-track pane (same as the bottom split
//     pane in the Dart screen, which is the parent's responsibility
//     here).
//   * Loose-tracks list — tracks the album grouping didn't claim
//     (no `album` tag, or an empty/whitespace-only tag). Each row has
//     a play button that calls ctx.player.loadAndPlay. Mirrors the
//     "Singles" bucket the Dart helper synthesises but rendered as a
//     plain list at the bottom rather than as another album card.
//
// Pure DOM. No framework. CSS classes are atomic and live in
// public/style.css alongside the rest of the shell.

import type { SongRow } from '../verbs';
import type { NodeClient } from '../node_client';

// ---------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------

/** Album grouping the host hands us. The host is responsible for
 *  bucketing tracks by album name (case-insensitive, same as the
 *  Dart `_bucketByNorm` helper); we just render whatever it gives us. */
export interface AlbumGroup {
  /** Display name of the album. The host already picked the dominant
   *  spelling for the bucket — we render it verbatim. */
  name: string;
  /** Earliest non-zero year across the album's tracks; 0 if unknown.
   *  Matches the Dart `_earliestYear` rule. */
  year: number;
  /** Tracks in track-number order. The host did the sorting. */
  tracks: SongRow[];
}

/** Shape of the artist payload the host passes into renderArtistView. */
export interface ArtistDetail {
  /** Display name of the artist. Rendered verbatim in the header. */
  name: string;
  /** Albums attributed to this artist. May be empty. */
  albums: AlbumGroup[];
  /** Tracks not assigned to any album (no `album` tag or empty). */
  looseTracks: SongRow[];
}

/** Minimum player surface the artist view calls. The host's shell
 *  player exposes loadAndPlay(song, playerAddress) per home.ts; we
 *  accept either the row-only one-arg form or the two-arg form so
 *  tests can hand in a fake without the full ShellPlayer. */
export interface ArtistViewPlayer {
  loadAndPlay(song: SongRow, playerAddress?: string): void | Promise<void>;
}

/** Subset of NodeClient the view interacts with. Kept narrow so a
 *  fake passes typecheck without instantiating a WebSocket. */
export type ArtistViewNode = Pick<NodeClient, 'request'>;

/** Context handed to renderArtistView. */
export interface ArtistViewCtx {
  node: ArtistViewNode;
  player: ArtistViewPlayer;
  artist: ArtistDetail;
  /** Called when the user taps an album card. Host pushes the album
   *  pane (same role the Dart screen plays via _selectedAlbum). */
  onAlbumTap: (album: AlbumGroup) => void;
  /** Called when the user taps the back affordance in the header. Host
   *  unmounts the artist view and restores the previous screen. */
  onClose: () => void;
  /** Optional error sink — defaults to console.error. */
  onError?: (err: Error) => void;
}

// ---------------------------------------------------------------------
// Small DOM helpers (kept local to avoid pulling in a shared ui.ts)
// ---------------------------------------------------------------------

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  cls?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

function formatTime(ms: number | undefined): string {
  if (!ms || !Number.isFinite(ms) || ms <= 0) return '';
  const total = Math.floor(ms / 1000);
  const min = Math.floor(total / 60);
  const sec = total % 60;
  return `${min}:${String(sec).padStart(2, '0')}`;
}

function songDisplayTitle(song: SongRow): string {
  const t = (song.title ?? '').trim();
  return t.length === 0 ? '(untitled)' : t;
}

/** Total tracks across albums + loose. Mirrors what the Dart artist
 *  chip would label as `(N)`. */
function countTotalTracks(artist: ArtistDetail): number {
  let n = artist.looseTracks.length;
  for (const a of artist.albums) n += a.tracks.length;
  return n;
}

// ---------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------

function renderHeader(
  artist: ArtistDetail,
  onClose: () => void,
): HTMLElement {
  const header = el('div', 'artist-view__header');

  const back = el('button', 'artist-view__back', '← Back');
  back.type = 'button';
  back.setAttribute('aria-label', 'Back');
  back.addEventListener('click', () => {
    try { onClose(); } catch (_) { /* swallow — caller's problem */ }
  });

  const meta = el('div', 'artist-view__meta col grow');

  const name = el('h1', 'artist-view__name', artist.name || 'Unknown Artist');

  const totalAlbums = artist.albums.length;
  const totalTracks = countTotalTracks(artist);
  const totals = el(
    'div',
    'artist-view__totals muted',
    `${totalAlbums} album${totalAlbums === 1 ? '' : 's'} ` +
      `· ${totalTracks} track${totalTracks === 1 ? '' : 's'}`,
  );

  meta.append(name, totals);
  header.append(back, meta);
  return header;
}

/** One card in the album grid. The art placeholder is a CSS-only block;
 *  the host can swap in real cover art by overriding `.album-card__art`
 *  later without a code change here. */
function renderAlbumCard(
  album: AlbumGroup,
  onTap: (album: AlbumGroup) => void,
): HTMLElement {
  const card = el('button', 'album-card');
  card.type = 'button';
  card.setAttribute('aria-label', `Open album ${album.name}`);

  const art = el('div', 'album-card__art');
  // Pick a stable letter for the placeholder — first char of the album
  // name (uppercased). Matches the avatar trick the header uses for the
  // username so the visual language stays consistent.
  const letter = (album.name.charAt(0) || '?').toUpperCase();
  art.textContent = letter;
  art.setAttribute('aria-hidden', 'true');

  const body = el('div', 'album-card__body');
  const title = el('div', 'album-card__title', album.name || 'Singles');
  const sub = el('div', 'album-card__sub muted');
  const parts: string[] = [];
  if (album.year > 0) parts.push(String(album.year));
  sub.textContent = parts.join(' · ');

  const count = el(
    'span',
    'album-card__chip',
    `${album.tracks.length} track${album.tracks.length === 1 ? '' : 's'}`,
  );

  body.append(title, sub, count);
  card.append(art, body);

  card.addEventListener('click', () => {
    try { onTap(album); } catch (_) { /* swallow */ }
  });

  return card;
}

function renderAlbumGrid(
  albums: AlbumGroup[],
  onAlbumTap: (album: AlbumGroup) => void,
): HTMLElement {
  const section = el('section', 'artist-view__section');
  const heading = el('h2', 'artist-view__heading', 'Albums');
  section.append(heading);

  if (albums.length === 0) {
    section.append(el('div', 'muted', 'No albums yet.'));
    return section;
  }

  const grid = el('div', 'album-grid');
  for (const album of albums) {
    grid.append(renderAlbumCard(album, onAlbumTap));
  }
  section.append(grid);
  return section;
}

function renderLooseRow(
  track: SongRow,
  index: number,
  player: ArtistViewPlayer,
  onError: (err: Error) => void,
): HTMLElement {
  const row = el('div', 'loose-track');
  row.setAttribute('role', 'listitem');

  const numCell = el('div', 'loose-track__num mono muted',
    track.track_number && track.track_number > 0
      ? String(track.track_number)
      : String(index + 1));

  const meta = el('div', 'loose-track__meta col grow');
  const title = el('div', 'loose-track__title', songDisplayTitle(track));
  const sub = el('div', 'loose-track__sub muted');
  const dur = formatTime(track.duration_ms);
  const subParts: string[] = [];
  if (track.album && track.album.trim().length > 0) subParts.push(track.album.trim());
  if (dur) subParts.push(dur);
  sub.textContent = subParts.join('  •  ');
  meta.append(title, sub);

  // Per-row Play button removed: the whole row is the play target now.
  // Click + keyboard activation drive `fire()`; the row also reads as a
  // button to assistive tech via role + aria-label.
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
      // The host's shell player exposes loadAndPlay(song, address) —
      // see ShellPlayer in screens/home.ts. We forward only the song;
      // the host's wrapper closure already has the wallet address.
      await player.loadAndPlay(track);
    } catch (e) {
      const err = e instanceof Error ? e : new Error(String(e));
      try { onError(err); } catch (_) { /* swallow */ }
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

function renderLooseTracks(
  tracks: SongRow[],
  player: ArtistViewPlayer,
  onError: (err: Error) => void,
): HTMLElement | null {
  if (tracks.length === 0) return null;

  const section = el('section', 'artist-view__section');
  const heading = el('h2', 'artist-view__heading',
    tracks.length === 1 ? 'Loose track' : 'Loose tracks');
  section.append(heading);

  const list = el('div', 'loose-list', undefined);
  list.setAttribute('role', 'list');
  for (let i = 0; i < tracks.length; i++) {
    const track = tracks[i];
    if (!track) continue;
    list.append(renderLooseRow(track, i, player, onError));
  }
  section.append(list);
  return section;
}

// ---------------------------------------------------------------------
// Public render
// ---------------------------------------------------------------------

/**
 * Mount the artist detail view inside `container` using `ctx`. Replaces
 * any existing children. The render is one-shot — there are no
 * subscriptions to tear down — so the host can simply call
 * `container.replaceChildren()` to dispose.
 *
 * `ctx.node` is exposed in the signature for future RPCs (e.g. fetching
 * cover art via an `art.get` verb the chain might gain), even though the
 * current implementation doesn't make any node requests. Keeping it in
 * the type now means callers don't have to be refactored when we wire
 * cover art on the home node side.
 */
export function renderArtistView(
  container: HTMLElement,
  ctx: ArtistViewCtx,
): void {
  // `node` isn't read today; the binding documents the contract. Avoid
  // tsc's noUnusedLocals by referencing it through the `void` operator.
  void ctx.node;

  container.replaceChildren();

  const onError =
    ctx.onError ??
    ((err: Error) => {
      // eslint-disable-next-line no-console
      console.error('artist-view:', err);
    });

  const root = el('div', 'main-pane col artist-view');
  root.setAttribute('data-screen', 'artist');

  // Header (back button + name + totals).
  root.append(renderHeader(ctx.artist, ctx.onClose));

  // Album grid section.
  root.append(renderAlbumGrid(ctx.artist.albums, ctx.onAlbumTap));

  // Loose-tracks list at the bottom (only if there are any).
  const loose = renderLooseTracks(ctx.artist.looseTracks, ctx.player, onError);
  if (loose) root.append(loose);

  container.append(root);
}
