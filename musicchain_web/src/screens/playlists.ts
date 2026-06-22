// Playlists screen.
//
// Two modes share one container:
//   1. List mode — every playlist the user has, one card per row.
//      Tap to drill in; primary "Create playlist" button at the top
//      opens a modal with a text input.
//   2. Detail mode — delegates to renderPlaylistView (widget). Returning
//      from detail rerenders the list so any reorder / remove / delete
//      is reflected immediately.
//
// Storage is local-only for now (see playlists.ts). The "future work"
// chain-backed flow would replace the listPlaylists / mutate calls with
// async RPCs and surface a sync indicator — the screen contract here is
// already shape-compatible with that.
//
// Styling follows the local-library / Discover idiom from the Android
// player (musicchain_player/lib/src/screens/local_library_screen.dart):
// a top section card holding the header + create button, then a list of
// playlist cards below. Each card shows the name, a small subtitle with
// the track count, and an inline Delete affordance (with confirm). Tap
// anywhere on the card to drill in.

import type { NodeClient } from '../node_client';
import type { SongRow } from '../verbs';
import {
  listPlaylists,
  createPlaylist,
  getPlaylist,
  setPlaylistTracks,
  deletePlaylist,
  type Playlist,
} from '../playlists';
import { renderPlaylistView } from '../widgets/playlist_view';

// ---------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------

/** Player surface — same shape the library / playlist_view widgets use
 *  so a single shell-level wrapper drives all three screens. */
export interface PlaylistsPlayer {
  load(source: string | Blob | SongRow): void | Promise<void>;
}

export interface PlaylistsNode {
  request<T = unknown>(
    type: string,
    body: unknown,
    timeoutMs?: number,
  ): Promise<T>;
}

