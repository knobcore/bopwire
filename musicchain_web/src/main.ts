// Web player entry point.
//
// Boot order (matches the Android player's MainActivity.onCreate and the
// Windows tracker's WinMain):
//   1. Pre-warm the wallet WASM. The user almost always lands on the
//      sign-in / create-mnemonic gate, and a 300+ ms WASM compile in the
//      middle of that interaction feels like a stall. Kicking it off in
//      parallel with the WebSocket connect means derivation is effectively
//      instant by the time they hit "create" / "import".
//   2. Resolve the home-node URL. Override key is `mc.node_url` in
//      localStorage (documented in README.md → "Pointing the player at a
//      home node"); falls back to wss://<host>/musicchain so the bundle
//      served by the home node itself just works.
//   3. Construct a NodeClient and call connect(). We do NOT await the
//      connection — the wallet gate doesn't need a live socket to render,
//      and a slow / unreachable node should still let the user see the
//      sign-in screen rather than a blank #boot div forever.
//   4. Render the wallet gate. On successful auth (mnemonic created or
//      imported, or unlock from persisted storage), the gate calls
//      onAuthed(wallet) and we tear it down and render the home shell,
//      which mounts the library / player / wallet / upload tabs.
//
// Error policy: anything thrown during bootstrap is surfaced inline in
// the #boot div rather than leaving the user staring at "Loading…".
// The fallback message is intentionally terse — production builds don't
// expose stack traces to the page.

import { NodeClient } from './node_client';
import { NODE_URL_KEY } from './discovery';
import { Wallet } from './wallet';
import { renderWalletGate } from './screens/gate';
import { renderHome } from './screens/home';

// NOTE: librats-WASM was ripped out of the browser player (see memory
// entry `project-musicchain-mini-node-router`). The browser now speaks
// only WebSocket to mini-nodes via NodeClient — the previous
// `RatsClient.create()` + `MiniNodePool.start(rats)` bring-up block is
// gone, along with rats_client.ts / mini_node_pool.ts. Multi-mini-node
// routing now lives server-side; the browser dials the configured
// home node URL and lets the mini-node fan requests out.

/** Compute the WebSocket URL the NodeClient should target. Reads the
 *  localStorage override first; falls back to wss://<page-host>/musicchain
 *  on the assumption the home node is serving this very bundle. */
function resolveNodeUrl(): string {
  try {
    const stored = localStorage.getItem(NODE_URL_KEY);
    if (stored && stored.trim().length > 0) return stored.trim();
  } catch {
    // localStorage can throw in private-mode Safari or behind aggressive
    // tracker-blocking extensions. Treat as "no override".
  }
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${proto}//${location.host}/musicchain`;
}

/** Locate the #app container the index.html ships with. Bail loudly if
 *  it's missing — there's nothing meaningful to do without it. */
function getAppContainer(): HTMLElement {
  const el = document.getElementById('app');
  if (!el) {
    throw new Error('main: #app container missing from index.html');
  }
  return el;
}

/** Replace whatever's currently in #app with a single-line error message.
 *  Used by the bootstrap catch-all so a thrown exception in any module is
 *  visible to the user instead of leaving them staring at "Loading…". */
function renderBootError(container: HTMLElement, err: unknown): void {
  const msg = err instanceof Error ? err.message : String(err);
  container.innerHTML = '';
  const wrap = document.createElement('div');
  wrap.id = 'boot';
  const head = document.createElement('div');
  head.textContent = 'musicchain failed to start';
  const body = document.createElement('div');
  body.className = 'mono err';
  body.textContent = msg;
  body.style.marginTop = '12px';
  wrap.appendChild(head);
  wrap.appendChild(body);
  container.appendChild(wrap);
  // Also dump to the console so devtools-equipped users get the full
  // stack — renderBootError only shows the message.
  console.error('[musicchain] bootstrap failed:', err);
}

/** The actual bootstrap. Exported only so tests can drive it; the
 *  side-effect call at the bottom of this file is what production uses. */
export async function bootstrap(): Promise<void> {
  const container = getAppContainer();

  // 1. Pre-warm the wallet WASM. We deliberately don't `await` this on
  //    the critical path past the next line — we just want the fetch +
  //    instantiate to be in flight by the time renderWalletGate hands
  //    control to the user. The Wallet.* factory methods all await the
  //    same memoised promise internally, so there's no race.
  const walletReady = Wallet.preload();

  // 2. Build the NodeClient against the configured (or default) URL.
  //    NodeClient is the JSON-envelope WebSocket transport the screens
  //    dispatch every RPC through. With librats-WASM gone (memory entry
  //    `project-musicchain-mini-node-router`), NodeClient is the SOLE
  //    transport — the mini-node behind the WebSocket fans verbs out to
  //    the rest of the mesh on the server side.
  const url = resolveNodeUrl();
  const node = new NodeClient({ url });

  // 3. Kick off the connection — non-blocking. We swallow the rejection
  //    here so a transiently-unreachable node doesn't abort the bootstrap;
  //    the client's auto-reconnect loop will keep trying in the background
  //    and any screen that needs a live socket will surface the error
  //    when it tries to send a request.
  node.connect().catch((err: unknown) => {
    console.warn('[musicchain] initial node connect failed:', err);
  });

  // Expose the node for devtools so you can inspect transport state
  // without touching closures. Off the import graph so production
  // tree-shaking still drops the symbol when unused.
  (globalThis as { __mc?: Record<string, unknown> }).__mc = { node };

  // Block on the wallet preload before mounting the gate. The screen
  // itself could await the same promise lazily, but doing it here means
  // the gate never has to render a "compiling wasm…" state of its own —
  // by the time we hand it a container, derivation is ready.
  await walletReady;

  // 4. Render the gate. When the user finishes auth (whether by creating
  //    a fresh mnemonic, importing one, or unlocking persisted state),
  //    the gate calls onAuthed(wallet) and we swap it out for the home
  //    shell, which owns the tabs.
  renderWalletGate({
    container,
    node,
    onAuthed: (wallet: Wallet) => {
      renderHome(container, { wallet, node });
    },
  });
}

// Kick the bootstrap as soon as this module's top-level code runs. The
// page's <script type="module"> already implies defer + DOMContentLoaded-
// like semantics, so #app is guaranteed to exist by the time we get here.
bootstrap().catch((err) => {
  const container = document.getElementById('app');
  if (container) {
    renderBootError(container, err);
  } else {
    // Truly catastrophic — no #app to even render an error into.
    console.error('[musicchain] bootstrap failed before #app available:', err);
  }
});
