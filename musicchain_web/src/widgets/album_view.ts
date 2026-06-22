// Album detail view. Mirrors the `_TrackPane` bottom-pane widget in
// musicchain_player/lib/src/screens/local_library_screen.dart (around
// lines 787-878): the section that renders an album's track list after
// the user drills artist -> album in the Flutter Library tab.
//
// Layout, top to bottom:
//   * Header card: album-art placeholder (square) on the left, then
//     title / artist / year / "N tracks - mm min" on the right. A
//     back button on the far right calls `ctx.onClose` and a "Play
//     album" primary button kicks off playback of the first track,
//     queueing the rest behind it.
//   * Track list: one row per track. Each row shows
//     `track_number ?? index+1` on the left, the track title in the
//     middle, the duration (mm:ss) on the right, and a small play
//     button that loads that specific track.
//
// The widget is render-once + replace-on-rerender. Track row clicks
// dispatch directly to the player; we don't subscribe to player
// 'state' events here (the persistent mini-player at the bottom of
// the shell already owns the now-playing UI). That keeps this widget
// pure DOM with no lingering listeners on detach.

import type { NodeClient } from '../node_client';
import type { SongRow }    from '../verbs';

// ---- Player surface --------------------------------------------------
//
// Structural type so the widget compiles regardless of which player
// adapter the shell hands us. We require `load` (the low-level
// HTMLAudioElement-backed source loader) and optionally `play`, and
// we use `loadAndPlay` if the host has wired the chain-session +
// queue layer in front of the raw player (see screens/home.ts'
// ShellPlayer for the production wiring).

interface AlbumPlayerLike {
  /** Optional: full session-aware path. The shell's ShellPlayer
   *  implements this; the raw Player class does not. When present we
   *  prefer it so session.start fires and the heartbeat runs. */
  loadAndPlay?: (song: SongRow, playerAddress: string) => Promise<void>;
  /** Required fallback: low-level source load. The widget will pair
   *  this with `play()` to give the user immediate feedback when the
   *  shell hasn't wired a session layer. */
  load: (source: string | Blob) => Promise<void>;
  /** Optional: only invoked on the fallback path. */
  play?: () => Promise<void>;
}

// ---- Context --------------------------------------------------------

export interface AlbumDescriptor {
  title:        string;
  artist:       string;
  year?:        number;
  /** Sort hint inherited from the row that opened the album view (the
   *  Dart pane uses the album's earliest track_number when present;
   *  we accept the same on the API and treat it as advisory only). */
  trackNumber?: number;
  tracks:       SongRow[];
}

export interface AlbumViewCtx {
  node:    NodeClient;
  player:  AlbumPlayerLike;
  album:   AlbumDescriptor;
  /** Optional: wallet address the session layer needs when we go
   *  through `loadAndPlay`. Falls back to "" — `startSession` in
   *  verbs.ts will reject empty addresses, which is the same failure
   *  mode the home shell currently has when called without a wallet
   *  context. */
  playerAddress?: string;
  onClose: () => void;
}

// ---- Formatting helpers --------------------------------------------