export interface PlaylistsCtx {
  node: PlaylistsNode | NodeClient;
  player: PlaylistsPlayer;
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

// ---------------------------------------------------------------------
// Create-playlist modal
// ---------------------------------------------------------------------

/** Show a modal that prompts for a playlist name. Resolves to the
 *  trimmed name on confirm, or null on cancel / Esc. The modal is a
 *  position:fixed overlay parented to <body> so it sits on top of the
 *  whole shell — same approach as the now-playing screen in the Dart
 *  player which routes through a full-screen MaterialPageRoute. */
function promptForPlaylistName(): Promise<string | null> {
  return new Promise((resolve) => {
    const overlay = el('div');
    overlay.style.cssText =
      'position: fixed; inset: 0; background: rgba(0, 0, 0, 0.6);' +
      'display: grid; place-items: center; z-index: 1000;';

    const dialog = el('div', 'card col');
    dialog.style.cssText =
      'min-width: 280px; max-width: 90vw; padding: 20px; gap: 12px;';

    const title = el('h3', undefined, 'New playlist');
    title.style.margin = '0';

    const label = el('label', 'muted', 'Name');
    label.style.fontSize = '12px';

    const input = el('input');
    input.type = 'text';
    input.placeholder = 'My playlist';
    input.maxLength = 120;
    // Auto-focus so a one-keystroke flow works: open modal -> type -> Enter.
    setTimeout(() => input.focus(), 0);

    const errLine = el('div', 'err');
    errLine.style.cssText = 'font-size: 12px; min-height: 1em;';

    const buttons = el('div', 'row');
    buttons.style.justifyContent = 'flex-end';
    const cancelBtn = el('button', undefined, 'Cancel');
    cancelBtn.type = 'button';
    const createBtn = el('button', 'primary', 'Create');
    createBtn.type = 'button';
    buttons.append(cancelBtn, createBtn);

    dialog.append(title, label, input, errLine, buttons);
    overlay.append(dialog);
    document.body.appendChild(overlay);

    let resolved = false;
    function finish(result: string | null): void {
      if (resolved) return;
      resolved = true;
      document.removeEventListener('keydown', onKey);
      if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
      resolve(result);
    }
    function tryCreate(): void {
      const name = input.value.trim();
      if (name.length === 0) {
        errLine.textContent = 'Name cannot be empty.';
        return;
      }
      finish(name);
    }
    function onKey(ev: KeyboardEvent): void {
      if (ev.key === 'Escape') {
        ev.preventDefault();
        finish(null);
      } else if (ev.key === 'Enter' && document.activeElement === input) {
        ev.preventDefault();
        tryCreate();
      }
    }

    cancelBtn.addEventListener('click', () => finish(null));
    createBtn.addEventListener('click', tryCreate);
    // Click on the dark backdrop dismisses — but a click that started
    // inside the dialog and ended on the overlay (e.g. selecting text)
    // shouldn't close. We use mousedown on the overlay itself to filter.
    overlay.addEventListener('mousedown', (ev) => {
      if (ev.target === overlay) finish(null);
    });
    document.addEventListener('keydown', onKey);
  });
}

// ---------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------

/** Mount the playlists screen into `container`. Replaces existing
 *  children. */
export function renderPlaylists(
  container: HTMLElement,
  ctx: PlaylistsCtx,
): void {
  container.replaceChildren();

  // Two-level navigation inside this single screen: we either show the
  // list or the detail. drillId is the currently-open playlist id, or
  // null for list mode.
  let drillId: string | null = null;

  function render(): void {
    container.replaceChildren();
    if (drillId === null) {
      renderList();
    } else {
      renderDetail(drillId);
    }
  }

  // -- List mode ----------------------------------------------------

  function renderList(): void {
    const root = el('div', 'main-pane col');
    root.setAttribute('data-screen', 'playlists');

    // Header card with the Create button.
    const headerCard = el('section', 'card col');
    headerCard.style.marginBottom = '16px';
    const headerRow = el('div', 'row');
    headerRow.style.justifyContent = 'space-between';
    const headerTitle = el('h2', undefined, 'Playlists');
    headerTitle.style.margin = '0';
    const createBtn = el('button', 'primary', 'New playlist');
    createBtn.type = 'button';
    headerRow.append(headerTitle, createBtn);
    const hint = el('div', 'muted');
    hint.style.fontSize = '12px';
    hint.textContent =
      'Playlists are stored in this browser only. Chain-synced playlists are coming.';
    headerCard.append(headerRow, hint);

    // The actual list.
    const listCard = el('section', 'card col');
    const playlists = listPlaylists();

    if (playlists.length === 0) {
      const empty = el(
        'div',
        'muted',
        'No playlists yet. Hit "New playlist" above to create one.',
      );
      listCard.appendChild(empty);
    } else {
      const frag = document.createDocumentFragment();
      for (const pl of playlists) {
        frag.appendChild(renderListRow(pl));
      }
      listCard.appendChild(frag);
    }

    root.append(headerCard, listCard);
    container.appendChild(root);

    createBtn.addEventListener('click', () => {
      void (async () => {
        const name = await promptForPlaylistName();
        if (name === null) return;
        try {
          const created = createPlaylist(name);
          // Drill into the new playlist immediately — matches the
          // "create and add tracks" flow the user almost certainly
          // came in for.
          drillId = created.id;
          render();
        } catch (err) {
          // eslint-disable-next-line no-console
          console.warn('playlists: createPlaylist failed', err);
        }
      })();
    });
  }

  function renderListRow(pl: Playlist): HTMLElement {
    const row = el('div', 'card row');
    row.setAttribute('role', 'listitem');
    row.style.cssText =
      'justify-content: space-between; padding: 10px 14px; cursor: pointer;';

    const meta = el('div', 'col grow');
    meta.style.gap = '2px';
    const name = el('div', undefined, pl.name);
    name.style.fontWeight = '600';
    const sub = el('div', 'muted');
    sub.textContent =
      pl.tracks.length === 1
        ? '1 track'
        : `${pl.tracks.length} tracks`;
    meta.append(name, sub);

    const controls = el('div', 'row');
    const openBtn = el('button', 'primary', 'Open');
    openBtn.type = 'button';
    const deleteBtn = el('button', undefined, 'Delete');
    deleteBtn.type = 'button';
    deleteBtn.setAttribute('aria-label', `Delete playlist ${pl.name}`);
    controls.append(openBtn, deleteBtn);

    function open(): void {
      drillId = pl.id;
      render();
    }
    openBtn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      open();
    });
    row.addEventListener('click', open);

    deleteBtn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      // Cheap confirm — a modal would be overkill for the in-list
      // delete; the user can still recover by re-creating since the
      // playlist UI doesn't carry any unique chain state yet.
      const ok = window.confirm(
        `Delete playlist "${pl.name}"? This can't be undone.`,
      );
      if (!ok) return;
      deletePlaylist(pl.id);
      render();
    });

    row.append(meta, controls);
    return row;
  }

  // -- Detail mode --------------------------------------------------

  function renderDetail(id: string): void {
    const pl = getPlaylist(id);
    if (!pl) {
      // Race: playlist was deleted (e.g. via another tab) between drill
      // and render. Bounce back to list rather than render an empty
      // detail.
      drillId = null;
      render();
      return;
    }
    renderPlaylistView(container, {
      node:   ctx.node,
      player: ctx.player,
      playlist: { id: pl.id, name: pl.name, tracks: pl.tracks },
      onTrackRemoved: (idx) => {
        // The view's working copy already dropped the slot — commit the
        // shorter list to storage. Re-read first so concurrent mutations
        // (none today, but defensive) don't get lost.
        const fresh = getPlaylist(id);
        if (!fresh) return;
        const next = fresh.tracks.slice();
        if (idx >= 0 && idx < next.length) {
          next.splice(idx, 1);
          setPlaylistTracks(id, next);
        }
      },
      onTracksReordered: (tracks) => {
        setPlaylistTracks(id, tracks);
      },
      onClose: () => {
        drillId = null;
        render();
      },
    });
  }

  // -- Initial paint ------------------------------------------------
  render();
}
