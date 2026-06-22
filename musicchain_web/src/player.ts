// Browser-side audio player. Mirrors the five-state machine in the
// Flutter PlayerProvider (lib/src/providers/player_provider.dart) so the
// rest of the web app can reason about state with the same vocabulary
// the player tab on Android / Windows already uses.
//
// Backing implementation is the platform <audio> element — libmpv / media_kit
// has no web build, and HTMLAudioElement already speaks every codec the
// chain currently allows (mp3/ogg/m4a/aac/opus/wav/flac via browser-native
// decoders).
//
// Sources:
//   * Songs streamed from a swarm peer arrive as binary chunks the caller
//     reassembles into a Blob. Pass the Blob directly to load() — we wrap
//     it in URL.createObjectURL() and revoke the URL on the next load() /
//     stop() so we don't leak object URLs on long playlists.
//   * Songs served by the home node's stream.open endpoint arrive as a
//     plain string URL (http://… or blob:…); pass that as-is.

export type PlayerStateValue =
  | 'idle'
  | 'loading'
  | 'playing'
  | 'paused'
  | 'stopped';

export class Player extends EventTarget {
  // We own a single HTMLAudioElement for the lifetime of the Player. The
  // element is not attached to the DOM — playback works headless and the
  // caller decides whether to expose any visible <audio controls> UI.
  private readonly _audio: HTMLAudioElement;

  // Tracks the state machine. Mirrors PlayerState in player_provider.dart:
  // idle (no song loaded), loading (open() in flight), playing, paused,
  // stopped (terminal — set by stop() or by ended event when no next).
  private _state: PlayerStateValue = 'idle';

  // Object URL we created via URL.createObjectURL — kept so we can
  // revokeObjectURL() on the next load() / stop() and not leak memory
  // across a playlist where every track ships as a Blob.
  private _objectUrl: string | null = null;

  // Cached duration in ms. The Dart side reads this from
  // Song.durationMs / mpv's stream.duration. In the browser we pull it
  // off the <audio> element's `duration` property once metadata loads.
  private _durationMs = 0;

  // Position in ms, refreshed from the timeupdate event. The
  // HeartbeatService reads this every 5s; keeping it as a number on the
  // Player itself avoids a round trip through the audio element on each
  // heartbeat tick.
  private _positionMs = 0;

  constructor() {
    super();
    this._audio = new Audio();
    // We never display the element ourselves, so `preload=auto` is the
    // right default: browsers can begin buffering the moment src is
    // assigned without waiting for a `play()` user gesture.
    this._audio.preload = 'auto';
    this._wireEvents();
  }

  // ---- Public API --------------------------------------------------

  /// Load a new source. Accepts either an `http(s)://` URL string from
  /// the home node's stream.open response, or a Blob the caller built
  /// by concatenating swarm-peer binary chunks. Returns when the audio
  /// element reports it has enough data to begin playback.
  ///
  /// State transition: any → loading → (paused | stopped). load() does
  /// NOT auto-play — pair it with play() in the caller. This matches
  /// player_provider.dart's `open(media, play: false)` choice so we
  /// don't race the browser's autoplay heuristic.
  async load(source: string | Blob): Promise<void> {
    // Tear down any prior object URL before swapping src — otherwise a
    // long queue would accumulate as many revocable URLs as it has
    // tracks. The browser-internal blob stays alive as long as the page
    // holds a Blob reference, but the URL itself is per-call.
    this._revokeObjectUrl();

    const url =
      typeof source === 'string'
        ? source
        : (() => {
            const u = URL.createObjectURL(source);
            this._objectUrl = u;
            return u;
          })();

    this._setState('loading');
    this._positionMs = 0;
    this._durationMs = 0;
    this._audio.src = url;

    // Force the element to begin buffering. Without an explicit load()
    // some browsers wait until play() before they even fetch the first
    // byte, which defeats the point of preload=auto for blob: URLs.
    this._audio.load();

    await this._waitForCanPlay();

    // Capture the now-known duration so positionMs/durationMs callers
    // (UI seek bar, heartbeat) don't see 0 between the load() resolve
    // and the first durationchange event landing.
    const dur = this._audio.duration;
    if (Number.isFinite(dur) && dur > 0) {
      this._durationMs = Math.floor(dur * 1000);
    }

    // Loaded-but-not-yet-playing maps to 'paused' in the Dart state
    // machine (the player tab shows a play button at offset 0). 'stopped'
    // would be wrong — stopped is terminal and means the user pressed
    // stop or the track ended.
    this._setState('paused');
  }

  /// Begin / resume playback. Idempotent if already playing. Resolves
  /// once the browser's `play()` promise resolves; rejection paths
  /// (autoplay blocked, no src, decode error) bubble to the caller.
  async play(): Promise<void> {
    if (this._state === 'playing') return;
    // The browser's HTMLMediaElement.play() returns a Promise that
    // rejects on autoplay denial. We propagate that to the caller so
    // the UI can surface "click to enable audio" instead of silently
    // swallowing the failure.
    await this._audio.play();
    // The 'playing' DOM event handler in _wireEvents will set state to
    // 'playing' too, but setting it eagerly here avoids a tick of
    // "user pressed play but state still says paused" in the UI.
    this._setState('playing');
  }

