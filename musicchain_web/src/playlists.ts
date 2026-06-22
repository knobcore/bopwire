// Local playlist storage for the web player.
//
// First-pass implementation: localStorage-backed only. The chain has no
// playlist verb yet; once a `playlists.*` family lands in the home node
// (see TODO in chain_rpc), this module gains a sync-up/sync-down pair and
// the screens stop knowing where the data lives. Until then everything
// is per-browser, per-user — clearing site data wipes the lot, which is
// the same contract the local-library list (mc.library.local) has today.
//
// Storage key: 'mc.playlists' as a JSON array of Playlist objects. The
// outer key matches the rest of the player's local-state slots (see
// discovery.ts NODE_URL_KEY = 'mc.node_url', library.ts LOCAL_STORAGE_KEY
// = 'mc.library.local') so a future "clear local data" sweep can find
// every key by prefix.
//
// Track shape is SongRow from verbs.ts — same JSON the chain RPC paths
// already round-trip. We don't dedupe by content_hash on push: a user
// who wants a song twice in their playlist (a wedding-march encore,
// say) gets to have it twice. The screens are the right layer to surface
// "already in playlist" hints if we want them later.

import type { SongRow } from './verbs';

// ---------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------

/** One playlist as persisted. The id is randomly generated at create
 *  time (crypto.randomUUID where available, fallback below); name is
 *  user-supplied and free-form. createdAt is an ISO timestamp so a
 *  future "sort by recent" affordance has something to key on. */
export interface Playlist {
  id: string;
  name: string;
  tracks: SongRow[];
  createdAt: string;
}

// ---------------------------------------------------------------------
// Persistence helpers
// ---------------------------------------------------------------------

const STORAGE_KEY = 'mc.playlists';

/** Generate a fresh id. Prefers crypto.randomUUID (every browser this
 *  project targets has it); falls back to a manual hex string so unit
 *  tests under jsdom without a Crypto polyfill still work. */
function newId(): string {
  const c = (globalThis as { crypto?: Crypto }).crypto;
  if (c && typeof c.randomUUID === 'function') {
    return c.randomUUID();
  }
  // Cheap fallback — not v4-conformant but unique enough for a key.
  const rand = Math.random().toString(16).slice(2);
  return `pl-${Date.now().toString(16)}-${rand}`;
}

/** Read the persisted array. Anything malformed is silently dropped so
 *  a hand-edited or corrupted slot can't brick the screen — the worst
 *  case is "user sees no playlists" which we recover from the moment
 *  they create one. */
function readAll(): Playlist[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed)) return [];
    const out: Playlist[] = [];
    for (const item of parsed) {
      if (!item || typeof item !== 'object') continue;
      const p = item as Partial<Playlist>;
      if (typeof p.id !== 'string' || p.id.length === 0) continue;
      if (typeof p.name !== 'string') continue;
      const tracks = Array.isArray(p.tracks) ? (p.tracks as SongRow[]) : [];
      out.push({
        id: p.id,
        name: p.name,
        tracks: tracks.filter((t) => t && typeof t.content_hash === 'string'),
        createdAt: typeof p.createdAt === 'string' ? p.createdAt : new Date().toISOString(),
      });
    }
    return out;
  } catch {
    // localStorage can throw in private-mode Safari / behind a tracker
    // blocker, and JSON.parse throws on garbage. Either way, treat as
    // "no playlists yet".
    return [];
  }
}

/** Write the array. localStorage.setItem can throw on quota — we
 *  swallow it because there's nothing useful the caller can do, and the
 *  in-memory list still drives the current render until the next reload. */
function writeAll(playlists: Playlist[]): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(playlists));
  } catch {
    // Non-fatal — see comment above.
  }
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

/** List every playlist, in storage order. Storage order is creation
 *  order (we append on create), which gives a stable list across
 *  reloads — good enough for v1. */
export function listPlaylists(): Playlist[] {
  return readAll();
}

/** Create a new playlist with the given name. Trims whitespace; rejects
 *  an empty name so the UI doesn't end up with rows the user can't tell
 *  apart. Returns the new Playlist for the caller's convenience (so a
 *  "create and immediately open" flow can drill into the detail view
 *  without a second read). */
export function createPlaylist(name: string): Playlist {
  const trimmed = name.trim();
  if (trimmed.length === 0) {
    throw new Error('createPlaylist: name must not be empty');
  }
  const playlist: Playlist = {
    id: newId(),
    name: trimmed,
    tracks: [],
    createdAt: new Date().toISOString(),
  };
  const all = readAll();
  all.push(playlist);
  writeAll(all);
  return playlist;
}

/** Look up a playlist by id. Returns null when missing — caller decides
 *  whether to surface "playlist no longer exists" or just bounce back to
 *  the list. */
export function getPlaylist(id: string): Playlist | null {
  const all = readAll();
  return all.find((p) => p.id === id) ?? null;
}

/** Replace the tracks of a playlist. Used by the detail view for both
 *  reorder (drag & drop) and remove operations — both end up rewriting
 *  the whole `tracks` array, which keeps the storage path trivially
 *  atomic. No-op when the id doesn't exist (lets a stale UI race the
 *  delete-elsewhere case without crashing). */
export function setPlaylistTracks(id: string, tracks: SongRow[]): void {
  const all = readAll();
  const idx = all.findIndex((p) => p.id === id);
  if (idx < 0) return;
  all[idx] = { ...all[idx], tracks: tracks.slice() };
  writeAll(all);
}

/** Delete a playlist. No-op when the id doesn't exist (same race-safety
 *  reasoning as setPlaylistTracks). */
export function deletePlaylist(id: string): void {
  const all = readAll();
  const next = all.filter((p) => p.id !== id);
  if (next.length === all.length) return;
  writeAll(next);
}