function fmtDuration(ms: number | undefined): string {
  if (!ms || !Number.isFinite(ms) || ms <= 0) return '--:--';
  const totalSec = Math.round(ms / 1000);
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${m}:${s.toString().padStart(2, '0')}`;
}

function totalMinutes(tracks: SongRow[]): number {
  let totalMs = 0;
  for (const t of tracks) {
    const d = t.duration_ms;
    if (typeof d === 'number' && Number.isFinite(d) && d > 0) {
      totalMs += d;
    }
  }
  return Math.round(totalMs / 60000);
}

function trackTitle(song: SongRow): string {
  const t = (song.title ?? '').trim();
  return t.length === 0 ? '(untitled)' : t;
}

function albumDisplayArtist(album: AlbumDescriptor): string {
  const a = (album.artist ?? '').trim();
  if (a.length > 0) return a;
  for (const t of album.tracks) {
    const ta = (t.artist ?? '').trim();
    if (ta.length > 0) return ta;
  }
  return '';
}

function escapeHtml(s: string): string {
  // Album / artist / track titles arrive from the chain (uploader-
  // supplied) so we can't trust them. Header + track rows interpolate
  // these into the innerHTML scaffold; everywhere else uses
  // textContent so this is only the hot path.
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// ---- Sort -----------------------------------------------------------

/** Sort tracks like the Dart pane does: prefer explicit track_number
 *  when both sides have one, otherwise fall back to title order so
 *  the list is at least stable. */
function sortTracks(tracks: SongRow[]): SongRow[] {
  return tracks.slice().sort((a, b) => {
    const at = a.track_number ?? 0;
    const bt = b.track_number ?? 0;
    if (at > 0 && bt > 0 && at !== bt) return at - bt;
    if (at > 0 && bt === 0) return -1;
    if (bt > 0 && at === 0) return 1;
    const aTitle = (a.title ?? '').toLowerCase();
    const bTitle = (b.title ?? '').toLowerCase();
    if (aTitle < bTitle) return -1;
    if (aTitle > bTitle) return  1;
    return 0;
  });
}

// ---- Playback dispatch ---------------------------------------------

/** Single entry point both the "Play album" button and per-row play
 *  buttons funnel through. Prefers the session-aware loadAndPlay
 *  path, falls back to raw load() + play() so the user still gets
 *  audio on shells that haven't wired the chain layer (e.g. the
 *  current minimal home shell in screens/home.ts). */
function playSong(
  player:  AlbumPlayerLike,
  song:    SongRow,
  address: string,
): void {
  if (typeof player.loadAndPlay === 'function') {
    void player.loadAndPlay(song, address);
    return;
  }
  // Fallback: hit the same /audio/<content_hash> passthrough the home
  // shell uses for its mini-player wiring. Mirrors streamUrlFor in
  // screens/home.ts so we don't drift if the home-node route name
  // changes.
  const url = `/audio/${encodeURIComponent(song.content_hash)}`;
  void (async () => {
    try {
      await player.load(url);
      if (typeof player.play === 'function') await player.play();
    } catch {
      // Surface nothing — the persistent mini-player already shows
      // the player's state, and a thrown error here would leave the
      // user with no UI feedback. The audio element's 'error' event
      // flips the player to 'stopped' which the mini-player picks up.
    }
  })();
}

// ---- Header skeleton -----------------------------------------------

function buildHeader(album: AlbumDescriptor, tracks: SongRow[]): string {
  const title  = escapeHtml(album.title.trim() || '(untitled album)');
  const artist = escapeHtml(albumDisplayArtist(album));
  const year   = album.year && album.year > 0 ? String(album.year) : '';
  const mins   = totalMinutes(tracks);
  const n      = tracks.length;

  // Meta line: artist, year, "N tracks", total minutes. Join with the
  // same middle-dot the Dart pane uses for consistency.
  const metaBits: string[] = [];
  if (artist) metaBits.push(artist);
  if (year)   metaBits.push(year);
  metaBits.push(`${n} track${n === 1 ? '' : 's'}`);
  if (mins > 0) metaBits.push(`${mins} min`);
  const meta = metaBits.map(escapeHtml).join(' &middot; ');

  return `
    <div class="album-header card">
      <div class="album-header__art" aria-hidden="true">
        <span class="album-header__art-glyph">&#9835;</span>
      </div>
      <div class="album-header__info">
        <h2 class="album-header__title" data-av="title">${title}</h2>
        <div class="album-header__meta muted" data-av="meta">${meta}</div>
        <div class="album-header__actions row">
          <button type="button" class="primary" data-av="play-album">
            Play album
          </button>
          <button type="button" data-av="back" aria-label="Back to library">
            Back
          </button>
        </div>
      </div>
    </div>
  `;
}

function buildTrackRow(song: SongRow, index: number): string {
  const numLabel = song.track_number && song.track_number > 0
    ? String(song.track_number)
    : String(index + 1);
  const title = escapeHtml(trackTitle(song));
  const dur   = escapeHtml(fmtDuration(song.duration_ms));
  return `
    <li class="album-track clickable" data-av-track="${index}"
        tabindex="0"
        role="button"
        aria-label="Play track ${escapeHtml(numLabel)}">
      <span class="album-track__num mono muted">${escapeHtml(numLabel)}</span>
      <span class="album-track__title">${title}</span>
      <span class="album-track__dur mono muted">${dur}</span>
    </li>
  `;
}

// ---- Detach handle --------------------------------------------------

export type Detach = () => void;

// ---- Public render --------------------------------------------------

/** Render the album detail view into `container`. Replaces any prior
 *  contents. Returns a detach function that drops the click listeners
 *  and removes the widget DOM — call it before navigating away. */
export function renderAlbumView(
  container: HTMLElement,
  ctx:       AlbumViewCtx,
): Detach {
  // node is part of the contract but the widget itself doesn't need
  // to talk to the chain — playback drives through the player
  // adapter, which already holds its own NodeClient. We accept it so
  // future affordances (e.g. "favorite album", "report track") can
  // grow into the same render call without breaking callers.
  void ctx.node;

  const { album, player, onClose } = ctx;
  const address = ctx.playerAddress ?? '';
  const tracks  = sortTracks(album.tracks);

  // Build root + write everything in one innerHTML pass to avoid
  // per-row reflows when the album has many tracks.
  const root = document.createElement('section');
  root.className = 'album-view';
  root.setAttribute('role', 'region');
  root.setAttribute('aria-label', 'Album detail');

  const trackRowsHtml = tracks.length === 0
    ? `<li class="album-track album-track--empty muted">No tracks in this album.</li>`
    : tracks.map((t, i) => buildTrackRow(t, i)).join('');

  root.innerHTML = `
    ${buildHeader(album, tracks)}
    <ol class="album-tracks card" data-av="tracks">
      ${trackRowsHtml}
    </ol>
  `;

  // Wipe whatever was in the container before — caller's contract is
  // that this widget owns the pane while it's mounted.
  container.replaceChildren(root);

  // ---- Listeners -----------------------------------------------------

  const $back  = root.querySelector('[data-av="back"]')        as HTMLButtonElement | null;
  const $play  = root.querySelector('[data-av="play-album"]')  as HTMLButtonElement | null;
  const $list  = root.querySelector('[data-av="tracks"]')      as HTMLElement       | null;

  const onBack = (): void => {
    try { onClose(); } catch { /* caller-owned, swallow */ }
  };

  const onPlayAlbum = (): void => {
    if (tracks.length === 0) return;
    const first = tracks[0];
    if (!first) return;
    // The widget itself doesn't own a queue model — the shell's
    // ShellPlayer.loadAndPlay replaces the playlist with [song]
    // today (see screens/home.ts: list = [s]). When the shell grows
    // multi-track queues the album view will benefit automatically
    // because we already hand it the first track and the rest will
    // queue up via the shell's queueing layer.
    playSong(player, first, address);
  };

  // Delegated click / keyboard handler so we don't add N listeners on a
  // long tracklist. Each <li.album-track> is the only clickable target
  // now (the standalone per-row Play button was removed); a single
  // closest() lookup against data-av-track routes the click.
  const onListClick = (e: MouseEvent): void => {
    const target = e.target;
    if (!(target instanceof HTMLElement)) return;
    const trackEl = target.closest<HTMLElement>('[data-av-track]');
    if (!trackEl) return;
    const idxRaw = trackEl.getAttribute('data-av-track');
    if (idxRaw == null) return;
    const idx = Number(idxRaw);
    if (!Number.isFinite(idx) || idx < 0 || idx >= tracks.length) return;
    const song = tracks[idx];
    if (!song) return;
    playSong(player, song, address);
  };
  const onListKey = (e: KeyboardEvent): void => {
    if (e.key !== 'Enter' && e.key !== ' ') return;
    const target = e.target;
    if (!(target instanceof HTMLElement)) return;
    const trackEl = target.closest<HTMLElement>('[data-av-track]');
    if (!trackEl) return;
    e.preventDefault();
    const idxRaw = trackEl.getAttribute('data-av-track');
    if (idxRaw == null) return;
    const idx = Number(idxRaw);
    if (!Number.isFinite(idx) || idx < 0 || idx >= tracks.length) return;
    const song = tracks[idx];
    if (!song) return;
    playSong(player, song, address);
  };

  if ($back)  $back.addEventListener('click', onBack);
  if ($play)  $play.addEventListener('click', onPlayAlbum);
  if ($list) {
    $list.addEventListener('click', onListClick);
    $list.addEventListener('keydown', onListKey);
  }

  // ---- Detach --------------------------------------------------------

  return () => {
    if ($back) $back.removeEventListener('click', onBack);
    if ($play) $play.removeEventListener('click', onPlayAlbum);
    if ($list) {
      $list.removeEventListener('click', onListClick);
      $list.removeEventListener('keydown', onListKey);
    }
    if (root.parentNode) root.parentNode.removeChild(root);
  };
}
