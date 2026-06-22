// Playlist detail view widget.
//
// Renders the contents of a single playlist:
//   - Header with name + back arrow + "Play playlist" primary action.
//   - Reorderable list of tracks (HTML5 drag-and-drop).
//   - Per-row "Remove from playlist" button.
//
// The widget itself is stateless w.r.t. persistence: it accepts a
// snapshot of the playlist on mount and emits onTrackRemoved when a row
// is dropped. Reorder is handled the same way — the host (playlists.ts
// screen) is responsible for committing the new order to localStorage
// via setPlaylistTracks. That split lets a future chain-backed playlist
// surface plug in without rewriting the view.
//
// The Dart equivalent doesn't exist yet — the Android player ships a
// queue UI in the now-playing screen but no standalone playlist view.
// Styling references the Discover/local-library card+row idiom from
// local_library_screen.dart for visual consistency with that screen.
//
// "Play playlist" hands the track list to the host's player wrapper one
// song at a time (current Player only exposes `load(source)`). When the
// shell player grows a real multi-track queue API we widen the contract
// here in a follow-up — the row click still works for cherry-pick play
// in the meantime.

import type { NodeClient } from '../node_client';
import type { SongRow } from '../verbs';

// ---------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------

/** The player surface this view drives. Matches the shape the home shell
 *  hands the library screen so we can reuse the same wrapper. */
export interface PlaylistViewPlayer {
  load(source: string | Blob | SongRow): void | Promise<void>;
}

/** Subset of NodeClient — we don't currently issue RPCs from this view
 *  but accept the field so a future "Play playlist" implementation that
 *  pre-warms session.start has a hook to grow into. */
export interface PlaylistViewNode {
  request<T = unknown>(
    type: string,
    body: unknown,
    timeoutMs?: number,
  ): Promise<T>;
}

/** Minimal snapshot the view needs. Not the full Playlist (no createdAt)
 *  so the host can pass it through from either listPlaylists() or a
 *  future remote source without converting. */
export interface PlaylistSnapshot {
  id: string;
  name: string;
  tracks: SongRow[];
}

export interface PlaylistViewCtx {
  node: PlaylistViewNode | NodeClient;
  player: PlaylistViewPlayer;
  playlist: PlaylistSnapshot;
  /** Called after the view has removed a row from its local working
   *  copy. The index is into the working list AT THE TIME OF THE CALL,
   *  which is the same index the host should pass to setPlaylistTracks
   *  after dropping that slot. */
  onTrackRemoved: (idx: number) => void;
  /** Called when the user hits Back, or the playlist becomes empty and
   *  they bail out. The host pops back to the list view. */
  onClose: () => void;
  /** Called whenever the working order changes (drag-and-drop). The host
   *  should persist the new array. Optional so a read-only embedding
   *  can opt out of reorder side-effects. */
  onTracksReordered?: (tracks: SongRow[]) => void;
}

// ---------------------------------------------------------------------
// DOM helpers
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
  return song.title?.trim() || '(Untitled)';
}

function songDisplayArtist(song: SongRow): string {
  return song.artist?.trim() || 'Unknown artist';
}

// ---------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------

/** Mount the playlist detail view into `container`. Replaces existing
 *  children. */
