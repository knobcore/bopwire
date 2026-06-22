// Adapter that wraps the raw `Player` (HTMLAudioElement-backed, EventTarget
// shape) into the `PlayerLike` surface the mini-player widget and the
// home shell expect.
//
// Why a separate file: `Player` is intentionally chain-agnostic — it
// only knows about audio. Concepts like the current `Song`, the playlist
// queue, the `togglePlayPause` / `playNext` / `playPrev` semantics, and
// the `on('state', cb)` / `on('position', cb)` API live here because
// they're shell-level concerns. Keeping them out of `Player` lets the
// home page wire whatever queue model it wants without forking the
// audio class.
//
// State propagation:
//   * `state` & `currentSong` & `durationMs` & `positionMs` are read on
//     demand from the underlying Player + this controller's own fields.
//   * 'state' fans out when the underlying Player emits 'state' OR
//     when the controller mutates the queue (currentSong / playlist /
//     index change), so the widget gets a single re-render signal
//     regardless of which side moved.
//   * 'position' is forwarded one-to-one from the underlying Player.

import type { SongRow } from './verbs';
import type {
  PlayerLike,
  PlayerEvent,
  PlayerState,
} from './widgets/mini_player';
import { Player } from './player';

/** Options for PlayerController.loadAndPlay. */
export interface LoadAndPlayOptions {
  /** Audio source — either a streaming URL or a Blob the caller built
   *  from swarm bytes. */
  source: string | Blob;
  /** Song row to display in the mini-player while this source plays. */
  song: SongRow;
}

/**
 * Wraps a `Player` instance with playlist / queue semantics and the
 * `PlayerLike` event surface the rest of the app expects.
 *
 * Single-source playback: every call to `loadAndPlay` replaces the
 * single-element playlist with `[song]`. Multi-track queues come via
 * `setPlaylist(songs, startIndex)`.
 */
export class PlayerController implements PlayerLike {
  // Underlying audio engine — owns the actual <audio> element. We treat
  // it as a black box past the EventTarget surface.
  readonly player: Player;

  // Queue model. Mirrors PlayerProvider in player_provider.dart.
  playlist: SongRow[] = [];
  playlistIndex = -1;
  currentSong: SongRow | null = null;

  // Listener bookkeeping so the on/off pair can deregister specific
  // handlers. We can't use addEventListener directly because the widget
  // wants the no-arg `() => void` signature, and addEventListener takes
  // an Event-typed handler. The two maps wrap each registered handler
  // with the EventListener the underlying EventTarget expects.
  private readonly _stateListeners = new Map<() => void, EventListener>();
  private readonly _positionListeners = new Map<() => void, EventListener>();

  constructor(player?: Player) {
    this.player = player ?? new Player();
  }

  // ---- PlayerLike getters -----------------------------------------

  get state(): PlayerState {
    return this.player.state;
  }

  get positionMs(): number {
    return this.player.positionMs;
  }

  get durationMs(): number {
    return this.player.durationMs;
  }

  // ---- Queue control ----------------------------------------------

  /**
   * Replace whatever was playing with a single song and start it. The
   * caller is responsible for the `session.start` RPC before this fires
   * (or the HomeShell's `loadAndPlay` adapter does it on top of this).
   */
  async loadAndPlay(opts: LoadAndPlayOptions): Promise<void> {
    this.playlist = [opts.song];
    this.playlistIndex = 0;
    this.currentSong = opts.song;
    this._fanoutState();

    await this.player.load(opts.source);
    await this.player.play();
  }

  /** Replace the playlist. Does NOT start playback — call `play()` after. */
  setPlaylist(songs: SongRow[], startIndex = 0): void {
    this.playlist = songs.slice();
    this.playlistIndex = startIndex.clamp(0, Math.max(0, songs.length - 1));
    this.currentSong = this.playlist[this.playlistIndex] ?? null;
    this._fanoutState();
  }

  // ---- PlayerLike methods -----------------------------------------

  async togglePlayPause(): Promise<void> {
    const s = this.player.state;
    if (s === 'playing') {
      this.player.pause();
      return;
    }
    if (s === 'paused') {
      await this.player.play();
      return;
    }
    // idle / loading / stopped — nothing meaningful to do. Mirrors the
    // Dart provider which only flips between playing & paused.
  }

  async playNext(): Promise<void> {
    if (this.playlist.length <= 1) return;
    const next = this.playlistIndex + 1;
    if (next >= this.playlist.length) return;
    this.playlistIndex = next;
    const song = this.playlist[next];
    if (!song) return;
    this.currentSong = song;
    this._fanoutState();
    // Re-load the underlying player. The caller supplies the source via
    // a separate hook (see HomeShell.loadAndPlay) — at this layer we
    // can't resolve a song to a URL without the chain context. So we
    // fire a 'state' event and let the host wire the next source.
  }

  async playPrev(): Promise<void> {
    if (this.playlist.length <= 1) return;
    const prev = this.playlistIndex - 1;
    if (prev < 0) return;
    this.playlistIndex = prev;
    const song = this.playlist[prev];
    if (!song) return;
    this.currentSong = song;
    this._fanoutState();
  }

  async seek(ms: number): Promise<void> {
    this.player.seek(ms);
  }

  // ---- on/off event bridge ----------------------------------------

  on(event: PlayerEvent, handler: () => void): void {
    const wrapped: EventListener = () => handler();
    if (event === 'state') {
      this._stateListeners.set(handler, wrapped);
      this.player.addEventListener('state', wrapped);
    } else if (event === 'position') {
      this._positionListeners.set(handler, wrapped);
      this.player.addEventListener('position', wrapped);
    }
  }

  off(event: PlayerEvent, handler: () => void): void {
    const map =
      event === 'state' ? this._stateListeners : this._positionListeners;
    const wrapped = map.get(handler);
    if (!wrapped) return;
    map.delete(handler);
    this.player.removeEventListener(event, wrapped);
  }

  // ---- internals ---------------------------------------------------

  /** Dispatch a synthetic 'state' on the underlying player so on('state')
   *  subscribers re-read currentSong / playlist after a queue mutation. */
  private _fanoutState(): void {
    this.player.dispatchEvent(new CustomEvent('state', { detail: this.player.state }));
  }
}

// Polyfill: TypeScript's stdlib doesn't declare Number.prototype.clamp,
// even though `Math.min/Math.max` work fine. Adding it as a small helper
// directly on Number's prototype is overkill — instead we patch the type
// inline with a tiny extension. Easier: inline a helper.
declare global {
  interface Number {
    clamp(lo: number, hi: number): number;
  }
}
if (!Object.prototype.hasOwnProperty.call(Number.prototype, 'clamp')) {
  Object.defineProperty(Number.prototype, 'clamp', {
    value(this: number, lo: number, hi: number): number {
      return Math.min(Math.max(this, lo), hi);
    },
    writable: false,
    enumerable: false,
    configurable: false,
  });
}
