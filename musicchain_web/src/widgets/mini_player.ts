// Persistent bottom-of-shell mini-player. Mirrors the Dart equivalent at
// musicchain_player/lib/src/widgets/progress_bar.dart + the controls in
// the now-playing screen: it's the always-visible footer that shows the
// current song, a scrubbable position bar, and play/pause/skip buttons.
//
// The widget is event-driven. It subscribes to the Player's 'state' and
// 'position' events and re-renders the affected pieces in place — no
// global re-render on each position tick. The Player object is treated
// as an EventTarget-like emitter (.on / .off) with a small set of fields
// the widget reads on demand (currentSong, state, positionMs, durationMs,
// playlist, playlistIndex).
//
// Visibility rules mirror the Dart provider: idle / stopped + no current
// song => hidden. Anything else (loading, playing, paused) => visible.

import type { SongRow } from '../verbs';
import type { NodeClient } from '../node_client';

// -- Player surface (structural type) ---------------------------------
//
// Lives here so the widget compiles before the runtime PlayerController
// module exists. The real controller (see player.ts, separate task)
// implements at least this surface.

export type PlayerState =
  | 'idle'
  | 'loading'
  | 'playing'
  | 'paused'
  | 'stopped';

export type PlayerEvent =
  | 'state'
  | 'position';

export interface PlayerLike {
  state: PlayerState;
  currentSong: SongRow | null;
  positionMs: number;
  durationMs: number;
  playlist: SongRow[];
  playlistIndex: number;

  togglePlayPause(): void | Promise<void>;
  playNext(): void | Promise<void>;
  playPrev(): void | Promise<void>;
  seek(ms: number): void | Promise<void>;

  on(event: PlayerEvent, handler: () => void): void;
  off(event: PlayerEvent, handler: () => void): void;
}

export interface MiniPlayerCtx {
  player: PlayerLike;
  node: NodeClient;
  /** Optional: invoked when the user taps the mini-player's meta area
   *  (title / artist) to open the full-screen now-playing overlay. The
   *  transport buttons (play/pause/skip/seek) keep their own handlers
   *  and do NOT trigger this — those are meant for in-place control
   *  without expanding the surface. Absent => the meta area is not
   *  interactive (it shows the song info but doesn't respond to clicks). */
  onOpenNowPlaying?: () => void;
  /** Optional streaming-load progress. When set while
   *  `player.state === 'loading'`, the widget renders "Buffering N%"
   *  below the title so the user sees forward motion while the
   *  AudioBridge pulls chunks off the swarm. The host mutates this
   *  field as bytes arrive (via the AudioBridge's `onProgress(received,
   *  total)` hook) and then fans out a synthetic 'state' event on the
   *  underlying Player so the widget re-renders — there's no separate
   *  'progress' channel on `PlayerLike`.
   *
   *  Reset to `null` (NOT undefined — see exactOptionalPropertyTypes)
   *  when the load completes or fails, so the widget falls back to the
   *  bare "…" loading affordance once buffering enters its own state. */
  loadingProgress?: { received: number; total: number } | null;
}

/** Detach handle returned by renderMiniPlayer — caller invokes this when
 *  the shell is torn down to drop the player event subscriptions. */
export type Detach = () => void;

// -- Implementation ---------------------------------------------------

const SEEK_DEBOUNCE_MS = 180;

function formatTime(ms: number): string {
  if (!isFinite(ms) || ms < 0) ms = 0;
  const total = Math.floor(ms / 1000);
  const min = Math.floor(total / 60);
  const sec = total % 60;
  return `${min}:${sec.toString().padStart(2, '0')}`;
}

function songTitle(song: SongRow | null): string {
  if (!song) return '';
  const t = (song.title ?? '').trim();
  return t.length === 0 ? '(untitled)' : t;
}

function songArtist(song: SongRow | null): string {
  if (!song) return '';
  return (song.artist ?? '').trim();
}

function shouldShow(player: PlayerLike): boolean {
  // Dart: PlayerState.idle / .stopped with no current song => hidden.
  // A song in `currentSong` keeps the bar visible even after `stopped` so
  // the user can scrub/resume the last track without it disappearing on
  // the natural end-of-track event.
  if (player.currentSong) return true;
  return player.state !== 'idle' && player.state !== 'stopped';
}

