/*
 * Web player wallet tab. Mirrors the Flutter `WalletScreen` in
 * musicchain_player/lib/src/screens/wallet_screen.dart:
 *
 *   - EIP-55 address card with copy button + QR
 *   - Balance card with refresh + auto-refresh hook
 *   - Send-tokens dialog (builds + signs a TransferTx preimage and
 *     posts via node.request('wallet.transfer', ...))
 *   - Show-recovery-phrase panel (re-fetches mnemonic from storage)
 *   - Sign-out (wipes the local wallet + invokes onSignOut callback)
 *
 * The chain-side preimage format MUST match
 * musicchain/src/core/transaction.cpp::TransferTx::sign_message:
 *   u32 LE chain_id(19779) || from(20) || to(20) || amount(8 LE) || nonce(8 LE)
 *
 * Doing this in plain JS keeps the wallet UI usable even if the chain-
 * core wasm module isn't wired up yet; the only call into native code
 * is `wallet.sign(bytes)` which lands on `mc_web_wallet_sign` and runs
 * the same secp256k1 backend as the desktop / Android players.
 */

import { qrcodeSVG } from '../vendor/qrcode.js';

// ---------------------------------------------------------------------
// External interfaces. These are kept narrow so the wallet screen can
// be unit-tested with fakes and so the eventual full app shell only
// needs to satisfy this surface.
// ---------------------------------------------------------------------

/** Wallet handle backed by the libwally WASM module (wasm/wallet.js).
 *  All methods are synchronous against the WASM heap; only `readMnemonic`
 *  is async because it re-prompts the user for their password and the
 *  underlying secure-storage API is promise-based. */
export interface WalletHandle {
  /** EIP-55-checksummed address (0x + 40 hex). */
  address: string;
  /** ECDSA-sign sha256(data) with the wallet's secp256k1 key. Returns
   *  128-char compact (r||s) hex — same shape mc_web_wallet_sign
   *  produces, same shape the home node's verify path expects. */
  sign(data: Uint8Array): string;
  /** Re-fetch the BIP39 mnemonic from local secure storage. Throws if
   *  the user-supplied password is wrong or no mnemonic is stored.
   *  The web shell wires this to a re-prompt dialog so the wallet
   *  screen doesn't need to know how secret material is gated. */
  readMnemonic(password: string): Promise<string>;
  /** Wipe the in-memory key and the stored mnemonic. Idempotent. */
  clearLocalWallet(): Promise<void>;
}

/** Thin RPC wrapper. Methods are named so the chain-RPC fallback to a
 *  generic `request('method.name', params)` works regardless of whether
 *  the higher-level helpers are implemented yet. */
export interface NodeHandle {
  /** Generic JSON-RPC call. Mirrors NodeClient._rpc in the Dart player.
   *  The chain side dispatches on the method name; payload is a flat
   *  object the server can re-key as it likes. */
  request<T = unknown>(method: string, params: Record<string, unknown>): Promise<T>;
}

export interface WalletTabCtx {
  wallet: WalletHandle;
  node:   NodeHandle;
  /** Called after the user confirms "wipe wallet". The shell is
   *  responsible for routing back to first-launch UI. */
  onSignOut(): void | Promise<void>;
  /** Optional listener that the balance card calls every time the
   *  balance string changes (initial load + refresh + auto-refresh).
   *  Useful for the top-bar balance pill the player draws elsewhere. */
  onBalanceChange?(balance: string): void;
}

// ---------------------------------------------------------------------
// Chain constants — duplicated here because the chain-core wasm module
// isn't ready yet. When/if we wire that in, swap these for an import.
// ---------------------------------------------------------------------
const MC_CHAIN_ID = 19779; // 0x4D43 — "MC". See musicchain/src/core/transaction.h
const AMOUNT_DECIMALS = 8; // 1 MC = 100_000_000 internal units
const AUTO_REFRESH_MS = 20_000; // matches WalletProvider's defensive 20 s tick

