// Post-login app shell. Mirrors the Dart `HomeScreen` in
//   musicchain_player/lib/src/screens/home_screen.dart
// but cut down for the web build:
//
//   * Tabs: Library / Search / Wallet / Settings (the Dart app also has
//     a "My Library" folder browser, which the browser doesn't have a
//     local-FS equivalent for — folded into the single "Library" tab).
//   * Persistent mini-player at the bottom (see widgets/mini_player.ts).
//   * Header strip with username + balance + online/offline indicator.
//
// Owns the long-lived runtime objects the rest of the app reads off of:
// the Player instance, a HeartbeatService factory the per-session play
// path can summon, the auto-refresh timer that mirrors the Dart
// WalletProvider's 20 s defensive tick, and the listener set wired to
// NodeClient + the player.
//
// Re-rendering individual tabs is destructive: each tab swap clears the
// main pane and the sub-screen's renderer rewrites it. The header and
// mini-player live OUTSIDE the swap zone so they stay mounted across
// navigations and don't lose subscription state.

import type { NodeClient } from '../node_client';
import type { Wallet } from '../wallet';
import { Player } from '../player';
import { HeartbeatService } from '../heartbeat';
import { getBalance, startSession, type SongRow } from '../verbs';
import { renderLibrary } from './library';
import { renderSearch } from './search';
import { renderSettings } from './settings';
import { renderWalletTab } from './wallet';
import { renderPlaylists } from './playlists';
import { getCachedUsername } from '../username';
import { Queue } from '../queue';
import { renderMiniPlayer, type Detach, type PlayerLike } from '../widgets/mini_player';
import { renderNowPlaying, type NowPlayingDetach } from './now_playing';
import { AudioBridge } from '../audio_bridge';

/** Context the host (main.ts) passes in once the wallet gate finishes.
 *  Both objects are already constructed; the home shell does not own
 *  their lifecycle and must not tear them down on tab swaps. */
export interface HomeCtx {
  wallet: Wallet;
  node:   NodeClient;
}

/** Tab identifiers — kept as a string union so a typo in the dispatch
 *  switch is a compile error rather than a silent fall-through. */
type TabId = 'library' | 'search' | 'playlists' | 'wallet' | 'settings';

const TAB_LABELS: Record<TabId, string> = {
  library:   'Library',
  search:    'Search',
  playlists: 'Playlists',
  wallet:    'Wallet',
  settings:  'Settings',
};

/** WalletProvider's defensive 20 s tick in the Dart player; we mirror
 *  the cadence so the chain-side play-mint that lands silently in the
 *  background (no session.complete UI surface today) gets picked up
 *  inside the user's current listening session. */
const BALANCE_REFRESH_MS = 20_000;

// ---------------------------------------------------------------------
// PlayerLike adapter
// ---------------------------------------------------------------------
//
// The Player class in ../player.ts owns the underlying HTMLAudioElement
// and dispatches 'state' / 'position' / 'ended' events, but it has no
// concept of "currentSong" or "playlist" — those are the chain-RPC layer
// up here in the shell. The mini-player widget wants a single
// PlayerLike object with the union of (transport, queue, metadata)
// methods, so we wrap the raw Player and stitch a tiny queue model in
// front of it. The same object is what the Search tab's row click hands
// the loaded song to via load() + startSession.
//
// The on/off pair forwards directly to the underlying Player's
// EventTarget surface — that's where 'state' and 'position' come from.

interface ShellPlayer extends PlayerLike {
  /** Underlying audio engine. Exposed so library can play local
   *  Blobs / pre-resolved URLs without going through the chain
   *  session/heartbeat path the SongRow flow requires. */
  readonly audio: Player;
  /** Queue model shared across screens. Artist / album / search /
   *  now-playing all push into this same instance so the user's
   *  cross-tab queue survives navigation (and reloads, via
   *  localStorage). */
  readonly queue: Queue;
  /** Live byte counters for the in-flight `audio.fetch` swarm pull, or
   *  `null` when nothing is being streamed. Updated by loadAndPlay's
   *  AudioBridge onProgress hook; consumed by the mini-player widget
   *  to render a "Buffering N%" affordance below the title. */
  readonly loadingProgress: { received: number; total: number } | null;
  /** Push `song` onto the queue as the now-playing track and start it.
   *  Kicks the chain session.start and arms a HeartbeatService against
   *  the returned session id. */
  loadAndPlay(song: SongRow, playerAddress: string): Promise<void>;
  /** Tear down the heartbeat for the current session — called on
   *  shell disposal so an orphaned interval doesn't outlive the page. */
  disposeHeartbeat(): void;
  /** Drop queue + Player subscriptions wired in buildShellPlayer.
   *  Idempotent. */
  dispose(): void;
}

