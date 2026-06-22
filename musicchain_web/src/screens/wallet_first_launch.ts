// Three-step wallet setup that runs on first launch (or whenever the
// player can't find a stored wallet in localStorage). Mirrors the Dart
// `WalletFirstLaunchScreen` in
//   musicchain_player/lib/src/screens/wallet_first_launch_screen.dart
// but kept much simpler: the web build only does the create-new flow
// (no "restore" branch — pasting a phrase into the unlock screen lands
// the same place), and there is no live "discovery banner" because the
// browser talks to the home node over a single WebSocket that's either
// up or it isn't.
//
// Steps:
//   1. Generate a 12-word BIP39 mnemonic and show it as a 4×3 grid.
//      The user copies it / saves it to a .txt before continuing.
//   2. Have the user re-type the phrase into a textarea so we know
//      they actually wrote it down. Validate against the BIP39
//      wordlist + checksum before letting them past.
//   3. Pick an optional username, derive the wallet, persist via
//      storage.saveEncrypted using the mnemonic as the passphrase, and
//      fire onComplete(wallet) so the app shell can swap in the main UI.
//
// Pure DOM + template-strings. No framework, no UI deps.

import { Wallet } from '@/wallet';
import { storage } from '@/storage';

/** Options passed by the host (the app shell in main.ts). */
export interface FirstLaunchOptions {
  /** Fired once the wallet is derived AND persisted. The host should
   *  navigate to the main app surface; the screen itself does no DOM
   *  cleanup beyond what the next render replaces. */
  onComplete: (wallet: Wallet) => void;
}

type Step =
  | { kind: 'showSeed';       mnemonic: string }
  | { kind: 'confirmSeed';    mnemonic: string; typed: string; err: string }
  | { kind: 'pickUsername';   mnemonic: string; username: string; busy: boolean; err: string };

/** Escape a value for safe interpolation into innerHTML. We only ever
 *  drop in mnemonic words and usernames, but escape defensively. */
function esc(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/** Trigger a Blob download of the recovery-phrase .txt without
 *  navigating away from the page. */
function downloadMnemonic(mnemonic: string): void {
  const body =
    `${mnemonic}\n\n` +
    `Keep this file offline. Anyone with these 12 words can spend ` +
    `from your wallet.\n`;
  const blob = new Blob([body], { type: 'text/plain;charset=utf-8' });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement('a');
  a.href     = url;
  a.download = 'musicchain-recovery-phrase.txt';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  // Give the browser a beat to actually start the download before we
  // revoke the URL; otherwise Safari sometimes saves a 0-byte file.
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** Async copy-to-clipboard with a graceful textarea fallback for the
 *  occasional Firefox/permissions edge case. */
async function copyToClipboard(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // Falls through to the legacy execCommand path.
  }
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    return ok;
  } catch {
    return false;
  }
}

/** Renders the wallet first-launch flow into `container`, replacing
 *  any existing children. Re-render is destructive: each step swaps
 *  innerHTML wholesale. */
