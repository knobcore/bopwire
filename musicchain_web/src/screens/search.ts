// Browser equivalent of musicchain_player/lib/src/screens/search_screen.dart.
//
// Dart path is: TextField.onChanged -> Provider.search(q) -> NodeClient.searchSongs(q)
// -> RPC 'songs.search' {q} -> List<Song>. Here we keep the exact same RPC,
// just driven from a plain <input> with a typed-debounce + latest-id pattern
// instead of Provider/setState. Tapping a row hands off to the shared
// ctx.player + ctx.session so the search screen owns no playback state.
//
// Render contract:
//   renderSearch(container, ctx)
//     - Clears `container` and mounts the screen.
//     - Wires keyboard input, runs RPC, paints results / empty / no-results / error.
//     - Cancels any in-flight debounce on re-mount so callers swapping screens
//       can do so without leaking stale timers.

import type { NodeClient } from '../node_client.js';
import { RpcError } from '../node_client.js';
import { renderSongRow } from '../widgets/song_row.js';
import type { SongRow as VerbsSongRow } from '../verbs.js';

/** Minimal song shape returned by `songs.search`. Extra keys (genre,
 *  album, swarm_size, …) are tolerated and ignored — the home node may
 *  add fields without breaking the player.
 *
 *  Structurally compatible with `verbs.SongRow`; that's the type the
 *  shared row widget accepts. We re-export under the same name so
 *  existing callers (home.ts, tests) keep type-checking. */
export type SongRow = VerbsSongRow;

/** Subset of the app-wide context this screen needs. The full ctx wired
 *  by main.ts has more fields; we accept the narrow shape so the screen
 *  is trivially unit-testable with a fake. */
export interface SearchCtx {
  node: Pick<NodeClient, 'request'>;
  player: {
    /** Load the given song into the mini-player (does not start playback
     *  on its own — pairs with session.start). */
    load(song: SongRow): void | Promise<void>;
  };
  session: {
    /** Open a play session against the home node for [contentHash]. The
     *  returned promise resolves once the session.start RPC has acked;
     *  errors propagate so the screen can surface them inline. */
    start(contentHash: string): void | Promise<void>;
  };
  /** Optional sink for unexpected errors (the row click path). Defaults
   *  to console.error if absent. */
  onError?: (err: Error) => void;
}

/** Time the input has to settle before we fire `songs.search`. Matches
 *  the Dart screen's 300 ms — short enough to feel live, long enough that
 *  a fast typist on a slow home node doesn't queue ten in-flight RPCs. */
const DEBOUNCE_MS = 300;

/** Cap how long a single search reply may take before we treat it as a
 *  failure and let the user retry. NodeClient's default is 15 s; search
 *  should fail faster than chain RPCs so a flaky node doesn't freeze
 *  the input. */
const SEARCH_TIMEOUT_MS = 8_000;

function extractSongs(body: unknown): SongRow[] {
  // The home node has shipped two reply shapes for `songs.search` over the
  // life of the project: a bare array, and `{songs: [...]}`. Accept both
  // so the web player keeps working across node versions.
  if (Array.isArray(body)) return body as SongRow[];
  if (body && typeof body === 'object') {
    const maybe = (body as { songs?: unknown }).songs;
    if (Array.isArray(maybe)) return maybe as SongRow[];
  }
  return [];
}

/** Mount the search screen inside [container] using [ctx] for RPC + playback.
 *  Existing children of [container] are cleared first. */