// ---------------------------------------------------------------------
// Public entry. Replaces whatever's in `container` with the wallet UI.
// ---------------------------------------------------------------------
export function renderWalletTab(container: HTMLElement, ctx: WalletTabCtx): void {
  container.replaceChildren();

  const root = el('div', { class: 'col', style: 'gap:12px;' });
  container.appendChild(root);

  // Track the current balance string so refresh callers and the
  // auto-refresh timer don't fight over a single DOM node.
  const state = { balance: '0.00000000' };

  // ---- Address card --------------------------------------------------
  root.appendChild(buildAddressCard(ctx.wallet.address));

  // ---- Balance card --------------------------------------------------
  const balanceCard = buildBalanceCard(state, ctx);
  root.appendChild(balanceCard.node);

  // First refresh, then start the auto-refresh tick. We deliberately do
  // not await — the card already shows the placeholder "0.00000000"
  // and will repaint when the RPC returns.
  void balanceCard.refresh();
  const tick = window.setInterval(() => void balanceCard.refresh(), AUTO_REFRESH_MS);
  // Stop the timer if the container is reparented or torn down. The
  // host can manually clear via `container.dispatchEvent(new Event('teardown'))`
  // for tests; in production the next render call replaces children
  // and we leak the interval (single tab, low cost) — but the
  // MutationObserver below catches detach-from-DOM as well.
  const obs = new MutationObserver(() => {
    if (!container.isConnected) {
      window.clearInterval(tick);
      obs.disconnect();
    }
  });
  if (container.parentNode) obs.observe(container.parentNode, { childList: true, subtree: true });

  // ---- Divider + action list ----------------------------------------
  root.appendChild(el('div', { class: 'muted', style: 'border-top:1px solid var(--border); margin-top:8px;' }));

  root.appendChild(buildActionRow({
    icon: '↗',                       // ↗
    title: 'Send Tokens',
    subtitle: 'Transfer MC to another address',
    onClick: () => openSendDialog(ctx, balanceCard.refresh),
  }));
  root.appendChild(buildActionRow({
    icon: '🔑',                  // 🔑
    title: 'Show recovery phrase',
    subtitle: 'Your 12-word BIP39 mnemonic — never share it',
    onClick: () => openMnemonicDialog(ctx.wallet),
  }));
  root.appendChild(buildActionRow({
    icon: '✖',                        // ✖
    title: 'Sign out / use a different wallet',
    subtitle:
      'Wipes the wallet from this device. The chain still knows your ' +
      'address. Have your recovery phrase first.',
    danger: true,
    onClick: () => confirmSignOut(ctx),
  }));
}

// ---------------------------------------------------------------------
// Address card
// ---------------------------------------------------------------------
function buildAddressCard(address: string): HTMLElement {
  const card = el('div', { class: 'card' });

  card.appendChild(el('div', { style: 'font-weight:600; margin-bottom:8px;' }, ['Address']));

  const row = el('div', { class: 'row', style: 'align-items:flex-start; gap:16px;' });
  card.appendChild(row);

  // QR (left). 'M' error correction matches what the Android player's
  // QR widget uses — comfortable margin against camera-side dirt while
  // still fitting a 42-byte EIP-55 address in version 3 (29x29 modules).
  const qrWrap = el('div', { style: 'flex:0 0 auto; background:#fff; padding:8px; border-radius:6px;' });
  qrWrap.innerHTML = qrcodeSVG(address, { ecc: 'M', scale: 4, margin: 2 });
  row.appendChild(qrWrap);

  // Address + copy (right)
  const right = el('div', { class: 'col grow', style: 'gap:6px; min-width:0;' });
  row.appendChild(right);

  const addrText = el('div', {
    class: 'mono',
    style: 'font-size:12px; word-break:break-all; line-height:1.4;',
  }, [address]);
  right.appendChild(addrText);

  const btnRow = el('div', { class: 'row', style: 'gap:8px;' });
  right.appendChild(btnRow);

  const copyBtn = el('button', {}, ['Copy address']) as HTMLButtonElement;
  copyBtn.addEventListener('click', () => {
    void navigator.clipboard.writeText(address).then(() => {
      const orig = copyBtn.textContent;
      copyBtn.textContent = 'Copied!';
      window.setTimeout(() => { copyBtn.textContent = orig; }, 1200);
    }).catch(() => {
      copyBtn.textContent = 'Copy failed';
    });
  });
  btnRow.appendChild(copyBtn);

  return card;
}

