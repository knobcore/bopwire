// Playback queue model for the web shell. Mirrors the queue half of
// musicchain_player/lib/src/providers/player_provider.dart: an ordered
// list of SongRows plus a cursor (`index`) into it, with the usual
// add/playNext/playPrev/remove primitives.
//
// Lives outside the Player class because the Player is intentionally
// chain-agnostic — it only knows about audio. The Queue is a shell-level
// concept consumed by the mini-player widget, the now-playing surface,
// and any tab (artist / album / search / library) that wants to push a
// song into the active session.
//
// Mutation contract: every method that changes `tracks` or `index`
// dispatches a single 'change' event after the mutation lands. Listeners
// re-read the queue state (length, current, tracks slice) on the event;
// the event itself carries no payload because the cheap O(1) reads off
// the Queue are already up to date by the time it fires.
//
// Persistence: instance methods load() / save() round-trip a tiny JSON
// blob through localStorage. The shell calls load() on construct and
// save() inside the 'change' subscriber, so a reload restores the user's
// last queue + cursor position. We deliberately do NOT auto-resume
// playback — restoring the queue is enough; the play button is one tap
// away and the browser autoplay rules would block it anyway.

import type { SongRow } from './verbs';

/** localStorage key for queue persistence. Namespaced under `mc.` like
 *  the other persisted bits (mc.username, mc.wallet, etc.) so a stray
 *  clear() in one feature doesn't blast unrelated state. */
export const QUEUE_STORAGE_KEY = 'mc.queue';

/** Disk shape of the persisted queue. Versioned so a future schema
 *  change can detect-and-discard old blobs without throwing on load. */
interface PersistedQueue {
  v: 1;
  tracks: SongRow[];
  index: number;
}

/**
 * Ordered playback queue. EventTarget so listeners (mini-player widget,
 * now-playing screen) can subscribe via addEventListener('change', cb)
 * the same way they subscribe to the underlying Player's events.
 *
 * The queue holds a flat list of `SongRow`s and a cursor (`index`)
 * pointing at the currently-active track, or -1 when the queue is empty.
 * All mutating methods funnel through `_emitChange` so every mutation
 * lands exactly one 'change' event regardless of how many internal
 * fields it touched.
 */
export class Queue extends EventTarget {
  /** Backing list. Treat as immutable externally — mutate only through
   *  the methods on this class so the 'change' event stays consistent. */
  tracks: SongRow[] = [];

  /** Cursor into `tracks`. -1 when the queue is empty; otherwise a valid
   *  index in [0, tracks.length). Callers should not write this field
   *  directly — use `next()` / `prev()` / `playNow()` to move it. */
  index = -1;

  // ---- Getters ------------------------------------------------------

  /** The currently-active track, or null when the queue is empty. */
  get current(): SongRow | null {
    if (this.index < 0 || this.index >= this.tracks.length) return null;
    return this.tracks[this.index] ?? null;
  }

  /** Number of tracks currently in the queue. */
  get length(): number {
    return this.tracks.length;
  }

  /** True iff `next()` would move the cursor to a real track. */
  get hasNext(): boolean {
    return this.index >= 0 && this.index + 1 < this.tracks.length;
  }

  /** True iff `prev()` would move the cursor to a real track. */
  get hasPrev(): boolean {
    return this.index > 0 && this.tracks.length > 0;
  }

  // ---- Mutators -----------------------------------------------------

  /** Append one or more songs to the end of the queue. If the queue was
   *  empty, the cursor lands on the first appended song so the next
   *  call to `current` returns something useful. */
  add(...songs: SongRow[]): void {
    if (songs.length === 0) return;
    const wasEmpty = this.tracks.length === 0;
    for (const s of songs) this.tracks.push(s);
    if (wasEmpty) this.index = 0;
    this._emitChange();
  }

  /** Insert a song immediately after the current cursor. Mirrors the
   *  "play next" affordance in the Dart now-playing menu. If the queue
   *  is empty, behaves like `add(song)`. */
  addNext(song: SongRow): void {
    if (this.tracks.length === 0) {
      this.tracks.push(song);
      this.index = 0;
      this._emitChange();
      return;
    }
    // Clamp insertion point so a stale cursor (e.g. -1) still lands at
    // the front of the queue rather than throwing on splice.
    const insertAt = Math.max(0, this.index + 1);
    this.tracks.splice(insertAt, 0, song);
    this._emitChange();
  }

