// Secure wallet persistence for the web player.
//
// The Dart player stashes the BIP39 mnemonic in platform secure
// storage (Keychain / KeyStore / DPAPI) — see
// musicchain_player/lib/src/services/wallet_service.dart. The web
// has no equivalent: a browser origin cannot ask the OS to seal a
// secret on its behalf. The best we can do is encrypt the mnemonic
// at rest with a key derived from a user-typed passphrase. The
// plaintext mnemonic never hits disk and never leaves a
// WebCrypto-derived AES-GCM key.
//
// Threat model: an attacker who reads IndexedDB / localStorage (XSS,
// stolen profile, disk forensics) gets ciphertext + PBKDF2 salt +
// GCM nonce only. Without the passphrase they have to brute force
// 250 000 PBKDF2-SHA256 iterations per guess. An attacker who can
// run JS in the origin while the passphrase is being typed wins —
// same caveat the Dart secure storage has when the device is
// unlocked.
//
// Backends:
//   * `IndexedDbWalletStorage`  — default. One object store
//     'mc-wallet' with a single record under key 'current' holding
//     `{ version, salt, nonce, ciphertext }` as raw Uint8Arrays.
//   * `LocalStorageWalletStorage` — fallback for browsers without
//     IndexedDB. Same payload, base64-encoded inside one JSON
//     string.
//
// `pickWalletStorage()` returns the first backend whose feature
// detection passes. Both implement the same `WalletStorage`
// contract so callers don't branch.

import {
  getCachedUsername as readCachedUsername,
  setCachedUsername as writeCachedUsername,
  clearCachedUsername,
} from './username';

// ---- Encryption parameters --------------------------------------------------
//
// Exported so tests (and any future "re-encrypt after parameter
// bump" migration) can reference exact values. Raise these only —
// never lower — and bump `ENVELOPE_VERSION` alongside any change so
// `loadDecrypted` can route to the matching deriver.

/** PBKDF2 iteration count. The spec calls for 250k.
 *  Bump in lockstep with ENVELOPE_VERSION. */
export const PBKDF2_ITERATIONS = 250_000;

/** Hash inside PBKDF2. SHA-256 matches the home node's derive_key
 *  helper so a future cross-device import flow can reuse the KDF. */
export const PBKDF2_HASH = 'SHA-256' as const;

/** PBKDF2 salt size in bytes. NIST SP 800-132 floor is 128 bits. */
export const SALT_BYTES = 16;

/** AES key size in bits. 256-bit AES-GCM. */
export const AES_KEY_BITS = 256;

/** AES-GCM nonce size in bytes. 12 is the AES-GCM standard length. */
export const NONCE_BYTES = 12;

/** Envelope version. Increment on any parameter change above. */
export const ENVELOPE_VERSION = 1;

/** IndexedDB database name. */
const DB_NAME = 'mc-wallet';

/** IndexedDB object store name (also the spec-mandated store name). */
const STORE_NAME = 'mc-wallet';

/** Stable record key inside the store. We only ever hold one wallet
 *  envelope; switching wallets calls `clear()` then `saveEncrypted()`. */
const RECORD_KEY = 'current';

/** localStorage key for the fallback backend. */
const LS_KEY = 'mc-wallet:envelope';

// ---- Public contract --------------------------------------------------------

/**
 * Encrypted-at-rest envelope. Stored as raw bytes in IndexedDB and
 * base64-inside-JSON in localStorage. `version` gates future KDF
 * upgrades.
 *
 * The Uint8Array generics are pinned to `ArrayBuffer` (not the
 * `ArrayBufferLike` default in TS 5.7+) so WebCrypto BufferSource
 * positions accept them without a cast — SharedArrayBuffer is not a
 * legal salt / iv / data input anyway.
 */
export interface WalletEnvelope {
  readonly version: number;
  readonly salt: Uint8Array<ArrayBuffer>;
  readonly nonce: Uint8Array<ArrayBuffer>;
  readonly ciphertext: Uint8Array<ArrayBuffer>;
}