// ---------------------------------------------------------------------
// Balance card
// ---------------------------------------------------------------------
function buildBalanceCard(
  state: { balance: string },
  ctx: WalletTabCtx,
): { node: HTMLElement; refresh: () => Promise<void> } {
  const card = el('div', { class: 'card row', style: 'gap:12px; align-items:center;' });
  const iconWrap = el('div', { style: 'font-size:28px;' }, ['◉']); // ◉
  const textCol  = el('div', { class: 'col grow', style: 'gap:2px;' });
  const label    = el('div', { style: 'font-weight:600;' }, ['Balance']);
  const value    = el('div', { class: 'mono', style: 'font-size:22px;' }, [state.balance]);
  textCol.appendChild(label);
  textCol.appendChild(value);

  const refreshBtn = el('button', { title: 'Refresh balance' }, ['↻']) as HTMLButtonElement; // ↻
  let refreshing = false;
  card.appendChild(iconWrap);
  card.appendChild(textCol);
  card.appendChild(refreshBtn);

  async function refresh(): Promise<void> {
    if (refreshing) return;
    refreshing = true;
    refreshBtn.disabled = true;
    const prev = refreshBtn.textContent;
    refreshBtn.textContent = '⋯'; // ⋯
    try {
      const r = await ctx.node.request<{ balance: string }>(
        'wallet.balance', { address: ctx.wallet.address });
      const bal = (r && typeof r.balance === 'string') ? r.balance : state.balance;
      state.balance = bal;
      value.textContent = bal;
      ctx.onBalanceChange?.(bal);
    } catch {
      // Soft-fail: the Dart provider does the same (swallow + leave the
      // last-known value visible). The user can retry by tapping refresh.
    } finally {
      refreshing = false;
      refreshBtn.disabled = false;
      refreshBtn.textContent = prev;
    }
  }

  refreshBtn.addEventListener('click', () => void refresh());

  return { node: card, refresh };
}

// ---------------------------------------------------------------------
// Action row (Send / Show recovery phrase / Sign out)
// ---------------------------------------------------------------------
function buildActionRow(opts: {
  icon: string;
  title: string;
  subtitle: string;
  danger?: boolean;
  onClick: () => void;
}): HTMLElement {
  const color = opts.danger ? 'var(--accent)' : 'var(--fg)';
  const row = el('div', {
    class: 'row card',
    style: `cursor:pointer; gap:14px; align-items:center;`,
    role: 'button',
    tabindex: '0',
  });
  row.appendChild(el('div', { style: `font-size:22px; width:28px; text-align:center; color:${color};` }, [opts.icon]));
  const text = el('div', { class: 'col grow', style: 'gap:2px;' });
  text.appendChild(el('div', { style: `font-weight:600; color:${color};` }, [opts.title]));
  text.appendChild(el('div', { class: 'muted', style: 'font-size:12px;' }, [opts.subtitle]));
  row.appendChild(text);
  row.appendChild(el('div', { style: 'font-size:18px; color:var(--fg-soft);' }, ['›'])); // ›

  row.addEventListener('click', opts.onClick);
  row.addEventListener('keydown', (e) => {
    if ((e as KeyboardEvent).key === 'Enter' || (e as KeyboardEvent).key === ' ') {
      e.preventDefault();
      opts.onClick();
    }
  });
  return row;
}