  /** Move the cursor onto `song` and surface it as the current track.
   *  If `atIndex` is supplied and points at an existing slot, the song
   *  there is replaced with `song`; otherwise `song` is appended and
   *  the cursor jumps to the new tail. Used by the search-row click and
   *  the now-playing "play from here" affordance. */
  playNow(song: SongRow, atIndex?: number): void {
    if (atIndex !== undefined && atIndex >= 0 && atIndex < this.tracks.length) {
      this.tracks[atIndex] = song;
      this.index = atIndex;
    } else {
      this.tracks.push(song);
      this.index = this.tracks.length - 1;
    }
    this._emitChange();
  }

  /** Remove the track at `idx`. The cursor stays pointing at the same
   *  logical "now playing" song where possible: if the removed track
   *  was before the cursor, the cursor shifts left by one; if it was
   *  the cursor itself, the cursor stays put so the next track slides
   *  into place; if it was after the cursor, the cursor doesn't move. */
  remove(idx: number): void {
    if (idx < 0 || idx >= this.tracks.length) return;
    this.tracks.splice(idx, 1);
    if (this.tracks.length === 0) {
      this.index = -1;
    } else if (idx < this.index) {
      this.index -= 1;
    } else if (idx === this.index) {
      // Removed the current track. Clamp the cursor so it still points
      // at a real slot — the new "current" is whatever slid into the
      // removed position, or the new tail if we removed the last one.
      if (this.index >= this.tracks.length) {
        this.index = this.tracks.length - 1;
      }
    }
    // idx > this.index: cursor unchanged, but length did, so still emit.
    this._emitChange();
  }

  /** Empty the queue and reset the cursor. */
  clear(): void {
    if (this.tracks.length === 0 && this.index === -1) return;
    this.tracks = [];
    this.index = -1;
    this._emitChange();
  }

  /** Advance the cursor by one and return the now-current track. Returns
   *  null (and leaves the cursor pinned to the tail) when there is no
   *  next track — caller is responsible for the "queue ended" UX. */
  next(): SongRow | null {
    if (!this.hasNext) return null;
    this.index += 1;
    const cur = this.current;
    this._emitChange();
    return cur;
  }

  /** Move the cursor back by one and return the now-current track.
   *  Returns null (cursor unchanged) when there is no previous track. */
  prev(): SongRow | null {
    if (!this.hasPrev) return null;
    this.index -= 1;
    const cur = this.current;
    this._emitChange();
    return cur;
  }

  // ---- Persistence --------------------------------------------------

  /** Rehydrate from localStorage. Silent no-op if the slot is missing,
   *  the JSON doesn't parse, or the schema version doesn't match — a
   *  fresh queue is a safer fallback than crashing the shell mount. */
  load(): void {
    let raw: string | null = null;
    try {
      raw = localStorage.getItem(QUEUE_STORAGE_KEY);
    } catch {
      // Some privacy modes throw on localStorage access. Treat as empty.
      return;
    }
    if (!raw) return;
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      return;
    }
    if (!parsed || typeof parsed !== 'object') return;
    const p = parsed as Partial<PersistedQueue>;
    if (p.v !== 1 || !Array.isArray(p.tracks)) return;
    // Filter out anything that doesn't look like a SongRow — a partial
    // schema migration shouldn't leave a hole the rest of the player
    // dereferences as `.content_hash`.
    const tracks: SongRow[] = p.tracks.filter(
      (s): s is SongRow =>
        s !== null &&
        typeof s === 'object' &&
        typeof (s as SongRow).content_hash === 'string',
    );
    let index = typeof p.index === 'number' ? Math.floor(p.index) : -1;
    if (tracks.length === 0) {
      index = -1;
    } else if (index < 0 || index >= tracks.length) {
      index = 0;
    }
    // Apply atomically. We deliberately bypass `_emitChange` for the
    // initial restore — listeners typically aren't attached yet at the
    // moment the shell calls load(), and an extra event before any
    // listeners subscribe would force re-renderers to special-case the
    // pre-attach window. Callers that DO want a paint can subscribe
    // first, then call load(), then dispatchEvent('change').
    this.tracks = tracks;
    this.index = index;
  }

  /** Persist the current queue. Idempotent; safe to call on every
   *  'change' event. localStorage failures are swallowed — losing the
   *  persisted queue is better than crashing the user out of the
   *  current session. */
  save(): void {
    const payload: PersistedQueue = {
      v: 1,
      tracks: this.tracks,
      index: this.index,
    };
    try {
      localStorage.setItem(QUEUE_STORAGE_KEY, JSON.stringify(payload));
    } catch {
      // QuotaExceeded or storage-disabled mode. Nothing the queue can
      // do — the session keeps working in memory.
    }
  }

  // ---- Internal -----------------------------------------------------

  private _emitChange(): void {
    this.dispatchEvent(new CustomEvent('change'));
  }
}