function buildShellPlayer(
  node: NodeClient,
  onPlayComplete: () => void,
  playerAddressFn: () => string,
): ShellPlayer {
  const audio = new Player();
  const queue = new Queue();
  let heartbeat: HeartbeatService | null = null;

  // One AudioBridge per shell. The bridge installs hooks on the
  // NodeClient's binary-frame / unmatched-reply slots and demuxes the
  // mini-node's `audio.fetch` streams; we share it across every
  // loadAndPlay call so concurrent prefetches (when added later) can
  // ride the same demux registry instead of clobbering each other's
  // hook installations.
  const audioBridge = new AudioBridge(node);

  // The object URL handed back by the last successful fetchSong. The
  // Player class also tracks the URL it's currently consuming and
  // revokes it on its next load(), but we keep our own pointer so we
  // can revoke as soon as we pick a NEW song — even before audio.load
  // is reached — rather than waiting for Player to swap sources. This
  // matters when the user mashes "next track" faster than the fetch
  // can complete: we'd otherwise leak the half-fetched Blob URL.
  let prevBlobUrl: string | null = null;

  // Mutable view of the in-flight audio.fetch byte counters. Set by the
  // AudioBridge onProgress callback inside loadAndPlay; reset to null
  // when the fetch finishes (or fails). The mini-player widget reads
  // this via ctx.loadingProgress on every 'state' re-render so the user
  // sees "Buffering N%" tick up while a track is being pulled off the
  // swarm. Stays null at all other times so a stale value can't bleed
  // into a later track's loading affordance.
  let loadingProgress: { received: number; total: number } | null = null;
  // While the AudioBridge is pulling bytes, the underlying Player's
  // state is still whatever the previous track ended in (paused /
  // stopped / idle). We override the reported state to 'loading' for
  // the duration of the fetch so the mini-player's
  //   state === 'loading'
  // gate over the buffering affordance lights up immediately rather
  // than only flashing during audio.load()'s decode stage. Cleared as
  // soon as audio.load() runs because Player's own state machine then
  // owns the truth.
  let loadingOverride = false;

  // Restore the user's last queue from localStorage before any caller
  // can attach a 'change' listener — load() deliberately suppresses the
  // initial 'change' so the mini-player's first paint sees a stable
  // snapshot rather than two paints in a row (one empty, one rehydrated).
  queue.load();

  // Fan out a synthetic 'state' event on the underlying audio whenever
  // the queue mutates. The mini-player widget subscribes via
  // player.on('state', ...) which forwards to audio.addEventListener,
  // so any cross-tab queue push (artist/album/search add → mini-player
  // re-render) only needs the queue to whisper through this channel.
  const onQueueChange = (): void => {
    queue.save();
    audio.dispatchEvent(new CustomEvent('state', { detail: audio.state }));
  };
  queue.addEventListener('change', onQueueChange);

  // The mini-player only cares about state + position; we forward both
  // by name. We also intercept the 'ended' event so we can trigger the
  // shell-level onPlayComplete (which refreshes the balance after a mint
  // lands silently on the chain) and auto-advance to the next queued
  // track if one exists.
  audio.addEventListener('ended', () => {
    try { onPlayComplete(); } catch { /* swallow */ }
    // Auto-advance with repeat-aware semantics:
    //   * 'one' — restart the same track. Same SongRow, same session is
    //     OK since session.complete already landed; the next loadAndPlay
    //     opens a fresh session for the replay.
    //   * 'all' — advance to next; wrap to head of queue at the tail.
    //   * 'off' — advance if hasNext, otherwise stop.
    // The mini-player widget owns the canonical mode and mirrors it on
    // globalThis.__mcRepeatMode so we don't drag a stateful import
    // dependency into here.
    const mode = (globalThis as { __mcRepeatMode?: 'off' | 'all' | 'one' })
      .__mcRepeatMode ?? 'off';
    const playerAddress = playerAddressFn();
    if (mode === 'one') {
      const same = queue.current;
      if (same) void loadAndPlay(same, playerAddress);
      return;
    }
    if (queue.hasNext) {
      const nxt = queue.next();
      if (nxt) void loadAndPlay(nxt, playerAddress);
      return;
    }
    if (mode === 'all' && queue.length > 0) {
      // Tail of queue under repeat-all: wrap to index 0.
      const head = queue.tracks[0];
      if (head) {
        queue.playNow(head, 0);
        void loadAndPlay(head, playerAddress);
      }
    }
  });

  function on(event: 'state' | 'position', handler: () => void): void {
    audio.addEventListener(event, handler);
  }
  function off(event: 'state' | 'position', handler: () => void): void {
    audio.removeEventListener(event, handler);
  }

  async function loadAndPlay(s: SongRow, playerAddress: string): Promise<void> {
    // If the song isn't already the current queue head (e.g. an autoplay
    // advance just moved the cursor), splice it in via playNow. Skipping
    // the push when the cursor already points at this track keeps the
    // queue from growing duplicate tails on repeated taps.
    if (queue.current?.content_hash !== s.content_hash) {
      queue.playNow(s);
    }

    // Tear down any previous heartbeat before opening the new session
    // so we don't double-fire session.complete in a race.
    if (heartbeat) {
      void heartbeat.stop();
      heartbeat = null;
    }

    // Drop the URL of the song we were playing before — the new fetch
    // is about to allocate a fresh Blob, and the old one's bytes are
    // no longer reachable through the Player. We revoke EAGERLY rather
    // than wait for the next audio.load() because the fetchSong below
    // can take seconds; if the user spams next, the in-progress fetch
    // would otherwise leak its Blob URL even though we abandoned it.
    if (prevBlobUrl !== null) {
      URL.revokeObjectURL(prevBlobUrl);
      prevBlobUrl = null;
    }

    // Repaint the mini-player into a loading affordance while the
    // fetch runs. The widget reads `player.state` direct from the
    // audio engine, which still says whatever the last source ended
    // in (paused / stopped / idle) — without this nudge the user
    // sees no feedback until audio.load() flips state to 'loading'
    // after the fetch completes. We dispatch the synthetic 'state'
    // event so the widget re-renders; the audio engine's underlying
    // state stays consistent and only flips to 'loading' for real
    // when audio.load() runs below.
    audio.dispatchEvent(new CustomEvent('state', { detail: 'loading' }));

    // Pull the song bytes over the mini-node WS gateway. Progress
    // ticks are folded into the same synthetic state nudge so the
    // mini-player's loading affordance stays "fresh" rather than
    // looking frozen on a slow connection. We don't expose a real
    // progress bar today — the existing 'loading' state already dims
    // the mini-player + replaces the play button with '…', which is
    // the same affordance the Dart side shows during stream open.
    const fetched = await audioBridge.fetchSong(s.content_hash, {
      onProgress: (_received, _total) => {
        audio.dispatchEvent(new CustomEvent('state', { detail: 'loading' }));
      },
    });
    prevBlobUrl = fetched.url;

    // audio.load(url) will revoke its own internal objectUrl (if any)
    // and start buffering the new Blob via the URL the bridge built.
    // We pass the URL string rather than the Blob so Player doesn't
    // build a duplicate object URL — the one fetchSong returned is
    // the canonical handle the bridge wants us to manage.
    await audio.load(fetched.url);
    await audio.play();

    // session.start can fail (chain unreachable, content not registered);
    // we still let the audio play so the user gets feedback, but skip
    // the heartbeat — without a session id there's nothing to ping.
    try {
      const session = await startSession(node, s.content_hash, playerAddress);
      if (session.session_id) {
        heartbeat = new HeartbeatService({
          node,
          player: audio,
          sessionId:       session.session_id,
          contentHash:     s.content_hash,
          playerAddress,
          songDurationMs:  s.duration_ms ?? audio.durationMs,
        });
      }
    } catch (err) {
      // eslint-disable-next-line no-console
      console.warn('[home] session.start failed:', err);
    }
  }

  function disposeHeartbeat(): void {
    if (heartbeat) {
      void heartbeat.stop();
      heartbeat = null;
    }
  }

  async function togglePlayPause(): Promise<void> {
    if (audio.state === 'playing') audio.pause();
    else if (queue.current) await audio.play();
  }

  async function playNext(): Promise<void> {
    const nxt = queue.next();
    if (!nxt) return;
    await loadAndPlay(nxt, playerAddressFn());
  }

  async function playPrev(): Promise<void> {
    const prv = queue.prev();
    if (!prv) return;
    await loadAndPlay(prv, playerAddressFn());
  }

  let disposed = false;
  function dispose(): void {
    if (disposed) return;
    disposed = true;
    queue.removeEventListener('change', onQueueChange);
    disposeHeartbeat();
    // Drop the AudioBridge's hook installations on the NodeClient and
    // reject any fetch still mid-stream. Safe to call even when no
    // fetch is in flight.
    try { audioBridge.dispose(); } catch { /* swallow */ }
    if (prevBlobUrl !== null) {
      URL.revokeObjectURL(prevBlobUrl);
      prevBlobUrl = null;
    }
  }

  return {
    audio,
    queue,
    // Synthesise the "loading" state while the AudioBridge is fetching
    // bytes from the swarm — Player itself only flips to 'loading' once
    // the Blob is in hand and decode starts. loadingOverride keeps the
    // mini-player on a buffering affordance during the fetch.
    get state()         { return loadingOverride ? 'loading' : audio.state; },
    get currentSong()   { return queue.current; },
    get positionMs()    { return audio.positionMs; },
    get durationMs()    { return audio.durationMs; },
    get playlist()      { return queue.tracks; },
    get playlistIndex() { return queue.index; },
    get loadingProgress() { return loadingProgress; },
    on,
    off,
    togglePlayPause,
    playNext,
    playPrev,
    seek: (ms: number) => audio.seek(ms),
    loadAndPlay,
    disposeHeartbeat,
    dispose,
  };
}

