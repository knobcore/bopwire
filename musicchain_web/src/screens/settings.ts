// Settings screen for the web player.
//
// Three cards, top-to-bottom:
//   1. Home node — text input for the WebSocket URL the NodeClient
//      should target. Saving persists to localStorage and reloads the
//      page so the new URL takes effect (the NodeClient's url is
//      immutable post-construction, and a full reload is the cleanest
//      way to also recreate any in-flight state).
//   2. Storage    — clear-local-data button with confirmation. Drops
//      localStorage, sessionStorage, and best-effort wipes IndexedDB
//      so the next boot is fresh. Then reloads.
//   3. About      — short blurb + commit hash from
//      `import.meta.env.VITE_GIT_REV` (set by Vite at build time via
//      `define: { 'import.meta.env.VITE_GIT_REV': JSON.stringify(rev) }`
//      or an `.env` file). Falls back to 'dev' when unset.
//
// Keep the implementation framework-free: the rest of the player builds
// the DOM imperatively, and this screen follows the same convention so
// it can be `renderSettings(container, ctx)`-mounted from any router.
//
// The Dart-side equivalent is musicchain_player/lib/src/screens/
// settings_screen.dart — that one configures cache size and shows the
// VPS rendezvous list; the web flavor has a different shape because
// the browser only ever talks to one home-node WebSocket.

import type { NodeClient } from '../node_client.js';
import type { Wallet } from '../wallet.js';
import { NODE_URL_KEY } from '../discovery.js';

/** localStorage key for the home-node WebSocket URL override. Re-exported
 *  for the rare downstream consumer; the canonical definition lives in
 *  `discovery.ts` (key: 'mc.node_url') so main.ts, discovery.ts, and
 *  this screen all read/write the same slot. The old in-file constant
 *  ('musicchain.nodeUrl') was a fork — saving in the Settings screen
 *  used to land on a different key than the bootstrap was reading, so
 *  changing the URL here did nothing. */
export const NODE_URL_STORAGE_KEY = NODE_URL_KEY;

/** Context the host page passes in. Both fields are optional so this
 *  screen still renders during early boot before the wallet/node are
 *  wired up — the Storage and About cards work either way, and the
 *  Home-node card just needs the input + localStorage. */
export interface SettingsContext {
  wallet?: Wallet | null;
  node?: NodeClient | null;
}

/** Read the build-time git revision. Vite inlines
 *  `import.meta.env.VITE_GIT_REV` at build time when defined; we cast
 *  through unknown to avoid pulling in `vite/client` types just for
 *  this one field. */
function gitRev(): string {
  const env = (import.meta as unknown as { env?: Record<string, string | undefined> }).env;
  const rev = env?.VITE_GIT_REV;
  if (typeof rev === 'string' && rev.length > 0) return rev;
  return 'dev';
}

/** Compute the URL we'll show in the input. Persisted override wins;
 *  otherwise mirror the default the NodeClient would pick (wss://host/
 *  musicchain in production, ws://host/musicchain over plain http). */
