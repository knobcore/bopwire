// Routes between the wallet setup screens and the main app surface
// based on what's stored in localStorage. Mirrors the Dart
// `WalletGate` in
//   musicchain_player/lib/src/screens/wallet_gate.dart
// but reduced for the web build:
//
//   * No wallet blob on disk → first-launch (mnemonic create + persist).
//   * Wallet blob on disk      → login (user re-enters the mnemonic).
//   * Authed                   → home (handed off to opts.onAuthed).
//
// The Dart version also has an "auto-unlock from secure storage" branch
// that goes straight to home; we deliberately don't replicate that for
// the browser because the encrypted localStorage blob is keyed off the
// mnemonic itself, so unlock without re-entering the phrase isn't
// meaningful — that's exactly what the login screen does.
//
// State machine: loading | firstLaunch | login | home. Re-render is
// destructive — each transition clears `container` and the relevant
// screen renderer rewrites it. `loading` is shown briefly on boot and
// again during the reset path, so we re-check storage rather than
// assuming the reset succeeded.
//
// Pure DOM + template strings. No framework, no UI deps.

import type { Wallet } from '@/wallet';
import { storage } from '@/storage';
import { renderFirstLaunch } from './wallet_first_launch';
import { renderLogin } from './wallet_login';

/** Options the host passes when mounting the gate. */
export interface WalletGateOptions {
  /** Fired once the wallet is unlocked / created. The host is expected
   *  to tear down the gate's DOM (typically by re-rendering its own
   *  home shell into the same container) and take over from there. */
  onAuthed: (wallet: Wallet) => void;
}

type GateState = 'loading' | 'firstLaunch' | 'login' | 'home';

/** Escape a value for safe interpolation into innerHTML. Only used by
 *  the loading placeholder today but kept consistent with the other
 *  screens' defensive-escape style. */
function esc(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/**
 * Mount the wallet gate into `container`. Decides which screen to
 * show based on whether an encrypted wallet blob is present in
 * localStorage, re-renders on every state transition, and hands off
 * to `opts.onAuthed` exactly once when the wallet is ready.
 *
 * Idempotent against the underlying storage: the reset path explicitly
 * re-reads `storage.hasEncryptedWallet()` instead of jumping straight
 * to firstLaunch, so a partially-failed `storage.clear()` (or a fresh
 * blob written by another tab between transitions) gets noticed.
 */
export function renderWalletGate(
  container: HTMLElement,
  opts: WalletGateOptions,
): void {
  let state: GateState = 'loading';
  // Latch onAuthed so a stray re-render after a transition to `home`
  // can't fire the callback twice — the host owns the container after
  // we hand control off, and we shouldn't fight it for the DOM.
  let authedFired = false;

  const setState = (next: GateState): void => {
    state = next;
    render();
  };

  /** Read storage and route accordingly. Used on boot AND on the reset
   *  path so a "sign out" that for whatever reason left the encrypted
   *  blob behind doesn't strand the user on firstLaunch with a stale
   *  wallet still on disk. Async because the underlying IndexedDB
   *  backend has to open a connection to answer — on the localStorage
   *  fallback the await resolves on the next microtask. */
  const decide = async (): Promise<void> => {
    try {
      const hasSaved = await storage.hasEncryptedWallet();
      // Guard against a race where onAuthed fired before this resolved
      // (e.g. another tab finished a sign-in via storage events). Once
      // we've handed off to the host we shouldn't overwrite their DOM.
      if (state === 'home') return;
      setState(hasSaved ? 'login' : 'firstLaunch');
    } catch (e) {
      // Storage detection failed (private-mode iframe, neither backend
      // available, etc.). Surface inline so the user sees something
      // actionable instead of a blank loading card forever.
      if (state === 'home') return;
      renderStorageError(e);
    }
  };

  const renderStorageError = (err: unknown): void => {
    const msg = err instanceof Error ? err.message : String(err);
    container.innerHTML = `
      <div class="main-pane">
        <div class="card">
          <p class="err">${esc('Couldn\'t read wallet storage')}</p>
          <p class="muted mono">${esc(msg)}</p>
        </div>
      </div>`;
  };

  const renderLoading = (): void => {
    container.innerHTML = `
      <div class="main-pane">
        <div class="card">
          <p class="muted">${esc('Loading…')}</p>
        </div>
      </div>`;
  };

  const onAuthed = (wallet: Wallet): void => {
    if (authedFired) return;
    authedFired = true;
    // Flip to `home` so a subsequent stray render() call (e.g. from a
    // late async resolution inside one of the screens) becomes a no-op
    // — the gate has nothing more to draw.
    state = 'home';
    opts.onAuthed(wallet);
  };

  const render = (): void => {
    switch (state) {
      case 'loading':
        renderLoading();
        return;
      case 'firstLaunch':
        renderFirstLaunch(container, {
          onComplete: onAuthed,
        });
        return;
      case 'login':
        renderLogin(container, {
          onLoggedIn: onAuthed,
          onResetWallet: () => {
            // Don't jump straight to firstLaunch — mirror the Dart gate's
            // pattern of bouncing through `loading` and re-running the
            // decision tree. That way if storage.clear() didn't actually
            // wipe the blob (private-mode quota error, racing tab, etc.)
            // we land back on the login screen instead of stranding the
            // user on firstLaunch with a stale wallet underneath.
            setState('loading');
            void decide();
          },
        });
        return;
      case 'home':
        // Host owns the container after onAuthed; nothing to draw.
        return;
    }
  };

  // Boot: paint the placeholder so the user sees something immediately,
  // then route on the (async) storage read. On the IndexedDB backend the
  // open-database round-trip means we'll typically paint at least one
  // loading frame; on the localStorage fallback the resolution is fast
  // enough that no human sees it.
  render();
  void decide();
}