/** Errors thrown by the storage layer. Code distinguishes UX paths
 *  ("wrong password" → reprompt, "no wallet" → first-launch flow). */
export class WalletStorageError extends Error {
  public readonly code: WalletStorageErrorCode;
  public constructor(code: WalletStorageErrorCode, message: string) {
    super(message);
    this.name = 'WalletStorageError';
    this.code = code;
  }
}

export type WalletStorageErrorCode =
  | 'no_wallet'
  | 'bad_password'
  | 'corrupt_envelope'
  | 'unsupported_version'
  | 'backend_unavailable';

/**
 * Storage backend for the encrypted wallet mnemonic. Implementations
 * are safe to construct unconditionally — feature detection lives in
 * `pickWalletStorage`.
 */
export interface WalletStorage {
  /** Encrypt `mnemonic` with `password` and persist the envelope.
   *  Overwrites any existing record. */
  saveEncrypted(mnemonic: string, password: string): Promise<void>;
  /** Decrypt and return the saved mnemonic. Throws
   *  `WalletStorageError('no_wallet')` if nothing's saved,
   *  `WalletStorageError('bad_password')` on AES-GCM auth-tag failure. */
  loadDecrypted(password: string): Promise<string>;
  /** Drop the envelope. Idempotent. */
  clear(): Promise<void>;
  /** True if an envelope is present. Doesn't touch the password. */
  has(): Promise<boolean>;
}

// ---- Backend selection ------------------------------------------------------

/** Returns the best available backend for this browser. Prefers
 *  IndexedDB, falls back to localStorage. Throws
 *  `WalletStorageError('backend_unavailable')` only if neither plus
 *  WebCrypto is present — a browser too old to bother with. */
export function pickWalletStorage(): WalletStorage {
  if (!hasWebCrypto()) {
    throw new WalletStorageError(
      'backend_unavailable',
      'WebCrypto SubtleCrypto unavailable — wallet cannot be encrypted',
    );
  }
  if (hasIndexedDb()) {
    return new IndexedDbWalletStorage();
  }
  if (hasLocalStorage()) {
    return new LocalStorageWalletStorage();
  }
  throw new WalletStorageError(
    'backend_unavailable',
    'Neither IndexedDB nor localStorage is available',
  );
}

function hasWebCrypto(): boolean {
  return (
    typeof globalThis.crypto !== 'undefined' &&
    typeof globalThis.crypto.subtle !== 'undefined' &&
    typeof globalThis.crypto.getRandomValues === 'function'
  );
}

function hasIndexedDb(): boolean {
  try {
    return typeof globalThis.indexedDB !== 'undefined';
  } catch {
    // Some browsers throw on bare access in private-mode iframes.
    return false;
  }
}

function hasLocalStorage(): boolean {
  try {
    const ls = globalThis.localStorage;
    if (!ls) return false;
    // Touch it once to flush SecurityError in cookie-blocked contexts.
    const probe = '__mc_probe__';
    ls.setItem(probe, '1');
    ls.removeItem(probe);
    return true;
  } catch {
    return false;
  }
}

// ---- Crypto core ------------------------------------------------------------
//
// One PBKDF2 round per save/load. We derive the AES-GCM key directly
// from the passphrase rather than caching it — caching the derived
// key in JS heap is no safer than the passphrase itself and adds
// session-state complexity the UI doesn't want.

async function deriveKey(
  password: string,
  salt: Uint8Array<ArrayBuffer>,
): Promise<CryptoKey> {
  const subtle = globalThis.crypto.subtle;
  // TextEncoder.encode returns Uint8Array<ArrayBufferLike>; we copy
  // into a fresh ArrayBuffer-backed view so WebCrypto's BufferSource
  // overload is happy. The intermediate buffer is held by the closure
  // until importKey resolves; no SAB ever enters this path.
  const passBytes = textToBytes(password);
  const baseKey = await subtle.importKey(
    'raw',
    passBytes,
    { name: 'PBKDF2' },
    false,
    ['deriveKey'],
  );
  return subtle.deriveKey(
    {
      name: 'PBKDF2',
      salt,
      iterations: PBKDF2_ITERATIONS,
      hash: PBKDF2_HASH,
    },
    baseKey,
    { name: 'AES-GCM', length: AES_KEY_BITS },
    false, // non-extractable: the key never leaves WebCrypto.
    ['encrypt', 'decrypt'],
  );
}

