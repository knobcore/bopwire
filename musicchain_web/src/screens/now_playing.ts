// Full-screen now-playing overlay. Web equivalent of the Dart
// `_MiniPlayer` expanded into a dedicated route — the Flutter side
// (musicchain_player/lib/src/screens/home_screen.dart) keeps the
// mini-player as the only playback surface, but the web build wants
// the room to show big art + a long scrubber + the upcoming queue.
//
// The reference for the controls + scrubber semantics is the Dart
// `PlaybackProgressBar` widget at
// musicchain_player/lib/src/widgets/progress_bar.dart: while the user
// is dragging the seek slider, incoming `position` events are ignored
// so the thumb doesn't fight the touch.
//
// Render contract:
//   renderNowPlaying(container, ctx)
//     - Appends a `.now-playing` modal element into `container` (NOT
//       replaceChildren — the host shell stays mounted underneath so
//       the persistent mini-player and balance header survive the
//       overlay). The caller chooses where in the DOM to mount us;
//       most callers will use `document.body` so the overlay covers
//       the whole viewport.
//     - Wires keyboard (Escape) + swipe-down + close-button to
//       ctx.onClose.
//     - Subscribes to the same `state` + `position` events the
//       mini-player uses, and re-renders the affected pieces in
//       place. No global re-render on each position tick.
//     - Returns a detach function. The caller invokes it from
//       onClose (or on shell teardown) to drop event subscriptions
//       and remove the overlay DOM.

import type { SongRow } from '../verbs';
import type { NodeClient } from '../node_client';
import type { PlayerLike } from '../widgets/mini_player';

/** Context the host passes to `renderNowPlaying`. Mirrors the
 *  `MiniPlayerCtx` from widgets/mini_player.ts plus the close hook
 *  — the overlay doesn't own its own dismissal, that's the parent
 *  shell's job (so back-button / route changes / sign-out can all
 *  drive it through the same code path). */
export interface NowPlayingCtx {
  player: PlayerLike;
  node:   NodeClient;
  /** Invoked when the user dismisses the overlay (close button,
   *  Escape key, swipe-down). The caller is responsible for actually
   *  unmounting the overlay via the detach function returned by
   *  `renderNowPlaying`. */
  onClose: () => void;
}

/** Detach handle returned by `renderNowPlaying`. Idempotent. */
export type NowPlayingDetach = () => void;

// -- Tunables ---------------------------------------------------------

/** Same debounce the mini-player uses so dragging the long bar doesn't
 *  spam the player with seek RPCs (the underlying audio element will
 *  happily accept dozens of currentTime writes per second, but if a
 *  future controller pipes seeks over RPC that becomes expensive). */
const SEEK_DEBOUNCE_MS = 180;

/** Vertical distance the user must drag the overlay before we treat
 *  the gesture as a dismissal. Below the threshold the overlay
 *  springs back (touchend without close). 120 px matches the
 *  Material spec for sheet dismissal and feels right on a phone in
 *  the browser at typical pixel ratios. */
const SWIPE_DISMISS_PX = 120;

// -- Helpers ----------------------------------------------------------

function formatTime(ms: number): string {
  if (!isFinite(ms) || ms < 0) ms = 0;
  const total = Math.floor(ms / 1000);
  const min = Math.floor(total / 60);
  const sec = total % 60;
  return `${min}:${sec.toString().padStart(2, '0')}`;
}

function songTitle(song: SongRow | null): string {
  if (!song) return 'Nothing playing';
  const t = (song.title ?? '').trim();
  return t.length === 0 ? '(untitled)' : t;
}

function songArtist(song: SongRow | null): string {
  if (!song) return '';
  return (song.artist ?? '').trim();
}

function songAlbum(song: SongRow | null): string {
  if (!song) return '';
  return (song.album ?? '').trim();
}

/** Pick a stable single-character monogram for the big art placeholder.
 *  We use the first character of the title; falls back to the artist's
 *  first character, then '?'. Matches the convention the Dart side
 *  uses for missing-art placeholders. */