export function renderSearch(container: HTMLElement, ctx: SearchCtx): void {
  // -- DOM skeleton ----------------------------------------------------
  container.replaceChildren();

  const root = document.createElement('div');
  root.className = 'main-pane col';
  root.setAttribute('data-screen', 'search');

  const inputRow = document.createElement('div');
  inputRow.className = 'row';

  const input = document.createElement('input');
  input.type = 'search';
  input.placeholder = 'Search songs…';
  input.autocomplete = 'off';
  // `autocorrect`/`autocapitalize`/`spellcheck` aren't on the TS lib type
  // for HTMLInputElement, but Safari/iOS WebKit honors them as attributes.
  // Without these the iOS keyboard helpfully capitalizes "Beethoven" into
  // a wrong-case query that misses chain rows.
  input.setAttribute('autocorrect', 'off');
  input.setAttribute('autocapitalize', 'off');
  input.spellcheck = false;
  input.className = 'grow';
  input.setAttribute('aria-label', 'Search songs');

  inputRow.appendChild(input);

  const status = document.createElement('div');
  status.className = 'muted';
  status.setAttribute('aria-live', 'polite');

  const results = document.createElement('div');
  results.className = 'col';
  results.setAttribute('role', 'list');

  root.append(inputRow, status, results);
  container.appendChild(root);

  // -- Debounce + latest-id state -------------------------------------
  let debounceTimer: ReturnType<typeof setTimeout> | null = null;
  // Monotonic id incremented on each search dispatch. Replies that don't
  // match `latestQueryId` at completion time are dropped — that's how we
  // shed out-of-order RPC results when the user types faster than the
  // network returns.
  let latestQueryId = 0;
  // Cache the last rendered query so a re-fire of the same debounce on
  // identical text doesn't repaint the list (avoids flicker if the user
  // tabs away and back without changing input).
  let lastRenderedQuery: string | null = null;

  function setStatus(text: string, kind: 'muted' | 'err' = 'muted'): void {
    status.textContent = text;
    status.className = kind === 'err' ? 'err' : 'muted';
  }

  function showEmptyState(): void {
    results.replaceChildren();
    setStatus('Search for a song above.');
  }

  function showNoResults(query: string): void {
    results.replaceChildren();
    setStatus(`No songs match "${query}".`);
  }

  function showError(message: string): void {
    results.replaceChildren();
    setStatus(`Search failed: ${message}`, 'err');
  }

  function showLoading(query: string): void {
    setStatus(`Searching for "${query}"…`);
  }

  function appendRow(song: SongRow, into: DocumentFragment | HTMLElement): void {
    // Delegate the visual + interaction shell to the shared widget; we
    // only own the playback orchestration callback that wires the row
    // back into the screen's player + session contracts.
    let playBtnRef: HTMLButtonElement | null = null;

    async function fire(): Promise<void> {
      if (!playBtnRef || playBtnRef.disabled) return;
      playBtnRef.disabled = true;
      playBtnRef.textContent = 'Loading…';
      try {
        await ctx.player.load(song);
        await ctx.session.start(song.content_hash);
      } catch (e) {
        const err = e instanceof Error ? e : new Error(String(e));
        if (ctx.onError) {
          try { ctx.onError(err); } catch (_) { /* swallow */ }
        } else {
          // eslint-disable-next-line no-console
          console.error('search: failed to start playback', err);
        }
        setStatus(`Couldn't play "${song.title ?? song.content_hash}": ${err.message}`, 'err');
      } finally {
        // Re-enable so the user can retry on transient errors. On
        // success the row is replaced when the user runs a new search.
        if (playBtnRef) {
          playBtnRef.disabled = false;
          playBtnRef.textContent = 'Play';
        }
      }
    }

    // The widget's container param is HTMLElement; DocumentFragment is a
    // Node but not an HTMLElement, so we route through a throwaway div
    // and then move the produced row into the caller's fragment. This
    // keeps renderResults' batched-fragment write pattern intact.
    const host = document.createElement('div');
    const handle = renderSongRow(host, {
      song,
      onPlay: () => { void fire(); },
    });
    playBtnRef = handle.playButton;
    // appendChild moves the node out of `host` (a Node only has one
    // parent at a time), so the throwaway div ends up empty and GCs.
    into.appendChild(handle.root);
  }

  function renderResults(songs: SongRow[]): void {
    results.replaceChildren();
    const frag = document.createDocumentFragment();
    for (const s of songs) {
      if (!s || typeof s.content_hash !== 'string' || s.content_hash.length === 0) continue;
      appendRow(s, frag);
    }
    results.appendChild(frag);
    setStatus(`${songs.length} result${songs.length === 1 ? '' : 's'}.`);
  }

  async function runSearch(query: string): Promise<void> {
    const myId = ++latestQueryId;
    showLoading(query);
    try {
      const body = await ctx.node.request<unknown>(
        'songs.search',
        { q: query },
        SEARCH_TIMEOUT_MS,
      );
      // Stale-reply guard: by the time this resolves the user may have
      // typed more characters and kicked off a newer search. Drop the
      // result silently — the newer search's reply will paint the list.
      if (myId !== latestQueryId) return;
      const songs = extractSongs(body);
      if (songs.length === 0) {
        showNoResults(query);
      } else {
        renderResults(songs);
      }
      lastRenderedQuery = query;
    } catch (e) {
      if (myId !== latestQueryId) return;
      const msg = e instanceof RpcError
        ? `${e.status}: ${e.message}`
        : (e instanceof Error ? e.message : String(e));
      showError(msg);
      lastRenderedQuery = null;
    }
  }

  function scheduleSearch(raw: string): void {
    const query = raw.trim();
    if (debounceTimer) {
      clearTimeout(debounceTimer);
      debounceTimer = null;
    }
    if (query.length === 0) {
      // Empty query: bump the id so any in-flight reply is dropped on
      // arrival, then show the idle state. Mirrors Dart's "return early
      // when q.isEmpty" but also resets the visible list.
      latestQueryId += 1;
      lastRenderedQuery = null;
      showEmptyState();
      return;
    }
    if (query === lastRenderedQuery) {
      // Same query, results already on screen — skip the round-trip.
      return;
    }
    debounceTimer = setTimeout(() => {
      debounceTimer = null;
      void runSearch(query);
    }, DEBOUNCE_MS);
  }

  input.addEventListener('input', () => scheduleSearch(input.value));
  input.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') {
      // Enter bypasses the debounce so power users get instant feedback.
      ev.preventDefault();
      if (debounceTimer) {
        clearTimeout(debounceTimer);
        debounceTimer = null;
      }
      const query = input.value.trim();
      if (query.length === 0) {
        showEmptyState();
        return;
      }
      void runSearch(query);
    }
  });

  // Initial paint + focus. Focus is what makes the search screen feel
  // like a search screen — without it the user has to click the input
  // first even though there's nothing else to interact with.
  showEmptyState();
  // setTimeout(0) avoids fighting whatever just took focus during
  // route swap (e.g. a clicked tab button). Without the yield the
  // browser snaps focus back to the tab after our focus() runs.
  setTimeout(() => { input.focus(); }, 0);
}