function setHidden(root: HTMLElement, hidden: boolean): void {
  // Use a CSS hook + the hidden attribute so callers can override with
  // their own animation if they want; the attribute alone is enough for
  // the default "display: none" semantics.
  root.classList.toggle('mini-player--hidden', hidden);
  if (hidden) root.setAttribute('hidden', '');
  else root.removeAttribute('hidden');
}

function buildSkeleton(): HTMLElement {
  const root = document.createElement('div');
  root.className = 'mini-player';
  root.setAttribute('role', 'region');
  root.setAttribute('aria-label', 'Now playing');

  root.innerHTML = `
    <div
      class="mini-player__meta"
      data-mp="meta"
      role="button"
      tabindex="0"
      aria-label="Open now playing"
    >
      <div class="mini-player__title" data-mp="title"></div>
      <div class="mini-player__artist muted" data-mp="artist"></div>
      <div
        class="mini-player__buffering muted"
        data-mp="buffering"
        aria-live="polite"
        hidden
      ></div>
    </div>
    <div class="mini-player__progress">
      <span class="mini-player__time mono" data-mp="pos">0:00</span>
      <input
        type="range"
        class="mini-player__seek"
        data-mp="seek"
        min="0"
        max="0"
        step="1000"
        value="0"
        aria-label="Seek position"
      />
      <span class="mini-player__time mono" data-mp="dur">0:00</span>
    </div>
    <div class="mini-player__controls">
      <button type="button" class="mini-player__btn" data-mp="prev"   aria-label="Previous track">⏮</button>
      <button type="button" class="mini-player__btn primary" data-mp="play" aria-label="Play / Pause">▶</button>
      <button type="button" class="mini-player__btn" data-mp="next"   aria-label="Next track">⏭</button>
      <button type="button" class="mini-player__btn" data-mp="repeat"
              aria-label="Toggle repeat mode" aria-pressed="false"
              data-repeat-mode="off">↻</button>
    </div>
  `;
  return root;
}

/** Render the persistent mini-player into `container` and wire it up to
 *  `ctx.player`. Returns a detach function that unsubscribes from the
 *  player and removes the widget DOM. */