async function encryptMnemonic(
  mnemonic: string,
  password: string,
): Promise<WalletEnvelope> {
  const salt = randomBytes(SALT_BYTES);
  const nonce = randomBytes(NONCE_BYTES);
  const key = await deriveKey(password, salt);
  const plaintext = textToBytes(mnemonic);
  const cipherBuf = await globalThis.crypto.subtle.encrypt(
    { name: 'AES-GCM', iv: nonce },
    key,
    plaintext,
  );
  return {
    version: ENVELOPE_VERSION,
    salt,
    nonce,
    ciphertext: new Uint8Array(cipherBuf),
  };
}

async function decryptMnemonic(
  envelope: WalletEnvelope,
  password: string,
): Promise<string> {
  if (envelope.version !== ENVELOPE_VERSION) {
    throw new WalletStorageError(
      'unsupported_version',
      `Envelope version ${envelope.version} not supported by this build`,
    );
  }
  const key = await deriveKey(password, envelope.salt);
  try {
    const plainBuf = await globalThis.crypto.subtle.decrypt(
      { name: 'AES-GCM', iv: envelope.nonce },
      key,
      envelope.ciphertext,
    );
    return new TextDecoder().decode(plainBuf);
  } catch {
    // AES-GCM auth-tag mismatch — wrong password or tampered blob.
    // We treat both as bad_password from the UI's POV; corrupt blobs
    // are user-fixable via clear() + restore-from-mnemonic.
    throw new WalletStorageError(
      'bad_password',
      'Wrong password or corrupt wallet envelope',
    );
  }
}

function randomBytes(n: number): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(new ArrayBuffer(n));
  globalThis.crypto.getRandomValues(out);
  return out;
}

/** UTF-8 encode `s` into an ArrayBuffer-backed Uint8Array. The fresh
 *  ArrayBuffer prevents the TS 5.7+ `Uint8Array<ArrayBufferLike>`
 *  inference of `TextEncoder.encode` from infecting WebCrypto
 *  BufferSource positions. */
function textToBytes(s: string): Uint8Array<ArrayBuffer> {
  const encoded = new TextEncoder().encode(s);
  const out = new Uint8Array(new ArrayBuffer(encoded.byteLength));
  out.set(encoded);
  return out;
}

// ---- IndexedDB backend ------------------------------------------------------

export class IndexedDbWalletStorage implements WalletStorage {
  public async saveEncrypted(mnemonic: string, password: string): Promise<void> {
    const envelope = await encryptMnemonic(mnemonic, password);
    const db = await openDb();
    try {
      await runTx(db, 'readwrite', (store) => store.put(envelope, RECORD_KEY));
    } finally {
      db.close();
    }
  }

  public async loadDecrypted(password: string): Promise<string> {
    const envelope = await this.readEnvelope();
    if (envelope === null) {
      throw new WalletStorageError('no_wallet', 'No saved wallet to decrypt');
    }
    return decryptMnemonic(envelope, password);
  }

  public async clear(): Promise<void> {
    const db = await openDb();
    try {
      await runTx(db, 'readwrite', (store) => store.delete(RECORD_KEY));
    } finally {
      db.close();
    }
  }

  public async has(): Promise<boolean> {
    return (await this.readEnvelope()) !== null;
  }