export function renderPlaylistView(
  container: HTMLElement,
  ctx: PlaylistViewCtx,
): void {
  container.replaceChildren();

  // Working copy of the track list. Drag-and-drop reorders, Remove
  // splices. We rebuild the rows from this array on every mutation so
  // indices stay in sync with what the user sees — premature
  // mini-mutations (just swapping two DOM nodes) leave the dataset-index
  // attributes stale and break the next drag.
  let tracks: SongRow[] = ctx.playlist.tracks.slice();

  const root = el('div', 'main-pane col');
  root.setAttribute('data-screen', 'playlist-view');

  // -- Header --------------------------------------------------------
  // Two rows: back+title on top, Play playlist primary action below.
  // Matches the visual rhythm of the library screen's section headers.

  const header = el('section', 'card col');
  header.style.marginBottom = '16px';

  const topRow = el('div', 'row');
  topRow.style.justifyContent = 'space-between';

  const titleGroup = el('div', 'row');
  titleGroup.style.gap = '10px';
  const backBtn = el('button', undefined, '← Back');
  backBtn.type = 'button';
  backBtn.setAttribute('aria-label', 'Back to playlists');
  backBtn.addEventListener('click', () => {
    try { ctx.onClose(); } catch { /* swallow — host owns nav */ }
  });
  const title = el('h2', undefined, ctx.playlist.name);
  title.style.margin = '0';
  titleGroup.append(backBtn, title);

  const countLabel = el('div', 'muted');
  countLabel.style.fontSize = '12px';

  topRow.append(titleGroup, countLabel);

  const actionRow = el('div', 'row');
  actionRow.style.marginTop = '8px';
  const playAllBtn = el('button', 'primary', 'Play playlist');
  playAllBtn.type = 'button';
  actionRow.append(playAllBtn);

  header.append(topRow, actionRow);

  // -- Body ----------------------------------------------------------

  const list = el('section', 'card col');
  list.setAttribute('role', 'list');

  root.append(header, list);
  container.appendChild(root);

  // -- Renderers -----------------------------------------------------

  function updateCount(): void {
    const n = tracks.length;
    countLabel.textContent = n === 1 ? '1 track' : `${n} tracks`;
    playAllBtn.disabled = n === 0;
  }

  /** Drag-and-drop bookkeeping. The dragstart handler stashes the
   *  source row index; dragover on a target row computes whether to
   *  drop above or below based on the pointer's Y position relative to
   *  the row midpoint, and dragend / drop applies the move. We re-render
   *  on drop so dataset-index attributes and any "drop indicator"
   *  styling resets cleanly. */
  let dragSrcIdx: number | null = null;

  function applyMove(from: number, to: number): void {
    if (from === to) return;
    if (from < 0 || from >= tracks.length) return;
    if (to < 0 || to > tracks.length) return;
    const next = tracks.slice();
    const [moved] = next.splice(from, 1);
    // Adjust target if removing the source shifted indices.
    const dest = from < to ? to - 1 : to;
    next.splice(dest, 0, moved);
    tracks = next;
    if (ctx.onTracksReordered) {
      try { ctx.onTracksReordered(tracks.slice()); } catch { /* swallow */ }
    }
    renderRows();
  }

  function renderRow(track: SongRow, idx: number): HTMLElement {
    const row = el('div', 'card row');
    row.setAttribute('role', 'listitem');
    row.setAttribute('data-idx', String(idx));
    row.draggable = true;
    row.style.cssText =
      'justify-content: space-between; padding: 10px 14px; gap: 10px;' +
      'cursor: grab; user-select: none;';

    // Drag handle hint (just a glyph — the whole row is draggable, but
    // the visual cue mirrors the standard list affordance).
    const grip = el('div', 'muted');
    grip.textContent = '☰';
    grip.style.cssText = 'font-size: 16px; cursor: grab; padding: 0 4px;';
    grip.setAttribute('aria-hidden', 'true');

    const meta = el('div', 'col grow');
    meta.style.gap = '2px';
    const titleEl = el('div', undefined, songDisplayTitle(track));
    titleEl.style.fontWeight = '600';
    const sub = el('div', 'muted');
    const dur = formatTime(track.duration_ms);
    sub.textContent = dur
      ? `${songDisplayArtist(track)} · ${dur}`
      : songDisplayArtist(track);
    meta.append(titleEl, sub);

    // Per-row Play button removed: the whole row is the play target.
    // Only the Remove (and drag-handle) controls keep their buttons.
    const controls = el('div', 'row');
    const removeBtn = el('button', undefined, 'Remove');
    removeBtn.type = 'button';
    removeBtn.setAttribute(
      'aria-label',
      `Remove ${songDisplayTitle(track)} from playlist`,
    );
    controls.append(removeBtn);

    // -- Whole-row play target ---
    row.classList.add('clickable');
    row.setAttribute('role', 'button');
    row.setAttribute('tabindex', '0');
    row.setAttribute('aria-label', `Play ${songDisplayTitle(track)}`);

    let inFlight = false;
    async function playThis(): Promise<void> {
      if (inFlight) return;
      inFlight = true;
      row.classList.add('loading');
      try {
        await ctx.player.load(track);
      } catch (err) {
        // eslint-disable-next-line no-console
        console.warn('playlist_view: play failed', err);
        sub.textContent = `Couldn't play: ${err instanceof Error ? err.message : String(err)}`;
        sub.className = 'err';
      } finally {
        inFlight = false;
        row.classList.remove('loading');
      }
    }
    row.addEventListener('click', (ev) => {
      // Skip when the click originated inside the Remove button or the
      // drag handle — those have their own meanings.
      const t = ev.target;
      if (t instanceof HTMLElement && t.closest('button')) return;
      void playThis();
    });
    row.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter' || ev.key === ' ') {
        ev.preventDefault();
        void playThis();
      }
    });

    // -- Row remove ---
    removeBtn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      const nextTracks = tracks.slice();
      nextTracks.splice(idx, 1);
      tracks = nextTracks;
      try { ctx.onTrackRemoved(idx); } catch { /* swallow */ }
      renderRows();
    });

    // -- Drag-and-drop ---
    row.addEventListener('dragstart', (ev) => {
      dragSrcIdx = idx;
      row.style.opacity = '0.5';
      // Required for Firefox to actually start the drag.
      if (ev.dataTransfer) {
        ev.dataTransfer.effectAllowed = 'move';
        try { ev.dataTransfer.setData('text/plain', String(idx)); } catch { /* swallow */ }
      }
    });
    row.addEventListener('dragend', () => {
      dragSrcIdx = null;
      row.style.opacity = '1';
      // Clear any lingering indicator borders the dragover handler set.
      for (const child of list.children) {
        (child as HTMLElement).style.borderTop = '';
        (child as HTMLElement).style.borderBottom = '';
      }
    });
    row.addEventListener('dragover', (ev) => {
      if (dragSrcIdx === null) return;
      ev.preventDefault();
      if (ev.dataTransfer) ev.dataTransfer.dropEffect = 'move';
      const rect = row.getBoundingClientRect();
      const above = ev.clientY < rect.top + rect.height / 2;
      row.style.borderTop = above ? '2px solid var(--accent)' : '';
      row.style.borderBottom = above ? '' : '2px solid var(--accent)';
    });
    row.addEventListener('dragleave', () => {
      row.style.borderTop = '';
      row.style.borderBottom = '';
    });
    row.addEventListener('drop', (ev) => {
      if (dragSrcIdx === null) return;
      ev.preventDefault();
      const rect = row.getBoundingClientRect();
      const above = ev.clientY < rect.top + rect.height / 2;
      // Drop above row idx -> dest = idx; below -> dest = idx + 1.
      const dest = above ? idx : idx + 1;
      const from = dragSrcIdx;
      dragSrcIdx = null;
      applyMove(from, dest);
    });

    row.append(grip, meta, controls);
    return row;
  }

  function renderRows(): void {
    list.replaceChildren();
    if (tracks.length === 0) {
      const empty = el(
        'div',
        'muted',
        'No tracks in this playlist yet. Add some from the library or search.',
      );
      list.appendChild(empty);
      updateCount();
      return;
    }
    const frag = document.createDocumentFragment();
    tracks.forEach((track, idx) => {
      frag.appendChild(renderRow(track, idx));
    });
    list.appendChild(frag);
    updateCount();
  }

  // -- Play all -----------------------------------------------------
  //
  // The shell player today only exposes `load(source)` — no real queue
  // API. To still give the user something useful on this button, we
  // load the first track and log a TODO; the row-level Play covers the
  // cherry-pick case. When the shell grows playNext / queue setters we
  // wire those here too.

  playAllBtn.addEventListener('click', () => {
    if (tracks.length === 0) return;
    void (async () => {
      try {
        await ctx.player.load(tracks[0]);
      } catch (err) {
        // eslint-disable-next-line no-console
        console.warn('playlist_view: play-all failed', err);
      }
    })();
  });

  // -- Initial paint ------------------------------------------------
  renderRows();
}