export function renderMiniPlayer(
  container: HTMLElement,
  ctx: MiniPlayerCtx,
): Detach {
  const { player, onOpenNowPlaying } = ctx;

  const root = buildSkeleton();
  container.appendChild(root);

  // Element refs.
  const $meta      = root.querySelector('[data-mp="meta"]')      as HTMLElement;
  const $title     = root.querySelector('[data-mp="title"]')     as HTMLElement;
  const $artist    = root.querySelector('[data-mp="artist"]')    as HTMLElement;
  const $buffering = root.querySelector('[data-mp="buffering"]') as HTMLElement;
  const $pos       = root.querySelector('[data-mp="pos"]')       as HTMLElement;
  const $dur       = root.querySelector('[data-mp="dur"]')       as HTMLElement;
  const $seek      = root.querySelector('[data-mp="seek"]')      as HTMLInputElement;
  const $prev      = root.querySelector('[data-mp="prev"]')      as HTMLButtonElement;
  const $play      = root.querySelector('[data-mp="play"]')      as HTMLButtonElement;
  const $next      = root.querySelector('[data-mp="next"]')      as HTMLButtonElement;
  const $repeat    = root.querySelector('[data-mp="repeat"]')    as HTMLButtonElement;

  // Tri-state repeat: 'off' → 'all' → 'one' → 'off'. Persisted to
  // localStorage so the user's preference survives reloads — matches the
  // way the Android player remembers playback toggles. The Queue
  // (src/queue.ts) reads the current mode from a static accessor so
  // playNext() can wrap or repeat as appropriate; we expose
  // window.__mcRepeatMode for now since shared state across
  // mini-player + queue requires either an import cycle or a singleton.
  type RepeatMode = 'off' | 'all' | 'one';
  const REPEAT_STORE_KEY = 'mc.player.repeat';
  function loadRepeat(): RepeatMode {
    try {
      const v = localStorage.getItem(REPEAT_STORE_KEY);
      if (v === 'all' || v === 'one' || v === 'off') return v;
    } catch { /* private mode, etc. */ }
    return 'off';
  }
  function saveRepeat(m: RepeatMode): void {
    try { localStorage.setItem(REPEAT_STORE_KEY, m); } catch { /* swallow */ }
    (globalThis as { __mcRepeatMode?: RepeatMode }).__mcRepeatMode = m;
  }
  function applyRepeat(m: RepeatMode): void {
    $repeat.setAttribute('data-repeat-mode', m);
    $repeat.setAttribute('aria-pressed', m === 'off' ? 'false' : 'true');
    // Glyph swaps so the user can see which mode is active without
    // colour-coding alone. Repeat-one shows a small "1" overlay glyph.
    $repeat.textContent = m === 'one' ? '🔂' : '🔁';
    if (m === 'off') {
      $repeat.classList.remove('mini-player__btn--active');
    } else {
      $repeat.classList.add('mini-player__btn--active');
    }
  }
  let repeatMode: RepeatMode = loadRepeat();
  saveRepeat(repeatMode);  // mirror to globalThis on construct
  applyRepeat(repeatMode);
  const onRepeatClick = (): void => {
    repeatMode =
      repeatMode === 'off' ? 'all' :
      repeatMode === 'all' ? 'one' : 'off';
    saveRepeat(repeatMode);
    applyRepeat(repeatMode);
  };
  $repeat.addEventListener('click', onRepeatClick);

  // Mark the meta area's interactive affordance based on whether the
  // host wired a handler. Without one, drop the button role + tabindex
  // so a keyboard user doesn't tab into a dead control.
  if (!onOpenNowPlaying) {
    $meta.removeAttribute('role');
    $meta.removeAttribute('tabindex');
    $meta.removeAttribute('aria-label');
    $meta.classList.add('mini-player__meta--static');
  } else {
    $meta.classList.add('mini-player__meta--tappable');
  }

  // While the user is dragging the slider, we mute incoming position
  // events so the thumb doesn't fight the touch. Matches the Dart
  // _dragMs guard in PlaybackProgressBar.
  let dragging = false;
  let seekTimer: ReturnType<typeof setTimeout> | null = null;
  let pendingSeekMs: number | null = null;

  function flushSeek(): void {
    if (pendingSeekMs == null) return;
    const ms = pendingSeekMs;
    pendingSeekMs = null;
    try {
      // seek may be async; we don't await — UI shouldn't block on RPC.
      void player.seek(ms);
    } catch (_) { /* swallow — controller surfaces errors */ }
  }

  function scheduleSeek(ms: number): void {
    pendingSeekMs = ms;
    if (seekTimer) clearTimeout(seekTimer);
    seekTimer = setTimeout(() => {
      seekTimer = null;
      flushSeek();
    }, SEEK_DEBOUNCE_MS);
  }

  function renderMeta(): void {
    const song = player.currentSong;
    $title.textContent  = songTitle(song);
    $artist.textContent = songArtist(song);

    // Duration only changes when the song changes.
    const dur = Math.max(0, player.durationMs | 0);
    $dur.textContent = formatTime(dur);
    $seek.max = String(dur);
    if (dur === 0) {
      $seek.disabled = true;
    } else {
      $seek.disabled = false;
    }
  }

  function renderBuffering(): void {
    // Only show "Buffering N%" while the player is actually in the
    // loading state AND the host has wired progress for the in-flight
    // load. The play button already shows the "…" affordance on its
    // own; this row gives the user a forward-motion signal so a slow
    // chunked fetch doesn't look like a hung UI.
    const prog = ctx.loadingProgress;
    const showBuffering =
      player.state === 'loading' &&
      prog != null &&
      prog.total > 0;
    if (!showBuffering) {
      $buffering.textContent = '';
      $buffering.setAttribute('hidden', '');
      return;
    }
    // prog is narrowed by the guard above; keep a local alias so TS
    // doesn't lose the non-null between the early-return and use.
    const p = prog as { received: number; total: number };
    const received = Math.max(0, p.received);
    const total    = Math.max(1, p.total);
    const pctRaw   = (received / total) * 100;
    const pct      = Math.max(0, Math.min(100, Math.floor(pctRaw)));
    $buffering.textContent = `Buffering ${pct}%`;
    $buffering.removeAttribute('hidden');
  }

  function renderState(): void {
    setHidden(root, !shouldShow(player));
    // Play/pause button label tracks state.
    switch (player.state) {
      case 'playing':
        $play.textContent = '⏸';
        $play.setAttribute('aria-label', 'Pause');
        $play.disabled = false;
        break;
      case 'loading':
        $play.textContent = '…';
        $play.setAttribute('aria-label', 'Loading');
        $play.disabled = true;
        break;
      case 'paused':
      case 'stopped':
      case 'idle':
      default:
        $play.textContent = '▶';
        $play.setAttribute('aria-label', 'Play');
        // No-op togglePlayPause from `stopped`/`idle` — match Dart provider
        // which only toggles between playing & paused. Disable so user
        // gets the affordance.
        $play.disabled = player.state === 'stopped' || player.state === 'idle';
        break;
    }
    // Skip controls only meaningful when there's more than one track in
    // the queue.
    const multi = player.playlist.length > 1;
    $prev.disabled = !multi;
    $next.disabled = !multi;

    // Song may have changed (e.g. on playNext) — refresh meta too.
    renderMeta();
    renderBuffering();
    renderPosition();
  }

  function renderPosition(): void {
    if (dragging) return;
    const dur = Math.max(0, player.durationMs | 0);
    const pos = Math.min(Math.max(0, player.positionMs | 0), dur);
    $pos.textContent = formatTime(pos);
    // Avoid writing the value back if the user is actively dragging — but
    // we already early-return above. Setting .value to the same string is
    // a no-op visually so no flicker either way.
    $seek.value = String(pos);
  }

  // -- DOM listeners --------------------------------------------------

  const onSeekInput = (): void => {
    dragging = true;
    const ms = Number($seek.value);
    if (!Number.isFinite(ms)) return;
    // Show the dragged time immediately so the user sees feedback.
    $pos.textContent = formatTime(ms);
    scheduleSeek(ms);
  };

  const onSeekCommit = (): void => {
    // change fires on release for mouse + keyboard interaction.
    dragging = false;
    // Make sure the final position is flushed even if the user releases
    // before the debounce timer fires.
    if (seekTimer) {
      clearTimeout(seekTimer);
      seekTimer = null;
    }
    if (pendingSeekMs != null) flushSeek();
  };

  const onPlayClick = (): void => {
    void player.togglePlayPause();
  };

  const onNextClick = (): void => {
    if (player.playlist.length <= 1) return;
    void player.playNext();
  };

  const onPrevClick = (): void => {
    if (player.playlist.length <= 1) return;
    void player.playPrev();
  };

  // Meta-area tap → open the full-screen now-playing overlay. We only
  // fire when the host wired the callback; otherwise the meta area
  // has no role attribute (see above) and these listeners no-op.
  const onMetaClick = (): void => {
    if (!onOpenNowPlaying) return;
    // Don't open the overlay when there's nothing playing — the meta
    // area shows empty strings in that state and an empty overlay is
    // worse than a no-op tap. shouldShow() already hides the whole
    // mini-player in that case, but a clean defensive check costs
    // nothing here.
    if (!player.currentSong) return;
    onOpenNowPlaying();
  };
  const onMetaKeyDown = (ev: KeyboardEvent): void => {
    if (!onOpenNowPlaying) return;
    if (ev.key === 'Enter' || ev.key === ' ') {
      ev.preventDefault();
      onMetaClick();
    }
  };

  $seek.addEventListener('input', onSeekInput);
  $seek.addEventListener('change', onSeekCommit);
  $play.addEventListener('click', onPlayClick);
  $next.addEventListener('click', onNextClick);
  $prev.addEventListener('click', onPrevClick);
  $meta.addEventListener('click', onMetaClick);
  $meta.addEventListener('keydown', onMetaKeyDown);

  // -- Player listeners -----------------------------------------------

  // 'state' covers state changes AND song-change (currentSong/duration
  // can flip in the same notify), so we re-render the whole skeleton's
  // dynamic bits. 'position' is high-frequency; only touch the slider.
  const onStateChange = (): void => renderState();
  const onPositionChange = (): void => renderPosition();

  player.on('state', onStateChange);
  player.on('position', onPositionChange);

  // Initial paint.
  renderState();

  // -- Detach ---------------------------------------------------------

  return () => {
    player.off('state', onStateChange);
    player.off('position', onPositionChange);
    $seek.removeEventListener('input', onSeekInput);
    $seek.removeEventListener('change', onSeekCommit);
    $play.removeEventListener('click', onPlayClick);
    $next.removeEventListener('click', onNextClick);
    $prev.removeEventListener('click', onPrevClick);
    $repeat.removeEventListener('click', onRepeatClick);
    $meta.removeEventListener('click', onMetaClick);
    $meta.removeEventListener('keydown', onMetaKeyDown);
    if (seekTimer) {
      clearTimeout(seekTimer);
      seekTimer = null;
    }
    if (root.parentNode) root.parentNode.removeChild(root);
  };
}