  /// Pause without releasing the source. Mirrors PlayerProvider.pause.
  pause(): void {
    if (this._state !== 'playing' && this._state !== 'loading') return;
    this._audio.pause();
    this._setState('paused');
  }

  /// Terminal stop. Equivalent of PlayerProvider.stop(): releases the
  /// underlying source, zeroes the position, and emits state='stopped'.
  /// The caller is expected to drive the session-complete RPC; the
  /// player itself is intentionally chain-agnostic.
  stop(): void {
    this._audio.pause();
    // Setting currentTime before clearing src avoids a noisy seeking
    // event the moment we re-load() in the next track of a playlist.
    try {
      this._audio.currentTime = 0;
    } catch {
      // Some browsers throw NotSupportedError if there's no src; ignore.
    }
    this._audio.removeAttribute('src');
    // Force the element to drop its internal media resource — without
    // this, the network buffer stays alive until GC.
    this._audio.load();
    this._revokeObjectUrl();
    this._positionMs = 0;
    this._setState('stopped');
  }

  /// Seek to [ms]. Clamped to [0, durationMs]. Mirrors
  /// PlayerProvider.seek — the heartbeat service picks up the new
  /// position on its next tick so the full node can decide whether
  /// the post-seek interval counts toward effective listen time.
  seek(ms: number): void {
    const dur = this._durationMs > 0 ? this._durationMs : ms;
    const clamped = Math.max(0, Math.min(ms, dur));
    // HTMLMediaElement.currentTime is in seconds, ms in our public API.
    this._audio.currentTime = clamped / 1000;
    this._positionMs = clamped;
    this.dispatchEvent(
      new CustomEvent<number>('position', { detail: clamped }),
    );
  }

  get positionMs(): number {
    return this._positionMs;
  }

  get durationMs(): number {
    return this._durationMs;
  }

  get state(): PlayerStateValue {
    return this._state;
  }

  // ---- Internal ----------------------------------------------------

  private _setState(next: PlayerStateValue): void {
    if (this._state === next) return;
    this._state = next;
    this.dispatchEvent(
      new CustomEvent<PlayerStateValue>('state', { detail: next }),
    );
  }

  private _wireEvents(): void {
    const a = this._audio;

    // The Dart side listens to mpv's `playing` stream — the browser's
    // equivalent is the pair (playing, pause). We deliberately do NOT
    // listen for 'play' (fired before audio actually starts) since the
    // Dart side gates on libmpv's authoritative "started decoding" event.
    a.addEventListener('playing', () => {
      // Ignore while loading / stopped / idle so a transient browser
      // event can't flip us out of a terminal state. Same defensive
      // gating PlayerProvider has against mpv resurrecting a session.
      if (
        this._state === 'loading' ||
        this._state === 'stopped' ||
        this._state === 'idle'
      ) {
        return;
      }
      this._setState('playing');
    });
    a.addEventListener('pause', () => {
      if (
        this._state === 'loading' ||
        this._state === 'stopped' ||
        this._state === 'idle'
      ) {
        return;
      }
      // The 'ended' event fires a separate 'pause' on some browsers; if
      // we're already past the duration, let the ended handler own the
      // transition instead of flipping to 'paused' for one frame.
      if (this._durationMs > 0 && this._positionMs >= this._durationMs - 50) {
        return;
      }
      this._setState('paused');
    });

    a.addEventListener('timeupdate', () => {
      this._positionMs = Math.floor(a.currentTime * 1000);
      this.dispatchEvent(
        new CustomEvent<number>('position', { detail: this._positionMs }),
      );
    });

    a.addEventListener('durationchange', () => {
      if (Number.isFinite(a.duration) && a.duration > 0) {
        this._durationMs = Math.floor(a.duration * 1000);
      }
    });

    a.addEventListener('ended', () => {
      // Match the Dart _onComplete path: the player transitions to
      // 'stopped' and emits 'ended' so the caller (PlayerProvider's web
      // equivalent) can fire session.complete and queue the next track.
      this._positionMs = this._durationMs;
      this._setState('stopped');
      this.dispatchEvent(new CustomEvent<void>('ended'));
    });

    a.addEventListener('error', () => {
      // A decode / network error during loading lands us in 'stopped'.
      // PlayerProvider does the same in the catch around _player.open.
      if (this._state === 'loading' || this._state === 'playing') {
        this._setState('stopped');
      }
    });
  }

  // Wait until the audio element can begin playback. canplay fires
  // earlier than canplaythrough but is usually enough for a play()
  // call to succeed; if the network stalls afterwards the browser
  // pauses and we'll flip to 'paused' via the pause handler.
  private _waitForCanPlay(): Promise<void> {
    return new Promise((resolve, reject) => {
      const a = this._audio;
      const onCanPlay = () => {
        cleanup();
        resolve();
      };
      const onError = () => {
        cleanup();
        reject(new Error('player: load failed (decode / network error)'));
      };
      const cleanup = () => {
        a.removeEventListener('canplay', onCanPlay);
        a.removeEventListener('error', onError);
      };
      a.addEventListener('canplay', onCanPlay);
      a.addEventListener('error', onError);
    });
  }

  private _revokeObjectUrl(): void {
    if (this._objectUrl !== null) {
      URL.revokeObjectURL(this._objectUrl);
      this._objectUrl = null;
    }
  }
}