// ---------------------------------------------------------------------
// Send-tokens dialog
// ---------------------------------------------------------------------
function openSendDialog(ctx: WalletTabCtx, onSent: () => Promise<void>): void {
  const dlg = openModal('Send Tokens');

  const form = el('div', { class: 'col', style: 'gap:10px;' });
  dlg.body.appendChild(form);

  const toInput = el('input', {
    type: 'text', placeholder: '0x… (recipient address)', spellcheck: 'false',
    autocomplete: 'off',
  }) as HTMLInputElement;
  const amountInput = el('input', {
    type: 'text', placeholder: 'Amount (e.g. 1.5)', inputmode: 'decimal',
  }) as HTMLInputElement;
  const err = el('div', { class: 'err', style: 'min-height:1.2em; font-size:12px;' });

  form.appendChild(labeled('To address', toInput));
  form.appendChild(labeled('Amount', amountInput));
  form.appendChild(err);

  const cancel = el('button', {}, ['Cancel']) as HTMLButtonElement;
  const send   = el('button', { class: 'primary' }, ['Send']) as HTMLButtonElement;
  dlg.actions.appendChild(cancel);
  dlg.actions.appendChild(send);

  cancel.addEventListener('click', () => dlg.close());
  send.addEventListener('click', () => {
    err.textContent = '';
    const to     = toInput.value.trim();
    const amount = amountInput.value.trim();
    const addrErr = validateAddress(to);
    if (addrErr) { err.textContent = addrErr; return; }
    const amtErr = validateAmount(amount);
    if (amtErr) { err.textContent = amtErr; return; }

    send.disabled = true;
    cancel.disabled = true;
    send.textContent = 'Sending…';
    submitTransfer(ctx, to, amount)
      .then((txHash) => {
        dlg.close();
        toast(`Transfer sent! ${txHash ? '0x' + txHash.slice(0, 8) + '…' : ''}`.trim());
        void onSent();
      })
      .catch((e: unknown) => {
        err.textContent = `Transfer failed: ${errMsg(e)}`;
        send.disabled = false;
        cancel.disabled = false;
        send.textContent = 'Send';
      });
  });
}

/** Validate the recipient address. Same shape the chain accepts:
 *  exactly 0x + 40 hex chars. Case-insensitive (we don't enforce EIP-55
 *  on input — the home node lowercases anyway). */
function validateAddress(addr: string): string | null {
  if (!addr) return 'Recipient address is required';
  if (!/^0x[0-9a-fA-F]{40}$/.test(addr)) {
    return 'Address must be 0x + 40 hex characters';
  }
  return null;
}

function validateAmount(s: string): string | null {
  if (!s) return 'Amount is required';
  if (!/^[0-9]+(\.[0-9]+)?$/.test(s)) return 'Amount must be a decimal number';
  // Reject zero and negative after parsing.
  const [whole, frac = ''] = s.split('.');
  if (frac.length > AMOUNT_DECIMALS) {
    return `At most ${AMOUNT_DECIMALS} decimal places`;
  }
  const units = parseAmountToUnits(s);
  if (units === null) return 'Amount out of range';
  if (units <= 0n) return 'Amount must be greater than zero';
  // Sanity ceiling: 21M MC * 1e8 fits in u64, but we guard anyway.
  if (units > (1n << 63n) - 1n) return 'Amount too large';
  void whole;
  return null;
}

/** Decimal-string → integer base-units. Returns null on overflow /
 *  malformed input. Mirrors the Dart logic in WalletProvider.sendTokens. */
function parseAmountToUnits(s: string): bigint | null {
  const parts = s.split('.');
  const whole = parts[0] ?? '0';
  const frac  = (parts[1] ?? '').padEnd(AMOUNT_DECIMALS, '0').slice(0, AMOUNT_DECIMALS);
  try {
    return BigInt(whole) * BigInt(10 ** AMOUNT_DECIMALS) + BigInt(frac || '0');
  } catch {
    return null;
  }
}

async function submitTransfer(
  ctx: WalletTabCtx,
  toAddress: string,
  amountStr: string,
): Promise<string> {
  // 1. Get nonce from the home node.
  const nonceRes = await ctx.node.request<{ nonce: number }>(
    'wallet.nonce', { address: ctx.wallet.address });
  const nonce = BigInt(nonceRes?.nonce ?? 0);

  // 2. Build the 60-byte sign message. Format mirrors
  //    TransferTx::sign_message in musicchain/src/core/transaction.cpp:
  //       chain_id(4 LE) | from(20) | to(20) | amount(8 LE) | nonce(8 LE)
  const amount = parseAmountToUnits(amountStr);
  if (amount === null) throw new Error('Bad amount');

  const msg = new Uint8Array(60);
  const dv  = new DataView(msg.buffer);
  dv.setUint32(0, MC_CHAIN_ID, true);
  msg.set(hexToBytes(ctx.wallet.address), 4);
  msg.set(hexToBytes(toAddress),         24);
  writeU64LE(dv, 44, amount);
  writeU64LE(dv, 52, nonce);

  // 3. Sign — mc_web_wallet_sign hashes sha256(msg) internally then
  //    ECDSA-signs with the wallet's secp256k1 key. Returns 128-char hex.
  const signature = ctx.wallet.sign(msg);

  // 4. Post wallet.transfer. Same param names the Dart NodeClient uses
  //    so the home node dispatcher is identical.
  const res = await ctx.node.request<{ tx_hash?: string }>('wallet.transfer', {
    from_address: ctx.wallet.address,
    to_address:   toAddress,
    amount:       amountStr,
    nonce:        Number(nonce),
    signature,
  });
  return res?.tx_hash ?? '';
}