export function renderFirstLaunch(
  container: HTMLElement,
  opts: FirstLaunchOptions,
): void {
  // We don't know the mnemonic until WASM resolves; show a placeholder
  // and kick off the generate.
  let state: Step | null = null;
  let toastTimer: number | null = null;

  const showToast = (msg: string): void => {
    const t = container.querySelector<HTMLElement>('[data-toast]');
    if (!t) return;
    t.textContent = msg;
    t.style.opacity = '1';
    if (toastTimer !== null) window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => {
      t.style.opacity = '0';
    }, 2500);
  };

  const render = (): void => {
    if (!state) {
      container.innerHTML = `
        <div class="main-pane">
          <div class="card">
            <p class="muted">Generating your recovery phrase…</p>
          </div>
        </div>`;
      return;
    }
    switch (state.kind) {
      case 'showSeed':     renderShowSeed(state);     break;
      case 'confirmSeed':  renderConfirmSeed(state);  break;
      case 'pickUsername': renderPickUsername(state); break;
    }
  };

  // ---- Step 1: show the 12 words --------------------------------
  const renderShowSeed = (s: Extract<Step, { kind: 'showSeed' }>): void => {
    const words = s.mnemonic.split(' ');
    const grid = words
      .map(
        (w, i) => `
          <div class="card mono" style="padding:10px 12px;text-align:left;">
            <span class="muted">${i + 1}.</span> ${esc(w)}
          </div>`,
      )
      .join('');
    container.innerHTML = `
      <div class="main-pane col" style="gap:16px;">
        <h2>Save your 12 words</h2>
        <p class="muted">
          This phrase is the only way to recover your wallet on another
          device. Don't screenshot it — write it down or save the file
          to encrypted storage.
        </p>
        <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px;">
          ${grid}
        </div>
        <div class="row">
          <button data-act="copy">Copy to clipboard</button>
          <button data-act="save">Save to file</button>
          <span class="grow"></span>
          <button class="primary" data-act="next">I've written it down</button>
        </div>
        <div data-toast class="muted" style="
          transition:opacity 0.3s;opacity:0;min-height:1.2em;"></div>
      </div>`;

    container.querySelector('[data-act="copy"]')?.addEventListener(
      'click',
      () => {
        void copyToClipboard(s.mnemonic).then((ok) =>
          showToast(ok ? 'Copied to clipboard.' : 'Copy failed.'),
        );
      },
    );
    container.querySelector('[data-act="save"]')?.addEventListener(
      'click',
      () => downloadMnemonic(s.mnemonic),
    );
    container.querySelector('[data-act="next"]')?.addEventListener(
      'click',
      () => {
        state = { kind: 'confirmSeed', mnemonic: s.mnemonic, typed: '', err: '' };
        render();
      },
    );
  };

  // ---- Step 2: re-type the phrase ------------------------------
  const renderConfirmSeed = (s: Extract<Step, { kind: 'confirmSeed' }>): void => {
    container.innerHTML = `
      <div class="main-pane col" style="gap:16px;">
        <h2>Type the words back</h2>
        <p class="muted">
          Paste or retype the 12 words to confirm you have them saved.
          We check the BIP39 checksum locally — no network call.
        </p>
        <textarea data-typed rows="3"
          autocapitalize="off" autocorrect="off" spellcheck="false"
          placeholder="word1 word2 word3 …">${esc(s.typed)}</textarea>
        ${s.err ? `<p class="err">${esc(s.err)}</p>` : ''}
        <div class="row">
          <button data-act="back">Back</button>
          <span class="grow"></span>
          <button class="primary" data-act="next">Continue</button>
        </div>
      </div>`;

    const ta = container.querySelector<HTMLTextAreaElement>('[data-typed]');
    ta?.addEventListener('input', () => {
      // Stash live so a re-render (e.g. for an error) preserves what
      // the user already typed. Don't re-render on every keystroke.
      if (state && state.kind === 'confirmSeed') state.typed = ta.value;
    });
    ta?.focus();

    container.querySelector('[data-act="back"]')?.addEventListener('click', () => {
      state = { kind: 'showSeed', mnemonic: s.mnemonic };
      render();
    });
    container.querySelector('[data-act="next"]')?.addEventListener('click', () => {
      const entered = (ta?.value ?? '').trim().toLowerCase().replace(/\s+/g, ' ');
      if (entered !== s.mnemonic) {
        state = {
          kind: 'confirmSeed',
          mnemonic: s.mnemonic,
          typed: ta?.value ?? '',
          err: 'That doesn\'t match. Check word order and spelling.',
        };
        render();
        return;
      }
      state = {
        kind:     'pickUsername',
        mnemonic: s.mnemonic,
        username: '',
        busy:     false,
        err:      '',
      };
      render();
    });
  };

  // ---- Step 3: optional username + create ----------------------
  const renderPickUsername = (
    s: Extract<Step, { kind: 'pickUsername' }>,
  ): void => {
    container.innerHTML = `
      <div class="main-pane col" style="gap:16px;">
        <h2>Pick a username (optional)</h2>
        <p class="muted">
          Username is optional — leave blank to be visible only by your
          wallet address. Your 12-word recovery phrase is the only thing
          you need to sign back in on another device, no password.
        </p>
        <input data-username type="text"
          autocapitalize="off" autocorrect="off" spellcheck="false"
          placeholder="3–30 chars, a–z 0–9 _"
          value="${esc(s.username)}"
          ${s.busy ? 'disabled' : ''} />
        ${s.err ? `<p class="err">${esc(s.err)}</p>` : ''}
        <div class="row">
          <span class="grow"></span>
          <button class="primary" data-act="create" ${s.busy ? 'disabled' : ''}>
            ${s.busy ? 'Setting up…' : 'Create account'}
          </button>
        </div>
      </div>`;

    const input = container.querySelector<HTMLInputElement>('[data-username]');
    input?.addEventListener('input', () => {
      if (state && state.kind === 'pickUsername') state.username = input.value;
    });
    input?.focus();

    container.querySelector('[data-act="create"]')?.addEventListener(
      'click',
      () => {
        void finish((input?.value ?? '').trim());
      },
    );
  };

  // ---- Finish ---------------------------------------------------
  const finish = async (rawUsername: string): Promise<void> => {
    if (!state || state.kind !== 'pickUsername') return;
    const mnemonic = state.mnemonic;
    const username = rawUsername;
    state = {
      kind:     'pickUsername',
      mnemonic,
      username,
      busy:     true,
      err:      '',
    };
    render();
    try {
      const wallet = await Wallet.fromMnemonic(mnemonic);
      // First-launch == no password. The mnemonic doubles as the
      // passphrase so the user can sign back in on another device with
      // just the recovery phrase. The unlock screen prompts for "the
      // 12 words or a custom password" in that order.
      await storage.saveEncrypted(
        username ? { mnemonic, username } : { mnemonic },
        mnemonic,
      );
      storage.cacheUsername(username);
      opts.onComplete(wallet);
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      state = {
        kind:     'pickUsername',
        mnemonic,
        username,
        busy:     false,
        err:      `Wallet setup failed: ${msg}`,
      };
      render();
    }
  };

  // Initial render — placeholder — then kick off mnemonic generation.
  render();
  void Wallet.generateMnemonic().then(
    (mnemonic: string) => {
      state = { kind: 'showSeed', mnemonic };
      render();
    },
    (e: unknown) => {
      const msg = e instanceof Error ? e.message : String(e);
      container.innerHTML = `
        <div class="main-pane">
          <div class="card">
            <p class="err">Couldn't generate a recovery phrase: ${esc(msg)}</p>
            <p class="muted">Reload the page to try again.</p>
          </div>
        </div>`;
    },
  );
}
