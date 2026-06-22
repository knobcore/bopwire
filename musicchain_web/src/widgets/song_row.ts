// Reusable song-row widget. Browser equivalent of
// musicchain_player/lib/src/widgets/song_tile.dart — a single visual unit
// used wherever the player surfaces a row of song metadata (search
// results, library, future playlists / queue panels).
//
// The Dart SongTile is a Material ListTile with title/subtitle/trailing.
// On the web we mirror the same information density but render plain
// DOM so the screens can compose rows into <div role="list">s without
// dragging a framework in.
//
// Render contract:
//   renderSongRow(container, opts)
//     - Appends a single <div class="song-row"> to `container`.
//     - The same call drives both modes:
//         compact  — one-line: title · artist · duration, play button,
//                    optional overflow menu (Add to queue / Add to
//                    playlist).
//         expanded — two-line: title (with optional "N. " track-number
//                    prefix) over "artist · album · year".
//     - Returns a handle the caller can use to read the row element
//       back (e.g. for ARIA bookkeeping) without re-querying the DOM.
//
// All visible classes live in public/style.css; nothing here writes
// inline styles other than the disabled-button affordance which is a
// runtime state, not a one-off layout tweak.

import type { SongRow } from '../verbs';

// ---------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------

export type SongRowMode = 'compact' | 'expanded';

export interface RenderSongRowOpts {
  /** The song to render. Title / artist may be undefined; the widget
   *  falls back to "(Untitled)" / "Unknown artist" same as the Dart
   *  side does in SongTile. */
  song: SongRow;
  /** Required — clicking the play button (or the row body) fires this. */
  onPlay: () => void;
  /** Optional — when present the kebab overflow menu shows an
   *  "Add to queue" item that invokes this callback. */
  onAddToQueue?: () => void;
  /** Optional — when present the kebab overflow menu shows an
   *  "Add to playlist" item that invokes this callback. */
  onAddToPlaylist?: () => void;
  /** Visual density. Defaults to `compact`. */
  mode?: SongRowMode;
}

/** Handle returned by renderSongRow — callers can read the root element
 *  back without re-querying. The legacy `playButton` field is kept for
 *  source compatibility but is now always `null`; rows no longer carry
 *  their own play button (the entire row is the play target). */
export interface SongRowHandle {
  /** The root <div> element appended into `container`. */
  root: HTMLElement;
  /** Always null after the play-button removal. Kept for back-compat. */
  playButton: HTMLButtonElement | null;
}

// ---------------------------------------------------------------------
// Pure helpers (no DOM)
// ---------------------------------------------------------------------

/** Title fallback. Empty / whitespace-only titles render as "(Untitled)"
 *  — matches SongTile.title (Dart) and search.ts' previous inline string. */
function displayTitle(song: SongRow): string {
  return song.title?.trim() || '(Untitled)';
}

/** Artist fallback. Empty / whitespace-only artists render as
 *  "Unknown artist" — same string the previous inline renderers used. */
function displayArtist(song: SongRow): string {
  return song.artist?.trim() || 'Unknown artist';
}

/** Format duration_ms as m:ss. Returns '' for missing / zero / non-finite
 *  durations so callers can omit the separator. Same shape as Dart
 *  Song.durationFormatted, with a guarded zero. */
function formatDuration(ms: number | undefined): string {
  if (!ms || !Number.isFinite(ms) || ms <= 0) return '';
  const total = Math.floor(ms / 1000);
  const min = Math.floor(total / 60);
  const sec = total % 60;
  return `${min}:${String(sec).padStart(2, '0')}`;
}

/** Build the compact subtitle string "artist · m:ss". When duration is
 *  missing the separator and time are dropped so we don't leave a
 *  trailing " · " that looks like a missing field. */
function compactSubtitle(song: SongRow): string {
  const artist = displayArtist(song);
  const dur = formatDuration(song.duration_ms);
  return dur ? `${artist} · ${dur}` : artist;
}

/** Build the expanded subtitle string "artist · album · year · m:ss" —
 *  parts that are missing get dropped, joined with " · ". This is what
 *  goes on the second line of the expanded mode. */
function expandedSubtitle(song: SongRow): string {
  const parts: string[] = [displayArtist(song)];
  const album = song.album?.trim();
  if (album) parts.push(album);
  if (typeof song.year === 'number' && song.year > 0) {
    parts.push(String(song.year));
  }
  const dur = formatDuration(song.duration_ms);
  if (dur) parts.push(dur);
  return parts.join(' · ');
}

/** Prefix "N. " if track_number is a positive integer. Mirrors the
 *  Dart trackNumber field (0 == unknown). */
function trackPrefix(song: SongRow): string {
  if (typeof song.track_number !== 'number') return '';
  if (!Number.isFinite(song.track_number) || song.track_number <= 0) return '';
  return `${song.track_number}. `;
}

// ---------------------------------------------------------------------
// DOM builders
// ---------------------------------------------------------------------

/** Build the kebab overflow menu — a small button that pops a vertical
 *  list with the optional Add-to-queue / Add-to-playlist actions. The
 *  menu is dismissed on outside-click or after an item is selected. */