function artGlyph(song: SongRow | null): string {
  if (!song) return '?';
  const t = (song.title ?? '').trim();
  if (t.length > 0) return t.charAt(0).toUpperCase();
  const a = (song.artist ?? '').trim();
  if (a.length > 0) return a.charAt(0).toUpperCase();
  return '?';
}

function escapeHtml(s: string): string {
  // Small inline escape so we can safely set queue row labels via
  // textContent without pulling in a DOM helper module. We never
  // assign user-provided strings to innerHTML — but two of the
  // queue helpers below use `textContent` so this isn't strictly
  // necessary; kept for the doc-string hint to future maintainers.
  return s.replace(/&/g, '&amp;')
          .replace(/</g, '&lt;')
          .replace(/>/g, '&gt;');
}

// -- DOM skeleton -----------------------------------------------------

function buildSkeleton(): HTMLElement {
  const root = document.createElement('div');
  root.className = 'now-playing';
  root.setAttribute('role', 'dialog');
  root.setAttribute('aria-modal', 'true');
  root.setAttribute('aria-label', 'Now playing');

  // The drag handle at the top is both an affordance ("swipe me down")
  // and the visible target of the touch gesture. The whole modal
  // background also handles the swipe so a stray drag anywhere on the
  // sheet closes it — matches Spotify / Apple Music behaviour.
  root.innerHTML = `
    <div class="now-playing__sheet" data-np="sheet">
      <div class="now-playing__handle" aria-hidden="true"></div>
      <button
        type="button"
        class="now-playing__close"
        data-np="close"
        aria-label="Close now playing"
      >&times;</button>

      <div class="now-playing__art" data-np="art">
        <span class="now-playing__art-glyph" data-np="glyph"></span>
      </div>

      <div class="now-playing__meta">
        <div class="now-playing__title" data-np="title"></div>
        <div class="now-playing__artist muted" data-np="artist"></div>
        <div class="now-playing__album muted" data-np="album"></div>
      </div>

      <div class="now-playing__progress">
        <input
          type="range"
          class="now-playing__seek"
          data-np="seek"
          min="0"
          max="0"
          step="1000"
          value="0"
          aria-label="Seek position"
        />
        <div class="now-playing__times">
          <span class="now-playing__time mono" data-np="pos">0:00</span>
          <span class="now-playing__time mono" data-np="dur">0:00</span>
        </div>
      </div>

      <div class="now-playing__controls">
        <button
          type="button"
          class="now-playing__btn"
          data-np="prev"
          aria-label="Previous track"
        >&laquo;</button>
        <button
          type="button"
          class="now-playing__btn now-playing__btn--primary"
          data-np="play"
          aria-label="Play / Pause"
        >Play</button>
        <button
          type="button"
          class="now-playing__btn"
          data-np="next"
          aria-label="Next track"
        >&raquo;</button>
      </div>

      <div class="now-playing__queue">
        <div class="now-playing__queue-header muted">Up next</div>
        <ul class="now-playing__queue-list" data-np="queue" role="list"></ul>
      </div>
    </div>
  `;
  return root;
}

// -- renderNowPlaying -------------------------------------------------

/**
 * Mount the full-screen now-playing overlay into [container] and wire
 * it up to [ctx.player]. Returns a detach function that unsubscribes
 * from the player and removes the overlay DOM. Calling detach() more
 * than once is safe.
 */