  private async readEnvelope(): Promise<WalletEnvelope | null> {
    const db = await openDb();
    try {
      const raw = await runTx<unknown>(db, 'readonly', (store) =>
        store.get(RECORD_KEY),
      );
      if (raw === undefined || raw === null) return null;
      return validateEnvelope(raw);
    } finally {
      db.close();
    }
  }
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = globalThis.indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = (): void => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME);
      }
    };
    req.onsuccess = (): void => resolve(req.result);
    req.onerror = (): void => {
      reject(
        new WalletStorageError(
          'backend_unavailable',
          `IndexedDB open failed: ${req.error?.message ?? 'unknown'}`,
        ),
      );
    };
    req.onblocked = (): void => {
      reject(
        new WalletStorageError(
          'backend_unavailable',
          'IndexedDB open blocked by another tab',
        ),
      );
    };
  });
}

function runTx<T>(
  db: IDBDatabase,
  mode: IDBTransactionMode,
  op: (store: IDBObjectStore) => IDBRequest<T>,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, mode);
    const store = tx.objectStore(STORE_NAME);
    const req = op(store);
    req.onsuccess = (): void => resolve(req.result);
    req.onerror = (): void => {
      reject(
        new WalletStorageError(
          'backend_unavailable',
          `IndexedDB op failed: ${req.error?.message ?? 'unknown'}`,
        ),
      );
    };
    tx.onabort = (): void => {
      reject(
        new WalletStorageError(
          'backend_unavailable',
          `IndexedDB tx aborted: ${tx.error?.message ?? 'unknown'}`,
        ),
      );
    };
  });
}

function validateEnvelope(raw: unknown): WalletEnvelope {
  if (typeof raw !== 'object' || raw === null) {
    throw new WalletStorageError('corrupt_envelope', 'Envelope is not an object');
  }
  const obj = raw as Record<string, unknown>;
  const version = obj['version'];
  const salt = obj['salt'];
  const nonce = obj['nonce'];
  const ciphertext = obj['ciphertext'];
  if (typeof version !== 'number') {
    throw new WalletStorageError('corrupt_envelope', 'Missing version');
  }
  if (!(salt instanceof Uint8Array) || salt.byteLength !== SALT_BYTES) {
    throw new WalletStorageError('corrupt_envelope', 'Bad salt');
  }
  if (!(nonce instanceof Uint8Array) || nonce.byteLength !== NONCE_BYTES) {
    throw new WalletStorageError('corrupt_envelope', 'Bad nonce');
  }
  if (!(ciphertext instanceof Uint8Array) || ciphertext.byteLength === 0) {
    throw new WalletStorageError('corrupt_envelope', 'Bad ciphertext');
  }
  // Copy into ArrayBuffer-backed views so the envelope's generic
  // parameter is locked to ArrayBuffer regardless of what the IDB
  // structured-clone deserializer chose for the parent buffer.
  return {
    version,
    salt: copyToArrayBuffer(salt),
    nonce: copyToArrayBuffer(nonce),
    ciphertext: copyToArrayBuffer(ciphertext),
  };
}

function copyToArrayBuffer(src: Uint8Array): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(new ArrayBuffer(src.byteLength));
  out.set(src);
  return out;
}

// ---- localStorage backend ---------------------------------------------------
//
// localStorage only holds strings, so we base64 the byte fields.
// Outer wrapper is plain JSON so a curious user can spot it in
// DevTools — the encryption is what's load-bearing, not obscurity.

interface LocalStorageEnvelopeJson {
  readonly v: number;
  readonly s: string; // base64 salt
  readonly n: string; // base64 nonce
  readonly c: string; // base64 ciphertext
}

export class LocalStorageWalletStorage implements WalletStorage {
  public async saveEncrypted(mnemonic: string, password: string): Promise<void> {
    const envelope = await encryptMnemonic(mnemonic, password);
    const json: LocalStorageEnvelopeJson = {
      v: envelope.version,
      s: bytesToBase64(envelope.salt),
      n: bytesToBase64(envelope.nonce),
      c: bytesToBase64(envelope.ciphertext),
    };
    globalThis.localStorage.setItem(LS_KEY, JSON.stringify(json));
  }