function buildOverflowMenu(opts: {
  onAddToQueue?: () => void;
  onAddToPlaylist?: () => void;
}): HTMLElement | null {
  const items: Array<{ label: string; cb: () => void }> = [];
  if (opts.onAddToQueue) {
    items.push({ label: 'Add to queue', cb: opts.onAddToQueue });
  }
  if (opts.onAddToPlaylist) {
    items.push({ label: 'Add to playlist', cb: opts.onAddToPlaylist });
  }
  if (items.length === 0) return null;

  const wrap = document.createElement('div');
  wrap.className = 'song-row__overflow';

  const trigger = document.createElement('button');
  trigger.type = 'button';
  trigger.className = 'song-row__overflow-btn';
  trigger.setAttribute('aria-haspopup', 'menu');
  trigger.setAttribute('aria-expanded', 'false');
  trigger.setAttribute('aria-label', 'More actions');
  // U+22EE VERTICAL ELLIPSIS — universally available, no icon font needed.
  trigger.textContent = '⋮';

  const menu = document.createElement('div');
  menu.className = 'song-row__overflow-menu';
  menu.setAttribute('role', 'menu');
  menu.hidden = true;

  for (const item of items) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'song-row__overflow-item';
    btn.setAttribute('role', 'menuitem');
    btn.textContent = item.label;
    btn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      closeMenu();
      try { item.cb(); } catch (_) { /* swallow — caller surfaces */ }
    });
    menu.appendChild(btn);
  }

  function closeMenu(): void {
    menu.hidden = true;
    trigger.setAttribute('aria-expanded', 'false');
    document.removeEventListener('click', onDocClick, true);
  }

  function openMenu(): void {
    menu.hidden = false;
    trigger.setAttribute('aria-expanded', 'true');
    // Capture-phase listener so we close before any inner handler runs
    // on the next click anywhere else in the document.
    document.addEventListener('click', onDocClick, true);
  }

  function onDocClick(ev: MouseEvent): void {
    if (ev.target instanceof Node && wrap.contains(ev.target)) return;
    closeMenu();
  }

  trigger.addEventListener('click', (ev) => {
    ev.stopPropagation();
    if (menu.hidden) openMenu();
    else closeMenu();
  });

  wrap.append(trigger, menu);
  return wrap;
}

// ---------------------------------------------------------------------
// Public renderer
// ---------------------------------------------------------------------

/** Render one song row into `container` per `opts`. Append-only — does
 *  not clear `container`, so callers can build a list with repeated
 *  calls inside a DocumentFragment. */
export function renderSongRow(
  container: HTMLElement,
  opts: RenderSongRowOpts,
): SongRowHandle {
  const mode: SongRowMode = opts.mode ?? 'compact';
  const { song } = opts;

  const row = document.createElement('div');
  row.className = `song-row song-row--${mode}`;
  row.setAttribute('role', 'listitem');

  // -- Meta (title + subtitle) ----------------------------------------
  const meta = document.createElement('div');
  meta.className = 'song-row__meta';

  const title = document.createElement('div');
  title.className = 'song-row__title';
  title.textContent = `${trackPrefix(song)}${displayTitle(song)}`;

  const sub = document.createElement('div');
  sub.className = 'song-row__sub muted';
  sub.textContent = mode === 'expanded'
    ? expandedSubtitle(song)
    : compactSubtitle(song);

  meta.append(title, sub);

  // -- Controls (optional kebab only) ---------------------------------
  //
  // The Play button used to live here. Per UX direction, library /
  // playlist / search rows are now click-to-play across the whole row
  // — the standalone Play button was redundant with the row click and
  // crowded mobile taps. The overflow kebab (Add to queue / Add to
  // playlist) stays because those are NOT the row's primary action.
  const controls = document.createElement('div');
  controls.className = 'song-row__controls';

  const overflow = buildOverflowMenu({
    ...(opts.onAddToQueue ? { onAddToQueue: opts.onAddToQueue } : {}),
    ...(opts.onAddToPlaylist ? { onAddToPlaylist: opts.onAddToPlaylist } : {}),
  });
  if (overflow) controls.appendChild(overflow);

  // -- Whole-row click drives playback --------------------------------
  row.classList.add('clickable');
  row.setAttribute('tabindex', '0');
  row.setAttribute('aria-label', `Play ${displayTitle(song)}`);
  const fire = (): void => {
    try { opts.onPlay(); } catch (_) { /* swallow — caller surfaces */ }
  };
  row.addEventListener('click', fire);
  row.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter' || ev.key === ' ') {
      ev.preventDefault();
      fire();
    }
  });

  // playButton is kept on the handle as `null` so existing callers that
  // peek the field (e.g. for an in-flight disabled toggle) can fall
  // through cleanly.
  row.append(meta, controls);
  container.appendChild(row);

  // We no longer build a play button — see the controls block above for
  // the rationale. The handle's `playButton` field is `null` so existing
  // call sites that read it can short-circuit.
  return { root: row, playButton: null };
}