// ---------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------
//
// The header carries the user-identity bits (username, balance) and the
// tab strip. Online/offline state is a small colored dot — mirrors the
// Dart _ConnectionBanner but trimmed: the browser only ever talks to
// ONE home node over ONE WebSocket, so the "N nodes · M peers" detail
// the Dart side shows would be misleading. A single online/offline tag
// is what the WebSocket actually tells us.

interface HeaderHandle {
  root: HTMLElement;
  setOnline(online: boolean): void;
  setBalance(balance: string): void;
  setActiveTab(tab: TabId): void;
}

function fmtBalance(raw: string): string {
  // The chain ships balances as a decimal string of microMC (8 decimals).
  // Anything we can't parse is rendered as a literal — better than
  // hiding the value behind an "—" placeholder when the home node
  // returns e.g. "0".
  if (!raw) return '0.00000000';
  const dot = raw.indexOf('.');
  if (dot < 0) {
    if (raw === '0') return '0.00000000';
    return raw;
  }
  return raw;
}

function buildHeader(
  username: string,
  onTabChange: (tab: TabId) => void,
): HeaderHandle {
  const root = document.createElement('header');
  root.className = 'col';
  root.style.cssText =
    'background: var(--bg-soft); border-bottom: 1px solid var(--border);' +
    'padding: 0;';

  // Top strip: identity (left), balance (centre), online dot (right).
  const top = document.createElement('div');
  top.className = 'row';
  top.style.cssText = 'padding: 10px 16px; gap: 12px;';

  const identity = document.createElement('div');
  identity.className = 'row';
  identity.style.cssText = 'gap: 8px; align-items: center;';

  const avatar = document.createElement('div');
  avatar.style.cssText =
    'width: 28px; height: 28px; border-radius: 50%; background: var(--accent-2);' +
    'color: var(--fg); display: grid; place-items: center; font-weight: 600;' +
    'font-size: 13px;';
  avatar.textContent = (username.charAt(0) || '?').toUpperCase();

  const handle = document.createElement('div');
  handle.style.cssText = 'font-weight: 600;';
  handle.textContent = username || 'anon';

  identity.append(avatar, handle);

  const balanceWrap = document.createElement('div');
  balanceWrap.className = 'row grow';
  balanceWrap.style.cssText = 'justify-content: center; gap: 6px;';
  const balanceLabel = document.createElement('span');
  balanceLabel.className = 'muted';
  balanceLabel.textContent = 'MC';
  const balanceValue = document.createElement('span');
  balanceValue.className = 'mono';
  balanceValue.textContent = '0.00000000';
  balanceWrap.append(balanceValue, balanceLabel);

  const status = document.createElement('div');
  status.className = 'row';
  status.style.cssText = 'gap: 6px; align-items: center;';
  const statusDot = document.createElement('span');
  statusDot.style.cssText =
    'width: 8px; height: 8px; border-radius: 50%; background: var(--ok);' +
    'display: inline-block;';
  const statusText = document.createElement('span');
  statusText.className = 'muted';
  statusText.style.fontSize = '12px';
  statusText.textContent = 'online';
  status.append(statusDot, statusText);

  top.append(identity, balanceWrap, status);

  // Tab strip.
  const tabRow = document.createElement('nav');
  tabRow.className = 'tab-row';
  tabRow.setAttribute('role', 'tablist');

  const tabButtons: Partial<Record<TabId, HTMLElement>> = {};
  (Object.keys(TAB_LABELS) as TabId[]).forEach((tab) => {
    const btn = document.createElement('div');
    btn.className = 'tab';
    btn.textContent = TAB_LABELS[tab];
    btn.setAttribute('role', 'tab');
    btn.setAttribute('data-tab', tab);
    btn.tabIndex = 0;
    btn.addEventListener('click', () => onTabChange(tab));
    btn.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter' || ev.key === ' ') {
        ev.preventDefault();
        onTabChange(tab);
      }
    });
    tabButtons[tab] = btn;
    tabRow.appendChild(btn);
  });

  root.append(top, tabRow);

  return {
    root,
    setOnline(online: boolean): void {
      statusDot.style.background = online ? 'var(--ok)' : 'var(--err)';
      statusText.textContent = online ? 'online' : 'offline';
    },
    setBalance(balance: string): void {
      balanceValue.textContent = fmtBalance(balance);
    },
    setActiveTab(tab: TabId): void {
      for (const t of Object.keys(tabButtons) as TabId[]) {
        const el = tabButtons[t];
        if (!el) continue;
        if (t === tab) el.classList.add('active');
        else           el.classList.remove('active');
      }
    },
  };
}