function currentNodeUrl(node?: NodeClient | null): string {
  const stored = localStorage.getItem(NODE_URL_STORAGE_KEY);
  if (stored && stored.length > 0) return stored;
  // Best-effort inference matching NodeClient.defaultUrl(). We can't
  // read NodeClient's private url field, so we re-derive instead.
  if (typeof location !== 'undefined' && location.host) {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/musicchain`;
  }
  // Last-resort placeholder; the user can edit it.
  void node;
  return 'wss://localhost/musicchain';
}

/** Validate that the entered URL is a well-formed ws:// or wss:// URL.
 *  We accept either scheme so dev can point at a local node over plain
 *  ws://; production should use wss://. */
function validateWsUrl(raw: string): string | null {
  const trimmed = raw.trim();
  if (trimmed.length === 0) return 'URL is required';
  let parsed: URL;
  try {
    parsed = new URL(trimmed);
  } catch {
    return 'Not a valid URL';
  }
  if (parsed.protocol !== 'ws:' && parsed.protocol !== 'wss:') {
    return 'Must be ws:// or wss://';
  }
  return null;
}

/** Wipe every IndexedDB database the origin owns. Best-effort: the
 *  `indexedDB.databases()` API isn't in Firefox yet, so on browsers
 *  that lack it we silently skip — localStorage/sessionStorage still
 *  get cleared, which covers the common case. */
async function wipeIndexedDb(): Promise<void> {
  const idb = (globalThis as { indexedDB?: IDBFactory }).indexedDB;
  if (!idb) return;
  const lister = (idb as IDBFactory & {
    databases?: () => Promise<{ name?: string }[]>;
  }).databases;
  if (typeof lister !== 'function') return;
  let dbs: { name?: string }[] = [];
  try {
    dbs = await lister.call(idb);
  } catch {
    return;
  }
  await Promise.all(
    dbs.map((db) => {
      if (!db.name) return Promise.resolve();
      return new Promise<void>((resolve) => {
        const req = idb.deleteDatabase(db.name!);
        // Resolve on success, error, or blocked — we just want this to
        // finish, not throw. Page reload after the clear handles the
        // edge cases where a DB was still open.
        req.onsuccess = () => resolve();
        req.onerror = () => resolve();
        req.onblocked = () => resolve();
      });
    }),
  );
}

/** Small DOM helper — element with text + optional class. */
function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  cls?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

/** Render the settings screen into `container`. Idempotent: the
 *  container is fully replaced on each call so the caller can re-mount
 *  cheaply from a router. */
export function renderSettings(
  container: HTMLElement,
  ctx: SettingsContext = {},
): void {
  container.replaceChildren();

  const pane = el('div', 'main-pane');
  const title = el('h1', undefined, 'Settings');
  title.style.margin = '0 0 16px 0';
  pane.appendChild(title);

  // ---- Card 1: Home node --------------------------------------------
  const nodeCard = el('section', 'card col');
  nodeCard.style.marginBottom = '16px';
  nodeCard.appendChild(el('h2', undefined, 'Home node'));
  nodeCard.appendChild(
    el(
      'p',
      'muted',
      'WebSocket URL of the home node this player talks to. Use ' +
        'wss:// in production. Changing this reloads the player.',
    ),
  );

  const nodeRow = el('div', 'row');
  const nodeInput = el('input');
  nodeInput.type = 'url';
  nodeInput.placeholder = 'wss://example.com/musicchain';
  nodeInput.value = currentNodeUrl(ctx.node);
  nodeInput.className = 'grow mono';
  nodeInput.spellcheck = false;
  nodeInput.autocomplete = 'off';

  const nodeSave = el('button', 'primary', 'Save');
  nodeSave.type = 'button';

  nodeRow.appendChild(nodeInput);
  nodeRow.appendChild(nodeSave);
  nodeCard.appendChild(nodeRow);

  const nodeStatus = el('div', 'muted');
  nodeStatus.style.minHeight = '1.2em';
  nodeStatus.style.fontSize = '12px';
  nodeCard.appendChild(nodeStatus);

  nodeSave.addEventListener('click', () => {
    const err = validateWsUrl(nodeInput.value);
    if (err) {
      nodeStatus.className = 'err';
      nodeStatus.textContent = err;
      return;
    }
    const normalised = nodeInput.value.trim();
    try {
      localStorage.setItem(NODE_URL_STORAGE_KEY, normalised);
    } catch (e) {
      nodeStatus.className = 'err';
      nodeStatus.textContent =
        'Could not save: ' + (e instanceof Error ? e.message : String(e));
      return;
    }
    nodeStatus.className = 'muted';
    nodeStatus.textContent = 'Saved. Reloading…';
    // Defer the reload one tick so the message paints before navigation.
    setTimeout(() => location.reload(), 250);
  });

  pane.appendChild(nodeCard);

  // ---- Card 2: Storage ----------------------------------------------
  const storageCard = el('section', 'card col');
  storageCard.style.marginBottom = '16px';
  storageCard.appendChild(el('h2', undefined, 'Storage'));
  storageCard.appendChild(
    el(
      'p',
      'muted',
      'Clear all locally cached player data: settings, cached chain ' +
        'state, and any IndexedDB databases this origin owns. The ' +
        'wallet seed is wiped too — make sure you have your recovery ' +
        'phrase before continuing.',
    ),
  );

  const storageRow = el('div', 'row');
  const clearBtn = el('button', undefined, 'Clear local data');
  clearBtn.type = 'button';
  const storageStatus = el('span', 'muted');
  storageStatus.style.fontSize = '12px';
  storageRow.appendChild(clearBtn);
  storageRow.appendChild(storageStatus);
  storageCard.appendChild(storageRow);

  let confirmPending = false;
  let confirmTimer: ReturnType<typeof setTimeout> | null = null;
  clearBtn.addEventListener('click', () => {
    if (!confirmPending) {
      confirmPending = true;
      clearBtn.textContent = 'Click again to confirm';
      clearBtn.classList.add('primary');
      storageStatus.className = 'err';
      storageStatus.textContent = 'This cannot be undone.';
      // Auto-cancel the confirm state after 5s so a stray click later
      // doesn't wipe everything.
      if (confirmTimer) clearTimeout(confirmTimer);
      confirmTimer = setTimeout(() => {
        confirmPending = false;
        clearBtn.textContent = 'Clear local data';
        clearBtn.classList.remove('primary');
        storageStatus.className = 'muted';
        storageStatus.textContent = '';
      }, 5000);
      return;
    }
    // Confirmed — execute the wipe.
    if (confirmTimer) {
      clearTimeout(confirmTimer);
      confirmTimer = null;
    }
    clearBtn.setAttribute('disabled', 'true');
    storageStatus.className = 'muted';
    storageStatus.textContent = 'Clearing…';
    void (async () => {
      try {
        // Drop any RPC socket cleanly before wiping its config.
        try { ctx.node?.disconnect(); } catch { /* swallow */ }
        try { localStorage.clear(); } catch { /* swallow */ }
        try { sessionStorage.clear(); } catch { /* swallow */ }
        await wipeIndexedDb();
        storageStatus.textContent = 'Cleared. Reloading…';
      } catch (e) {
        storageStatus.className = 'err';
        storageStatus.textContent =
          'Partial clear: ' + (e instanceof Error ? e.message : String(e));
      } finally {
        setTimeout(() => location.reload(), 300);
      }
    })();
  });

  pane.appendChild(storageCard);

  // ---- Card 3: About ------------------------------------------------
  const aboutCard = el('section', 'card col');
  aboutCard.appendChild(el('h2', undefined, 'About'));
  aboutCard.appendChild(
    el('p', 'muted', 'musicchain web player. Wallet runs in WASM in this ' +
      'browser; chain RPC is proxied through the home node over WebSocket.'),
  );

  const versionRow = el('div', 'row mono');
  versionRow.style.fontSize = '12px';
  const versionLabel = el('span', 'muted', 'build');
  const versionValue = el('span', undefined, gitRev());
  versionRow.appendChild(versionLabel);
  versionRow.appendChild(versionValue);
  aboutCard.appendChild(versionRow);

  pane.appendChild(aboutCard);

  container.appendChild(pane);
}