function hexToBytes(hex: string): Uint8Array {
  let h = hex.toLowerCase();
  if (h.startsWith('0x')) h = h.slice(2);
  if (h.length % 2 !== 0) throw new Error('odd hex length');
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

function writeU64LE(dv: DataView, offset: number, value: bigint): void {
  const lo = Number(value & 0xffffffffn);
  const hi = Number((value >> 32n) & 0xffffffffn);
  dv.setUint32(offset, lo, true);
  dv.setUint32(offset + 4, hi, true);
}

// ---------------------------------------------------------------------
// Recovery-phrase dialog
// ---------------------------------------------------------------------
function openMnemonicDialog(wallet: WalletHandle): void {
  // First gate: re-type password. The Dart screen calls
  // WalletService().readSavedMnemonic() which on mobile is gated by the
  // platform's secure-storage biometric prompt; on web we don't have
  // a hardware key store so the shell wires this to a password prompt.
  const gate = openModal('Show recovery phrase');
  gate.body.appendChild(el('div', { class: 'muted', style: 'margin-bottom:8px;' }, [
    'Re-enter your wallet password to reveal the 12-word BIP39 mnemonic.',
    ' Never share these words with anyone.',
  ]));
  const pw = el('input', {
    type: 'password', placeholder: 'Password', autocomplete: 'current-password',
  }) as HTMLInputElement;
  gate.body.appendChild(pw);
  const err = el('div', { class: 'err', style: 'min-height:1.2em; font-size:12px; margin-top:6px;' });
  gate.body.appendChild(err);

  const cancel = el('button', {}, ['Cancel']) as HTMLButtonElement;
  const reveal = el('button', { class: 'primary' }, ['Reveal']) as HTMLButtonElement;
  gate.actions.appendChild(cancel);
  gate.actions.appendChild(reveal);

  cancel.addEventListener('click', () => gate.close());
  reveal.addEventListener('click', () => {
    err.textContent = '';
    reveal.disabled = true;
    cancel.disabled = true;
    wallet.readMnemonic(pw.value)
      .then((mnemonic) => {
        gate.close();
        if (!mnemonic || mnemonic.trim().length === 0) {
          toast('No recovery phrase saved on this device.');
          return;
        }
        showMnemonic(mnemonic.trim());
      })
      .catch((e: unknown) => {
        err.textContent = errMsg(e) || 'Wrong password';
        reveal.disabled = false;
        cancel.disabled = false;
      });
  });
  pw.addEventListener('keydown', (e) => {
    if ((e as KeyboardEvent).key === 'Enter') reveal.click();
  });
  pw.focus();
}

function showMnemonic(mnemonic: string): void {
  const dlg = openModal('Recovery phrase');
  const box = el('div', {
    class: 'mono',
    style: 'background:var(--bg); padding:12px; border-radius:6px; user-select:all; line-height:1.6; font-size:14px;',
  }, [mnemonic]);
  dlg.body.appendChild(box);

  const copy  = el('button', {}, ['Copy']) as HTMLButtonElement;
  const close = el('button', { class: 'primary' }, ['Close']) as HTMLButtonElement;
  dlg.actions.appendChild(copy);
  dlg.actions.appendChild(close);

  copy.addEventListener('click', () => {
    void navigator.clipboard.writeText(mnemonic).then(() => {
      copy.textContent = 'Copied!';
    });
  });
  close.addEventListener('click', () => dlg.close());
}

// ---------------------------------------------------------------------
// Sign-out confirmation
// ---------------------------------------------------------------------
function confirmSignOut(ctx: WalletTabCtx): void {
  const dlg = openModal('Wipe wallet from this device?');
  dlg.body.appendChild(el('div', { class: 'muted' }, [
    "If you don't have your recovery phrase, you will lose access to ",
    'this wallet forever.',
  ]));
  const cancel = el('button', {}, ['Cancel']) as HTMLButtonElement;
  const wipe   = el('button', { class: 'primary', style: 'background:var(--accent); border-color:var(--accent);' }, ['Wipe wallet']) as HTMLButtonElement;
  dlg.actions.appendChild(cancel);
  dlg.actions.appendChild(wipe);

  cancel.addEventListener('click', () => dlg.close());
  wipe.addEventListener('click', () => {
    wipe.disabled = true;
    cancel.disabled = true;
    void Promise.resolve(ctx.wallet.clearLocalWallet())
      .then(() => Promise.resolve(ctx.onSignOut()))
      .then(() => dlg.close())
      .catch((e: unknown) => {
        toast(`Sign-out failed: ${errMsg(e)}`);
        wipe.disabled = false;
        cancel.disabled = false;
      });
  });
}

// ---------------------------------------------------------------------
// DOM helpers
// ---------------------------------------------------------------------
function el(
  tag: string,
  attrs: Record<string, string> = {},
  children: (string | Node)[] = [],
): HTMLElement {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') node.className = v;
    else node.setAttribute(k, v);
  }
  for (const c of children) {
    node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  }
  return node;
}