// ---------------------------------------------------------------------
// renderHome
// ---------------------------------------------------------------------

/** Mount the post-login app shell into `container`. Clears whatever
 *  the gate (or any prior render) left behind, builds the persistent
 *  header + main-pane + mini-player, and wires the long-lived
 *  subscriptions (node disconnect, balance auto-refresh, play complete). */
export function renderHome(container: HTMLElement, ctx: HomeCtx): void {
  container.replaceChildren();

  // -- Structural DOM ------------------------------------------------
  // Three vertical regions: header / main-pane / mini-player. Header
  // and mini-player are persistent; main-pane gets swapped on tab
  // change. The wrapper is column-flex so the mini-player sticks to
  // the bottom of the viewport even when the main pane is short.
  const shell = document.createElement('div');
  shell.className = 'col';
  shell.style.cssText = 'min-height: 100vh;';

  // Shell-level state.
  let activeTab: TabId = 'library';
  const username = getCachedUsername() ?? '';

  const header = buildHeader(username, (tab) => {
    if (tab === activeTab) return;
    activeTab = tab;
    header.setActiveTab(tab);
    renderActiveTab();
  });

  const main = document.createElement('main');
  main.className = 'col grow';
  main.setAttribute('role', 'main');

  const miniPlayerSlot = document.createElement('div');
  miniPlayerSlot.style.cssText = 'position: sticky; bottom: 0;';

  shell.append(header.root, main, miniPlayerSlot);
  container.appendChild(shell);

  // -- Balance auto-refresh -----------------------------------------
  //
  // refreshBalance is invoked on three triggers:
  //   1. Mount (so the header shows something other than 0 on first
  //      paint when the wallet has on-chain funds already).
  //   2. The 20 s defensive tick.
  //   3. Immediately after every play complete — the chain emits a
  //      MintTx that lands silently on the home node; the Dart side
  //      mirrors this via WalletProvider.refreshNow().
  //
  // Errors are swallowed: a transient RPC failure shouldn't blank
  // out a balance the user can already see.

  async function refreshBalance(): Promise<void> {
    try {
      const balance = await getBalance(ctx.node, ctx.wallet.address);
      header.setBalance(balance);
    } catch (err) {
      // eslint-disable-next-line no-console
      console.debug('[home] refreshBalance failed:', err);
    }
  }

  const balanceTimer = window.setInterval(() => {
    void refreshBalance();
  }, BALANCE_REFRESH_MS);
  void refreshBalance();

  // -- Player + heartbeat -------------------------------------------
  //
  // The shell owns the long-lived Player instance + queue model. The
  // search tab's row click drives loadAndPlay via the SearchCtx
  // contract; the mini-player widget reads state/position straight off
  // the same object. onPlayComplete fires the immediate balance
  // refresh — same trigger order as Dart's PlayerProvider._onComplete.

  const player = buildShellPlayer(
    ctx.node,
    () => { void refreshBalance(); },
    // playerAddressFn — read lazily so a future wallet rotation lands
    // without rebuilding the shell. The address is also stable today,
    // so the indirection is essentially free.
    () => ctx.wallet.address,
  );

  // -- Online / offline indicator -----------------------------------
  //
  // NodeClient exposes onConnect / onDisconnect as singular fields.
  // We compose with whatever the host (main.ts) wired before mounting
  // by saving the previous handlers and chaining ours; that way the
  // shell's listeners don't blow away the bootstrap's logging.

  const prevConnect    = ctx.node.onConnect;
  const prevDisconnect = ctx.node.onDisconnect;
  ctx.node.onConnect = () => {
    header.setOnline(true);
    // Re-fetch the balance when we come back online — the user may
    // have racked up plays during the outage and the home node's
    // current value is now the source of truth.
    void refreshBalance();
    if (prevConnect) {
      try { prevConnect(); } catch { /* swallow */ }
    }
  };
  ctx.node.onDisconnect = (reason: string) => {
    header.setOnline(false);
    if (prevDisconnect) {
      try { prevDisconnect(reason); } catch { /* swallow */ }
    }
  };
  // Initial state mirror — if we already connected before the shell
  // mounted, NodeClient won't fire onConnect again. Reflect the
  // current readyState on the dot.
  header.setOnline(ctx.node.isConnected);

  // -- Mini-player + now-playing overlay ----------------------------
  //
  // The mini-player stays mounted in the bottom slot. Tapping its
  // meta area opens the full-screen now-playing overlay, which we
  // host in a dedicated layer parented to the shell wrapper (so it
  // covers header + main + mini in one go without escaping the
  // host's mount point — important for hosts that mount into a
  // scoped element rather than document.body).
  const nowPlayingLayer = document.createElement('div');
  nowPlayingLayer.className = 'now-playing-layer';
  // The layer itself is just a positioned container; visibility is
  // driven by whether it has any children, so an empty layer is a
  // no-op visually.
  shell.appendChild(nowPlayingLayer);

  let nowPlayingDetach: NowPlayingDetach | null = null;

  function closeNowPlaying(): void {
    if (!nowPlayingDetach) return;
    try { nowPlayingDetach(); } catch { /* swallow */ }
    nowPlayingDetach = null;
  }

  function openNowPlaying(): void {
    // Re-tapping the mini-player while the overlay is up is a no-op;
    // the overlay already has the user's attention.
    if (nowPlayingDetach) return;
    nowPlayingDetach = renderNowPlaying(nowPlayingLayer, {
      player,
      node: ctx.node,
      onClose: () => closeNowPlaying(),
    });
  }

  let miniDetach: Detach | null = null;
  miniDetach = renderMiniPlayer(miniPlayerSlot, {
    player,
    node: ctx.node,
    onOpenNowPlaying: openNowPlaying,
  });

  // -- Tab rendering ------------------------------------------------
  //
  // Each tab gets its own context narrowed to what it actually uses;
  // none of them know about the shell-level state directly. Re-render
  // is destructive: we wipe `main` and the screen renderer rewrites
  // it. The header + mini-player live outside `main` and are unaffected.

  function renderActiveTab(): void {
    switch (activeTab) {
      case 'library':
        // The Discover-shell library hands the player SongRows straight
        // from songs.list — no Blob / string variants. The shell's
        // loadAndPlay does swarm URL resolution, session.start, and
        // heartbeat arming for us.
        renderLibrary(main, {
          wallet: { address: ctx.wallet.address },
          node:   ctx.node,
          player: {
            load: (song) => {
              void player.loadAndPlay(song, ctx.wallet.address);
            },
          },
          onError: (err) => {
            // eslint-disable-next-line no-console
            console.error('[home] library error:', err);
          },
        });
        return;
      case 'search':
        renderSearch(main, {
          node: ctx.node,
          player: {
            // Search hands us a SongRow; the shell's player turns that
            // into stream URL + session.start + heartbeat. We split
            // load+session into the two callbacks search.ts expects so
            // a future renderer can opt out of the chain session for
            // local previews.
            load: (song) => {
              void player.loadAndPlay(song, ctx.wallet.address);
            },
          },
          session: {
            // No-op: loadAndPlay already calls session.start under the
            // hood, so doing it again here would double-mint. Search's
            // contract just requires the call to be safe.
            start: () => { /* handled inside load() */ },
          },
          onError: (err) => {
            // eslint-disable-next-line no-console
            console.error('[home] search error:', err);
          },
        });
        return;
      case 'playlists':
        renderPlaylists(main, {
          node:   ctx.node,
          player: {
            // The playlists screen pushes onto the shared queue and
            // calls loadAndPlay for the first track of a playlist
            // press-to-play. Reuse the SongRow path so the chain
            // session.start + heartbeat get armed exactly like search.
            load: (song: SongRow) => {
              void player.loadAndPlay(song, ctx.wallet.address);
            },
          },
        });
        return;
      case 'wallet':
        renderWalletTab(main, {
          wallet: {
            address: ctx.wallet.address,
            sign:    (data: Uint8Array) => ctx.wallet.sign(data),
            // The wallet WASM doesn't surface the mnemonic re-read flow;
            // a future task wires this to the storage.loadEncrypted path.
            readMnemonic: async (_password: string) => {
              throw new Error('Mnemonic readback not implemented on web yet');
            },
            // Same shape as the Dart sign-out: clear local state and
            // bounce back to the gate. The gate watches storage and
            // re-mounts itself on its own.
            clearLocalWallet: async () => {
              try { localStorage.clear(); } catch { /* swallow */ }
            },
          },
          node: {
            request: (method, params) => ctx.node.request(method, params),
          },
          onSignOut: () => {
            // Tear down shell-owned subscriptions before handing
            // control back to the gate — leaving the timer + node
            // handlers wired into a teardown container leaks both.
            dispose();
            // Reload to re-run main.ts → gate. Cleaner than trying to
            // mount the gate ourselves; the bootstrap path already
            // knows how to read the now-cleared storage.
            location.reload();
          },
          onBalanceChange: (balance) => {
            // The wallet tab does its own refresh on a 20 s cadence;
            // mirror the value into the header so the user doesn't see
            // two stale numbers diverge.
            header.setBalance(balance);
          },
        });
        return;
      case 'settings':
        renderSettings(main, {
          wallet: ctx.wallet,
          node:   ctx.node,
        });
        return;
    }
  }

  // -- Teardown helper ----------------------------------------------
  //
  // Not part of the public surface — the host (main.ts) only mounts
  // the shell once per session and reloads the page on sign-out — but
  // wallet sign-out wants the same path, and tests can drive it via
  // the disposal hook on the container. Idempotent.

  let disposed = false;
  function dispose(): void {
    if (disposed) return;
    disposed = true;
    window.clearInterval(balanceTimer);
    // Tear the overlay down before the mini-player so we don't leave
    // a now-playing surface bound to a player whose subscriptions are
    // about to drop.
    closeNowPlaying();
    if (miniDetach) {
      try { miniDetach(); } catch { /* swallow */ }
      miniDetach = null;
    }
    // dispose() folds in disposeHeartbeat() and drops the queue
    // 'change' subscription so the persistence side-effect doesn't
    // outlive the shell.
    player.dispose();
    // Restore the original node handlers so the bootstrap's logging
    // (and any other module that wired itself in before us) still works
    // after the shell goes away.
    ctx.node.onConnect    = prevConnect;
    ctx.node.onDisconnect = prevDisconnect;
  }

  // Expose teardown so a future router / sign-out path that doesn't
  // reload the page can drive it without reaching into module
  // internals. Hidden under a Symbol so it doesn't show up as a
  // regular DOM attribute.
  (container as unknown as { __mcHomeDispose?: () => void }).__mcHomeDispose = dispose;

  // -- Initial paint ------------------------------------------------
  header.setActiveTab(activeTab);
  renderActiveTab();
}
