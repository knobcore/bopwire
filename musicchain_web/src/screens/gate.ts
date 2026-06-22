// Thin shim that forwards to the real wallet-gate implementation.
//
// main.ts imports renderWalletGate from './screens/gate' and calls it
// with an options bag (`{container, node, onAuthed}`), while the actual
// state-machine implementation in './screens/wallet_gate' takes the
// container positionally and an options object without `node` /
// `container`. Rather than push a signature change into either side, this
// module adapts the two: it accepts the options-bag form, ignores `node`
// (the gate itself doesn't need a live socket — the screens it mounts
// pull what they need from their own contexts), and forwards to the
// underlying renderer.
//
// Future cleanup: once nothing outside this file imports the options-bag
// form, delete this shim and rename `wallet_gate.ts` → `gate.ts` so the
// import path matches the file that owns the implementation.

import type { NodeClient } from '../node_client';
import type { Wallet } from '../wallet';
import {
  renderWalletGate as renderInternal,
  type WalletGateOptions as InternalOptions,
} from './wallet_gate';

/** Options bag the host (main.ts) hands us. `node` is accepted for
 *  forward-compatibility — the underlying gate doesn't need it yet, but
 *  any future "auto-unlock by talking to the chain" branch would. */
export interface WalletGateOptions {
  container: HTMLElement;
  node: NodeClient;
  onAuthed: (wallet: Wallet) => void;
}

/** Mount the wallet gate into `opts.container`. Forwards to the real
 *  implementation in ./wallet_gate.ts with the signature that file
 *  exposes. */
export function renderWalletGate(opts: WalletGateOptions): void {
  const inner: InternalOptions = { onAuthed: opts.onAuthed };
  renderInternal(opts.container, inner);
}