function labeled(label: string, input: HTMLElement): HTMLElement {
  const wrap = el('div', { class: 'col', style: 'gap:4px;' });
  wrap.appendChild(el('div', { class: 'muted', style: 'font-size:12px;' }, [label]));
  wrap.appendChild(input);
  return wrap;
}

interface Modal {
  body:    HTMLElement;
  actions: HTMLElement;
  close():  void;
}

function openModal(title: string): Modal {
  const backdrop = el('div', {
    style:
      'position:fixed; inset:0; background:rgba(0,0,0,0.6); display:grid; ' +
      'place-items:center; z-index:1000;',
  });
  const panel = el('div', {
    class: 'card',
    style: 'min-width:320px; max-width:480px; width:90%; background:var(--bg-soft);',
  });
  backdrop.appendChild(panel);
  panel.appendChild(el('div', {
    style: 'font-weight:700; font-size:16px; margin-bottom:10px;',
  }, [title]));
  const body    = el('div', { class: 'col', style: 'gap:8px; margin-bottom:14px;' });
  const actions = el('div', { class: 'row', style: 'justify-content:flex-end; gap:8px;' });
  panel.appendChild(body);
  panel.appendChild(actions);

  document.body.appendChild(backdrop);

  // Close on Escape + clicking outside the panel.
  function onKey(e: KeyboardEvent): void {
    if (e.key === 'Escape') close();
  }
  function onBackdrop(e: MouseEvent): void {
    if (e.target === backdrop) close();
  }
  document.addEventListener('keydown', onKey);
  backdrop.addEventListener('click', onBackdrop);

  function close(): void {
    document.removeEventListener('keydown', onKey);
    backdrop.removeEventListener('click', onBackdrop);
    backdrop.remove();
  }

  return { body, actions, close };
}

function toast(message: string): void {
  const t = el('div', {
    style:
      'position:fixed; bottom:24px; left:50%; transform:translateX(-50%); ' +
      'background:var(--bg-soft); border:1px solid var(--border); ' +
      'border-radius:8px; padding:10px 16px; z-index:2000; ' +
      'box-shadow:0 4px 20px rgba(0,0,0,0.4);',
  }, [message]);
  document.body.appendChild(t);
  window.setTimeout(() => { t.remove(); }, 3000);
}

function errMsg(e: unknown): string {
  if (e instanceof Error) return e.message;
  if (typeof e === 'string') return e;
  try { return JSON.stringify(e); } catch { return String(e); }
}