  public async loadDecrypted(password: string): Promise<string> {
    const envelope = this.readEnvelope();
    if (envelope === null) {
      throw new WalletStorageError('no_wallet', 'No saved wallet to decrypt');
    }
    return decryptMnemonic(envelope, password);
  }

  public async clear(): Promise<void> {
    globalThis.localStorage.removeItem(LS_KEY);
  }

  public async has(): Promise<boolean> {
    return globalThis.localStorage.getItem(LS_KEY) !== null;
  }

  private readEnvelope(): WalletEnvelope | null {
    const raw = globalThis.localStorage.getItem(LS_KEY);
    if (raw === null) return null;
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      throw new WalletStorageError('corrupt_envelope', 'Envelope is not JSON');
    }
    if (typeof parsed !== 'object' || parsed === null) {
      throw new WalletStorageError('corrupt_envelope', 'Envelope is not an object');
    }
    const obj = parsed as Record<string, unknown>;
    const v = obj['v'];
    const s = obj['s'];
    const n = obj['n'];
    const c = obj['c'];
    if (
      typeof v !== 'number' ||
      typeof s !== 'string' ||
      typeof n !== 'string' ||
      typeof c !== 'string'
    ) {
      throw new WalletStorageError('corrupt_envelope', 'Envelope field types wrong');
    }
    return validateEnvelope({
      version: v,
      salt: base64ToBytes(s),
      nonce: base64ToBytes(n),
      ciphertext: base64ToBytes(c),
    });
  }
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) {
    binary += String.fromCharCode(bytes[i]!);
  }
  return globalThis.btoa(binary);
}

function base64ToBytes(b64: string): Uint8Array<ArrayBuffer> {
  const binary = globalThis.atob(b64);
  const out = new Uint8Array(new ArrayBuffer(binary.length));
  for (let i = 0; i < binary.length; i++) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

// ---- Back-compat facade -----------------------------------------------------
//
// Existing screens (wallet_first_launch, settings) imported the
// pre-refactor `storage` object: `saveEncrypted(payload, passphrase)`,
// `cacheUsername`, `getCachedUsername`, `clear`. We keep a thin
// facade that routes through the new backend + the username helpers
// in ./username.ts. The new contract is what consumers should target
// going forward; this exists so the refactor doesn't require a
// flag-day update of every caller.

/** Payload shape accepted by the legacy `storage.saveEncrypted`. */
export interface LegacyWalletPayload {
  readonly mnemonic: string;
  readonly username?: string;
}

interface LegacyStorageFacade {
  saveEncrypted(payload: LegacyWalletPayload, passphrase: string): Promise<void>;
  loadEncrypted(passphrase: string): Promise<LegacyWalletPayload | null>;
  hasEncryptedWallet(): Promise<boolean>;
  cacheUsername(username: string): void;
  getCachedUsername(): string;
  clear(): Promise<void>;
}

let cachedBackend: WalletStorage | null = null;
function backend(): WalletStorage {
  if (cachedBackend === null) {
    cachedBackend = pickWalletStorage();
  }
  return cachedBackend;
}

export const storage: LegacyStorageFacade = {
  async saveEncrypted(payload, passphrase): Promise<void> {
    await backend().saveEncrypted(payload.mnemonic, passphrase);
    if (payload.username !== undefined) {
      writeCachedUsername(payload.username);
    }
  },

  async loadEncrypted(passphrase): Promise<LegacyWalletPayload | null> {
    const be = backend();
    if (!(await be.has())) return null;
    const mnemonic = await be.loadDecrypted(passphrase);
    const username = readCachedUsername();
    return username !== null ? { mnemonic, username } : { mnemonic };
  },

  async hasEncryptedWallet(): Promise<boolean> {
    return backend().has();
  },

  cacheUsername(username): void {
    writeCachedUsername(username);
  },

  getCachedUsername(): string {
    return readCachedUsername() ?? '';
  },

  async clear(): Promise<void> {
    await backend().clear();
    clearCachedUsername();
  },
};
