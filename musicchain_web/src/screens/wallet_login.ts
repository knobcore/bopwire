// Login surface for the web player. Mirrors the Dart
// `WalletLoginScreen` in
//   musicchain_player/lib/src/screens/wallet_login_screen.dart
// but reduced to the only flow the browser actually supports: type the
// 12-word recovery phrase, validate it through libwally-WASM, derive
// the wallet, fire onLoggedIn(wallet).
//
// The web build has no "unlock with password" branch (yet) — the
// encrypted blob in localStorage is keyed off the mnemonic itself, so
// re-entering the phrase IS the unlock action. The Dart screen had a
// password field as a faster cold-start on a device that already had
// the wallet on disk; we can layer that on later by storing a second
// envelope under a user-chosen passphrase.
//
// The "sign out / use a different wallet" path clears localStorage and
// hands control back to the app shell so it can re-mount the first-
// launch flow. Defers to `onResetWallet` instead of doing the route
// swap inline so the shell stays in charge of navigation (same gotcha
// the Dart version's comments call out: blowing away the gate
// mid-callback drops the next state transition on the floor).
//
// Pure DOM + template-strings. No framework, no UI deps.

import { Wallet } from '@/wallet';
import { storage } from '@/storage';
import { getCachedUsername } from '@/username';

/** Options passed by the host (the app shell in main.ts). */
export interface LoginOptions {
  /** Fired once the mnemonic validates AND the wallet is derived. The
   *  host should navigate to the main app surface. */
  onLoggedIn: (wallet: Wallet) => void;
  /** Fired after the user confirms "sign out and start fresh" and we've
   *  wiped localStorage. The host swaps to the first-launch flow. */
  onResetWallet: () => void;
}

interface State {
  typed: string;
  busy: boolean;
  err: string;
}

/** Escape a value for safe interpolation into innerHTML. */
function esc(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/** Renders the wallet login screen into `container`, replacing any
 *  existing children. Re-render is destructive: each state change
 *  swaps innerHTML wholesale, same pattern as wallet_first_launch. */
export function renderLogin(
  container: HTMLElement,
  opts: LoginOptions,
): void {
  // Cached username is plain localStorage; safe to read synchronously.
  // `null` means we have no hint to show — render-time we just omit
  // the "stored wallet on this device" header in that case.
  const savedUsername = getCachedUsername() ?? '';

  const state: State = { typed: '', busy: false, err: '' };

  const render = (): void => {
    const header = savedUsername
      ? `
        <div class="card row" style="gap:12px;align-items:center;">
          <div style="
            width:40px;height:40px;border-radius:50%;
            background:var(--accent-2);color:var(--fg);
            display:grid;place-items:center;font-weight:600;">
            ${esc(savedUsername.charAt(0).toUpperCase() || '?')}
          </div>
          <div class="col" style="gap:2px;">
            <div style="font-weight:600;font-size:16px;">
              ${esc(savedUsername)}
            </div>
            <div class="muted">Stored wallet on this device</div>
          </div>
        </div>`
      : '';

    container.innerHTML = `
      <div class="main-pane col" style="gap:16px;">
        <h2>Sign in</h2>
        ${header}
        <div class="col" style="gap:8px;">
          <div style="font-weight:600;">Sign in with your recovery phrase</div>
          <p class="muted" style="margin:0;">
            Type the 12 words from any device that uses the same wallet.
            We derive the keypair locally; no part of the phrase leaves
            this browser.
          </p>
        </div>
        <textarea data-mnemonic rows="3"
          autocapitalize="off" autocorrect="off" spellcheck="false"
          placeholder="word1 word2 word3 …"
          ${state.busy ? 'disabled' : ''}>${esc(state.typed)}</textarea>
        ${state.err ? `<p class="err">${esc(state.err)}</p>` : ''}
        <div class="row">
          <span class="grow"></span>
          <button class="primary" data-act="signin" ${state.busy ? 'disabled' : ''}>
            ${state.busy ? 'Signing in…' : 'Sign in'}
          </button>
        </div>
        <hr style="
          border:0;border-top:1px solid var(--border);
          margin:24px 0 8px 0;width:100%;" />
        <div class="row">
          <button data-act="reset" ${state.busy ? 'disabled' : ''}
            style="color:var(--err);border-color:var(--err);">
            Sign out of this device and start fresh
          </button>
        </div>
      </div>`;

    const ta = container.querySelector<HTMLTextAreaElement>('[data-mnemonic]');
    ta?.addEventListener('input', () => {
      // Stash live so a re-render (e.g. for an error) preserves what
      // the user typed. Don't re-render on every keystroke.
      state.typed = ta.value;
    });
    // Submit on cmd/ctrl+Enter as a small ergonomic win for users
    // pasting the phrase from a password manager.
    ta?.addEventListener('keydown', (ev: KeyboardEvent) => {
      if ((ev.metaKey || ev.ctrlKey) && ev.key === 'Enter' && !state.busy) {
        ev.preventDefault();
        void signIn();
      }
    });
    if (!state.busy) ta?.focus();

    container.querySelector('[data-act="signin"]')?.addEventListener(
      'click',
      () => { void signIn(); },
    );
    container.querySelector('[data-act="reset"]')?.addEventListener(
      'click',
      () => { void resetWallet(); },
    );
  };

  // ---- Sign in --------------------------------------------------
  const signIn = async (): Promise<void> => {
    const mnemonic = state.typed.trim().toLowerCase().replace(/\s+/g, ' ');
    if (!mnemonic) {
      state.err = 'Type the 12 words from your recovery phrase.';
      render();
      return;
    }
    // Local checksum check first so we don't burn a WASM derivation
    // on something obviously wrong. Wallet.validate runs the BIP39
    // wordlist + checksum inside the same module Wallet.fromMnemonic
    // would use anyway, so this is cheap.
    let valid = false;
    try {
      valid = await Wallet.validate(mnemonic);
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      state.busy = false;
      state.err = `Couldn't validate phrase: ${msg}`;
      render();
      return;
    }
    if (!valid) {
      state.err = "That's not a valid 12-word recovery phrase.";
      render();
      return;
    }

    state.busy = true;
    state.err = '';
    render();
    try {
      const wallet = await Wallet.fromMnemonic(mnemonic);
      opts.onLoggedIn(wallet);
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      state.busy = false;
      state.err = `Restore failed: ${msg}`;
      render();
    }
  };

  // ---- Sign out / reset -----------------------------------------
  const resetWallet = async (): Promise<void> => {
    // Hard confirm. Wiping the encrypted blob is reversible from the
    // user's recovery phrase, but only if they have it written down —
    // surface the same warning the Dart player shows.
    const ok = window.confirm(
      'Sign out of this device?\n\n' +
      'Your wallet stays on the chain — you can sign back in with ' +
      'your 12-word recovery phrase. If you do NOT have those 12 ' +
      'words written down, you will permanently lose access to this ' +
      'wallet.',
    );
    if (!ok) return;
    // storage.clear() routes through the active backend (IndexedDB or
    // localStorage) AND the username helpers, so it's the single call
    // that fully wipes everything the previous session wrote. If it
    // throws (e.g. IndexedDB blocked), surface it as an inline error
    // and DON'T fire onResetWallet — the app would otherwise jump to
    // the first-launch flow with the old wallet still on disk.
    try {
      await storage.clear();
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      state.err = `Sign out failed: ${msg}`;
      render();
      return;
    }
    opts.onResetWallet();
  };

  render();
}