export function renderNowPlaying(
  container: HTMLElement,
  ctx: NowPlayingCtx,
): NowPlayingDetach {
  const { player, onClose } = ctx;

  const root  = buildSkeleton();
  container.appendChild(root);

  // Element refs — narrow them once so the render functions don't
  // repeat the querySelector + cast dance on every event.
  const $sheet  = root.querySelector('[data-np="sheet"]')  as HTMLElement;
  const $close  = root.querySelector('[data-np="close"]')  as HTMLButtonElement;
  const $glyph  = root.querySelector('[data-np="glyph"]')  as HTMLElement;
  const $title  = root.querySelector('[data-np="title"]')  as HTMLElement;
  const $artist = root.querySelector('[data-np="artist"]') as HTMLElement;
  const $album  = root.querySelector('[data-np="album"]')  as HTMLElement;
  const $seek   = root.querySelector('[data-np="seek"]')   as HTMLInputElement;
  const $pos    = root.querySelector('[data-np="pos"]')    as HTMLElement;
  const $dur    = root.querySelector('[data-np="dur"]')    as HTMLElement;
  const $prev   = root.querySelector('[data-np="prev"]')   as HTMLButtonElement;
  const $play   = root.querySelector('[data-np="play"]')   as HTMLButtonElement;
  const $next   = root.querySelector('[data-np="next"]')   as HTMLButtonElement;
  const $queue  = root.querySelector('[data-np="queue"]')  as HTMLElement;

  // While the user is dragging the slider, mute incoming position
  // events so the thumb doesn't fight the touch. Same _dragMs guard
  // PlaybackProgressBar uses in Dart.
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
    } catch (_) { /* swallow — controller surfaces errors elsewhere */ }
  }

  function scheduleSeek(ms: number): void {
    pendingSeekMs = ms;
    if (seekTimer) clearTimeout(seekTimer);
    seekTimer = setTimeout(() => {
      seekTimer = null;
      flushSeek();
    }, SEEK_DEBOUNCE_MS);
  }

  // -- Renderers -----------------------------------------------------
  //
  // Three layers: meta (title/artist/album/art), state (play button +
  // skip enablement + queue), position (slider thumb + time labels).
  // Each can be invoked independently so a high-frequency position
  // event doesn't pay for a queue rebuild.

  function renderMeta(): void {
    const song = player.currentSong;
    $title.textContent  = songTitle(song);
    $artist.textContent = songArtist(song);
    $album.textContent  = songAlbum(song);
    $glyph.textContent  = artGlyph(song);

    // Duration only changes when the song changes; pin the slider
    // range here so dragging the unknown-duration case caps at the
    // current position rather than an arbitrary 0..1 .
    const dur = Math.max(0, player.durationMs | 0);
    $dur.textContent = formatTime(dur);
    $seek.max = String(dur);
    $seek.disabled = dur === 0;
  }

  function renderQueue(): void {
    // The queue shows the upcoming tracks AFTER the current one,
    // with the current row highlighted at the top. Dart's home screen
    // does the same: a single ListView with the "now playing" row
    // pinned visually distinct from the queue.
    const list = player.playlist;
    const idx  = player.playlistIndex;

    // Clear and rebuild — the queue is small (typical playlists are
    // 1..50 tracks) so the cost of a full rebuild is negligible
    // compared to the bookkeeping a diff would need.
    $queue.replaceChildren();

    if (list.length === 0) {
      const li = document.createElement('li');
      li.className = 'now-playing__queue-empty muted';
      li.textContent = 'No upcoming tracks';
      $queue.appendChild(li);
      return;
    }

    list.forEach((song, i) => {
      const li = document.createElement('li');
      li.className = 'now-playing__queue-row';
      if (i === idx) li.classList.add('now-playing__queue-row--current');

      // Two-column row: number + meta. Number is the playlist
      // position; current row uses a glyph instead of a digit so
      // the highlight reads at a glance.
      const num = document.createElement('span');
      num.className = 'now-playing__queue-num mono muted';
      num.textContent = i === idx ? '>' : String(i + 1);

      const meta = document.createElement('span');
      meta.className = 'now-playing__queue-meta';

      const title = document.createElement('span');
      title.className = 'now-playing__queue-title';
      title.textContent = (song.title ?? '').trim() || '(untitled)';

      const artist = document.createElement('span');
      artist.className = 'now-playing__queue-artist muted';
      artist.textContent = (song.artist ?? '').trim();

      meta.append(title, artist);
      li.append(num, meta);
      $queue.appendChild(li);
    });
  }

  function renderState(): void {
    switch (player.state) {
      case 'playing':
        $play.textContent = 'Pause';
        $play.setAttribute('aria-label', 'Pause');
        $play.disabled = false;
        break;
      case 'loading':
        $play.textContent = '...';
        $play.setAttribute('aria-label', 'Loading');
        $play.disabled = true;
        break;
      case 'paused':
      case 'stopped':
      case 'idle':
      default:
        $play.textContent = 'Play';
        $play.setAttribute('aria-label', 'Play');
        // Same gating as the mini-player: no-op togglePlayPause from
        // stopped / idle, so disable the button to make that visible.
        $play.disabled = player.state === 'stopped' || player.state === 'idle';
        break;
    }

    // Skip controls only meaningful with multi-track queues.
    const multi = player.playlist.length > 1;
    $prev.disabled = !multi || player.playlistIndex <= 0;
    $next.disabled = !multi ||
                     player.playlistIndex >= player.playlist.length - 1;

    // Meta + queue may have changed (the controller fires 'state'
    // after queue mutations too) — refresh both.
    renderMeta();
    renderQueue();
    renderPosition();
  }

  function renderPosition(): void {
    if (dragging) return;
    const dur = Math.max(0, player.durationMs | 0);
    const pos = Math.min(Math.max(0, player.positionMs | 0), dur);
    $pos.textContent = formatTime(pos);
    $seek.value = String(pos);
  }

  // -- Close handling ------------------------------------------------
  //
  // Three close paths: Escape key, the X button, and swipe-down on
  // the sheet. All three call onClose, then the caller is responsible
  // for invoking the detach function we returned. We don't unmount
  // ourselves here so the dismissal animation (if the host wants
  // one) has a hook.

  let closing = false;
  function triggerClose(): void {
    if (closing) return;
    closing = true;
    try { onClose(); }
    catch (_) { /* swallow — the host should not be able to break us */ }
  }

  // Escape key — bound on the document so the overlay catches the
  // press regardless of which inner control has focus.
  const onKeyDown = (ev: KeyboardEvent): void => {
    if (ev.key === 'Escape') {
      ev.stopPropagation();
      triggerClose();
    }
  };

  // Swipe-down. We track pointerdown / pointermove / pointerup on the
  // sheet and translate the sheet during the gesture for direct
  // feedback. On release, if the displacement exceeds the threshold
  // we close; otherwise we snap back to 0.
  let pointerActive = false;
  let pointerStartY = 0;
  let pointerStartX = 0;
  let pointerId     = -1;
  let pointerScrollLocked = false;
  // Vertical translation cap so flinging the sheet down doesn't tear
  // it off the viewport into invisible space. Half a screen is enough
  // visual feedback.
  const MAX_TRANSLATE = 600;

  function isOnControl(target: EventTarget | null): boolean {
    // The slider input + queue rows + buttons all need to receive
    // their own pointer events without the sheet's drag handler
    // stealing them. Pointer events on <input type="range">,
    // <button>, or any descendant of the queue list are excluded.
    if (!(target instanceof Element)) return false;
    if (target.closest('input')) return true;
    if (target.closest('button')) return true;
    if (target.closest('.now-playing__queue')) return true;
    return false;
  }

  const onPointerDown = (ev: PointerEvent): void => {
    if (ev.button !== 0 && ev.pointerType === 'mouse') return;
    if (isOnControl(ev.target)) return;
    pointerActive = true;
    pointerScrollLocked = false;
    pointerStartY = ev.clientY;
    pointerStartX = ev.clientX;
    pointerId     = ev.pointerId;
    // Capture so we keep getting move/up events even if the finger
    // wanders off the sheet bounds.
    try { $sheet.setPointerCapture(ev.pointerId); }
    catch { /* not supported — fall through */ }
  };

  const onPointerMove = (ev: PointerEvent): void => {
    if (!pointerActive || ev.pointerId !== pointerId) return;
    const dy = ev.clientY - pointerStartY;
    const dx = ev.clientX - pointerStartX;
    // Only treat down-swipes (positive dy) as the dismiss gesture;
    // up-swipes do nothing so they can't trigger a stray close on
    // intent-to-scroll. We also bail if the horizontal component
    // dominates — that's a horizontal scroll attempt, not a sheet
    // drag.
    if (!pointerScrollLocked) {
      if (Math.abs(dx) > Math.abs(dy) && Math.abs(dx) > 12) {
        // Horizontal drag — release the gesture.
        pointerActive = false;
        try { $sheet.releasePointerCapture(ev.pointerId); }
        catch { /* swallow */ }
        $sheet.style.transform = '';
        return;
      }
      if (Math.abs(dy) > 4) pointerScrollLocked = true;
    }
    if (dy <= 0) {
      $sheet.style.transform = '';
      return;
    }
    const clamped = Math.min(dy, MAX_TRANSLATE);
    $sheet.style.transform = `translateY(${clamped}px)`;
  };

  const onPointerUp = (ev: PointerEvent): void => {
    if (!pointerActive || ev.pointerId !== pointerId) return;
    pointerActive = false;
    try { $sheet.releasePointerCapture(ev.pointerId); }
    catch { /* swallow */ }
    const dy = ev.clientY - pointerStartY;
    if (dy >= SWIPE_DISMISS_PX) {
      // Leave the sheet in its dragged position so the parent's
      // dismiss animation continues smoothly.
      triggerClose();
      return;
    }
    // Snap back. The transition is set up via CSS on the sheet — we
    // just clear the inline transform and the CSS class handles the
    // springback.
    $sheet.style.transform = '';
  };

  // -- DOM listeners -------------------------------------------------

  const onSeekInput = (): void => {
    dragging = true;
    const ms = Number($seek.value);
    if (!Number.isFinite(ms)) return;
    // Show the dragged time immediately so the user sees feedback.
    $pos.textContent = formatTime(ms);
    scheduleSeek(ms);
  };

  const onSeekCommit = (): void => {
    dragging = false;
    if (seekTimer) {
      clearTimeout(seekTimer);
      seekTimer = null;
    }
    if (pendingSeekMs != null) flushSeek();
  };

  const onPlayClick = (): void => { void player.togglePlayPause(); };
  const onNextClick = (): void => {
    if (player.playlist.length <= 1) return;
    void player.playNext();
  };
  const onPrevClick = (): void => {
    if (player.playlist.length <= 1) return;
    void player.playPrev();
  };
  const onCloseClick = (): void => { triggerClose(); };

  $seek.addEventListener('input', onSeekInput);
  $seek.addEventListener('change', onSeekCommit);
  $play.addEventListener('click', onPlayClick);
  $next.addEventListener('click', onNextClick);
  $prev.addEventListener('click', onPrevClick);
  $close.addEventListener('click', onCloseClick);

  $sheet.addEventListener('pointerdown', onPointerDown);
  $sheet.addEventListener('pointermove', onPointerMove);
  $sheet.addEventListener('pointerup', onPointerUp);
  $sheet.addEventListener('pointercancel', onPointerUp);

  document.addEventListener('keydown', onKeyDown, true);

  // -- Player listeners ----------------------------------------------

  const onStateChange    = (): void => renderState();
  const onPositionChange = (): void => renderPosition();

  player.on('state', onStateChange);
  player.on('position', onPositionChange);

  // Initial paint.
  renderState();

  // -- Detach --------------------------------------------------------

  let detached = false;
  return () => {
    if (detached) return;
    detached = true;
    player.off('state', onStateChange);
    player.off('position', onPositionChange);
    $seek.removeEventListener('input', onSeekInput);
    $seek.removeEventListener('change', onSeekCommit);
    $play.removeEventListener('click', onPlayClick);
    $next.removeEventListener('click', onNextClick);
    $prev.removeEventListener('click', onPrevClick);
    $close.removeEventListener('click', onCloseClick);
    $sheet.removeEventListener('pointerdown', onPointerDown);
    $sheet.removeEventListener('pointermove', onPointerMove);
    $sheet.removeEventListener('pointerup', onPointerUp);
    $sheet.removeEventListener('pointercancel', onPointerUp);
    document.removeEventListener('keydown', onKeyDown, true);
    if (seekTimer) {
      clearTimeout(seekTimer);
      seekTimer = null;
    }
    if (root.parentNode) root.parentNode.removeChild(root);
    // Touch the escapeHtml helper so it doesn't get tree-shaken into
    // a warning under `noUnusedLocals` — kept around because future
    // tooltip / aria-description code will need it.
    void escapeHtml;
  };
}
