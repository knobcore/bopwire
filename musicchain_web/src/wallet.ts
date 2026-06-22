// TypeScript wrapper around the wallet WASM module (wallet_glue.c).
//
// Mirrors the Dart `WalletService` flow at musicchain_player/lib/src/
// services/wallet_service.dart — BIP39 12-word mnemonic in, opaque
// native wallet handle out, plus the address / pubkey / sign
// operations the rest of the player needs. Same libwally + secp256k1
// derivation as the desktop DLL and Android JNI, so the address shown
// in the browser matches the one the home node registers on chain.
//
// The .wasm payload is loaded LAZILY: the factory is only called the
// first time `Wallet.generateMnemonic`, `Wallet.validate`, or
// `Wallet.fromMnemonic` runs. Subsequent calls reuse the cached module
// promise so we never instantiate twice in the same page session.
//
// Memory discipline: every malloc'd C pointer (the wallet handle, the
// strings returned by mc_web_*) is freed via `Module._free`, including
// on the error paths. The wrapper allocates UTF-8 input buffers with
// `Module._malloc` + `Module.stringToUTF8` and frees them in `finally`
// blocks so a thrown exception doesn't leak heap.

import createWalletModule from '@wasm/wallet.js';

// Concrete type alias for the awaited factory result so we can reuse
// it in helper signatures without re-typing the awaited generic each
// time.
type WalletModule = Awaited<ReturnType<typeof createWalletModule>>;

// Cached module promise. The first caller starts instantiation; later
// callers `await` the same promise so we don't pay the WASM compile
// cost twice. Held at module scope so it survives across `Wallet`
// instances — the underlying libwally + secp256k1 contexts in
// wallet_glue.c live for the lifetime of the page.
let modulePromise: Promise<WalletModule> | null = null;
let moduleOptions: Record<string, unknown> | null = null;

function loadModule(): Promise<WalletModule> {
  if (!modulePromise) {
    modulePromise = moduleOptions
      ? createWalletModule(moduleOptions)
      : createWalletModule();
  }
  return modulePromise;
}

/**
 * Test-only override: pre-bake `Module` options (e.g. `{ wasmBinary }`)
 * before the first `loadModule()` call. Production code never calls
 * this — the browser's fetch path resolves `wallet.wasm` from the
 * served `/wasm/` directory and that's all we need. Under vitest /
 * Node there's no fetch, so the smoke tests inject the .wasm bytes
 * directly via this hook.
 *
 * Must be called before any `Wallet.*` static method; throws if the
 * module has already been instantiated. Pass `null` to clear.
 */
export function __setWalletModuleOptionsForTest(
  opts: Record<string, unknown> | null,
): void {
  if (modulePromise) {
    throw new Error('wallet module already loaded; set options before first call');
  }
  moduleOptions = opts;
}

// Helper: copy a JS string into a fresh malloc'd UTF-8 buffer on the
// WASM heap. Returns the pointer; caller must `Module._free(ptr)` when
// done. Mirrors the convention in wallet_glue.c where every input
// string is `const char*` with NUL termination.
function allocUtf8(module: WalletModule, str: string): number {
  const byteLen = module.lengthBytesUTF8(str) + 1;
  const ptr = module._malloc(byteLen);
  if (!ptr) throw new Error('WASM heap alloc failed');
  module.stringToUTF8(str, ptr, byteLen);
  return ptr;
}

// Helper: read a NUL-terminated C string off the WASM heap and then
// free the underlying pointer. Used for every `char*`-returning
// function in wallet_glue.c (address, pubkey, signature, mnemonic).
// Tolerates a 0 return so callers can use it directly inside an
// expression and check for failure afterwards.
function readAndFreeCString(module: WalletModule, ptr: number): string | null {
  if (!ptr) return null;
  const str = module.UTF8ToString(ptr);
  module._free(ptr);
  return str;
}

// Cwrapped signatures. We bind these once per module instance and
// cache them on a `WrappedExports` so call sites don't pay cwrap's
// per-call closure construction. The shape mirrors the C signatures
// in wallet_glue.c exactly.
interface WrappedExports {
  bip39Generate12: (entropyPtr: number) => number; // returns char*
  bip39Validate: (mnemonicPtr: number) => number;  // returns int (0/1)
  walletFromMnemonic: (mnemonicPtr: number, passphrasePtr: number) => number; // void*
  walletGetAddress: (walletPtr: number) => number; // char*
  walletGetPubkey: (walletPtr: number) => number;  // char*
  walletSign: (walletPtr: number, dataPtr: number, len: number) => number; // char*
  walletFree: (walletPtr: number) => void;
}

function bindExports(module: WalletModule): WrappedExports {
  return {
    bip39Generate12: module.cwrap<number>('mc_web_bip39_generate_12', 'number', ['number']),
    bip39Validate: module.cwrap<number>('mc_web_bip39_validate', 'number', ['number']),
    walletFromMnemonic: module.cwrap<number>(
      'mc_web_wallet_from_mnemonic',
      'number',
      ['number', 'number'],
    ),
    walletGetAddress: module.cwrap<number>('mc_web_wallet_get_address', 'number', ['number']),
    walletGetPubkey: module.cwrap<number>('mc_web_wallet_get_pubkey', 'number', ['number']),
    walletSign: module.cwrap<number>(
      'mc_web_wallet_sign',
      'number',
      ['number', 'number', 'number'],
    ),
    walletFree: module.cwrap<void>('mc_web_wallet_free', null, ['number']),
  };
}

// Per-module-instance cache of the cwrapped exports. Keyed by the
// module object itself (which is stable within a page session) so we
// don't rebuild the closures on every call.
const exportsCache = new WeakMap<WalletModule, WrappedExports>();

async function getExports(): Promise<{
  module: WalletModule;
  exports: WrappedExports;
}> {
  const module = await loadModule();
  let exports_ = exportsCache.get(module);
  if (!exports_) {
    exports_ = bindExports(module);
    exportsCache.set(module, exports_);
  }
  return { module, exports: exports_ };
}

/**
 * Browser-side mirror of the Dart `WalletService` wallet. Holds an
 * opaque native handle (a malloc'd 32-byte secp256k1 private key inside
 * the WASM heap) plus the cached derived address / public key.
 *
 * The recovery secret is the BIP39 mnemonic — never the priv-key bytes
 * directly. Callers persist the mnemonic in browser storage (IndexedDB
 * behind a passphrase, or sessionStorage for ephemeral logins) and
 * recreate the `Wallet` instance via `Wallet.fromMnemonic` on each
 * page load.
 *
 * The lazy WASM load means even the "sync-looking" static methods
 * (`generateMnemonic`, `validate`) return a Promise — they have to
 * await the factory's first run. After that the underlying calls are
 * synchronous pointer chases inside the WASM module.
 */
export class Wallet {
  // Static methods only construct via `Wallet.fromMnemonic`; the ctor
  // is private so the invariant "handle is non-zero" holds for the
  // lifetime of the instance.
  private constructor(
    private readonly module: WalletModule,
    private readonly exports: WrappedExports,
    private handle: number,
    private readonly cachedAddress: string,
    private readonly cachedPublicKey: string,
  ) {}

  /**
   * Kick off the WASM fetch + instantiate without producing a wallet.
   * Used by the bootstrap path so that by the time the user clicks
   * "create" / "import" on the gate, derivation is a synchronous-feeling
   * pointer chase rather than a 300+ ms WASM compile in the middle of
   * the interaction. Safe to call any number of times — the underlying
   * module promise is memoised.
   */
  static async preload(): Promise<void> {
    await loadModule();
  }

  /**
   * Generates a fresh 12-word English BIP39 mnemonic.
   *
   * Sources 16 bytes of entropy via the browser's `crypto.getRandom-
   * Values` — same strength as the desktop / Android paths (which read
   * from BCryptGenRandom or /dev/urandom). The entropy is copied into
   * the WASM heap, mc_web_bip39_generate_12 turns it into a mnemonic,
   * and both the entropy buffer and the returned string allocation are
   * scrubbed / freed before we return.
   */
  static async generateMnemonic(): Promise<string> {
    const { module, exports } = await getExports();

    const entropy = new Uint8Array(16);
    crypto.getRandomValues(entropy);

    const entropyPtr = module._malloc(16);
    if (!entropyPtr) throw new Error('WASM heap alloc failed');
    let mnemonicPtr = 0;
    try {
      module.HEAPU8.set(entropy, entropyPtr);
      mnemonicPtr = exports.bip39Generate12(entropyPtr);
      // Best-effort scrub of the entropy bytes both in JS and in the
      // WASM heap so they don't linger in memory after we return.
      entropy.fill(0);
      module.HEAPU8.fill(0, entropyPtr, entropyPtr + 16);
      const mnemonic = readAndFreeCString(module, mnemonicPtr);
      mnemonicPtr = 0;
      if (!mnemonic) throw new Error('mc_web_bip39_generate_12 returned null');
      return mnemonic;
    } finally {
      if (mnemonicPtr) module._free(mnemonicPtr);
      module._free(entropyPtr);
    }
  }

  /**
   * Returns `true` if `mnemonic` is a valid BIP39 phrase (wordlist
   * hit + checksum byte matches). Lowercases + trims first because the
   * underlying libwally validator is strict about both.
   *
   * Async because the WASM module may not be loaded yet on first call;
   * later calls resolve on the next microtask.
   */
  static async validate(mnemonic: string): Promise<boolean> {
    const { module, exports } = await getExports();
    const cleaned = mnemonic.trim().toLowerCase();
    const ptr = allocUtf8(module, cleaned);
    try {
      return exports.bip39Validate(ptr) === 1;
    } finally {
      module._free(ptr);
    }
  }

  /**
   * Derive (or restore) a `Wallet` from a 12-word mnemonic. The optional
   * `passphrase` is mixed into the BIP39 seed per the standard — leave
   * it empty/undefined to match the desktop + Android players, which
   * don't surface a passphrase field today.
   *
   * Throws `Error("Invalid BIP39 mnemonic")` if validation fails so the
   * UI can show a typo-friendly error before deriving the key. Throws
   * `Error("Failed to derive wallet from mnemonic")` if libwally
   * accepts the phrase but secp256k1 rejects the derived key (vanishingly
   * rare but the C side guards for it).
   */
  static async fromMnemonic(mnemonic: string, passphrase?: string): Promise<Wallet> {
    const { module, exports } = await getExports();
    const cleaned = mnemonic.trim().toLowerCase();

    const mnemonicPtr = allocUtf8(module, cleaned);
    // BIP39 spec: an empty passphrase salts with just "mnemonic". The C
    // glue tolerates a NULL passphrase and substitutes "" itself, so we
    // could pass 0 here — but allocating an empty UTF-8 buffer keeps the
    // calling convention uniform and is cheap.
    const passphrasePtr = allocUtf8(module, passphrase ?? '');
    let handle = 0;
    let addressPtr = 0;
    let pubkeyPtr = 0;
    try {
      if (exports.bip39Validate(mnemonicPtr) !== 1) {
        throw new Error('Invalid BIP39 mnemonic');
      }
      handle = exports.walletFromMnemonic(mnemonicPtr, passphrasePtr);
      if (!handle) throw new Error('Failed to derive wallet from mnemonic');

      // Materialise the cached views the rest of the player asks for so
      // `wallet.address` / `wallet.publicKey` are plain string getters.
      addressPtr = exports.walletGetAddress(handle);
      const address = readAndFreeCString(module, addressPtr);
      addressPtr = 0;
      if (!address) throw new Error('mc_web_wallet_get_address returned null');

      pubkeyPtr = exports.walletGetPubkey(handle);
      const publicKey = readAndFreeCString(module, pubkeyPtr);
      pubkeyPtr = 0;
      if (!publicKey) throw new Error('mc_web_wallet_get_pubkey returned null');

      const wallet = new Wallet(module, exports, handle, address, publicKey);
      handle = 0; // ownership transferred to the new Wallet instance
      return wallet;
    } finally {
      module._free(mnemonicPtr);
      module._free(passphrasePtr);
      if (addressPtr) module._free(addressPtr);
      if (pubkeyPtr) module._free(pubkeyPtr);
      if (handle) exports.walletFree(handle);
    }
  }

  /**
   * EIP-55-checksummed 0x-prefixed 20-byte address derived from the
   * wallet's secp256k1 key — same derivation the home node uses to
   * register the wallet on chain.
   */
  get address(): string {
    this.ensureLive();
    return this.cachedAddress;
  }

  /**
   * 33-byte compressed secp256k1 public key as 66 lowercase hex chars.
   * Used by the chain RPC layer for signed-request authentication.
   */
  get publicKey(): string {
    this.ensureLive();
    return this.cachedPublicKey;
  }

  /**
   * ECDSA-sign sha256(data). Returns the compact 64-byte (r,s)
   * signature as 128 lowercase hex chars — the form the chain expects.
   * Throws if the wallet has been freed.
   */
  sign(data: Uint8Array): string {
    this.ensureLive();
    if (data.length === 0) throw new Error('Cannot sign empty data');

    const dataPtr = this.module._malloc(data.length);
    if (!dataPtr) throw new Error('WASM heap alloc failed');
    let sigPtr = 0;
    try {
      this.module.HEAPU8.set(data, dataPtr);
      sigPtr = this.exports.walletSign(this.handle, dataPtr, data.length);
      const sig = readAndFreeCString(this.module, sigPtr);
      sigPtr = 0;
      if (!sig) throw new Error('mc_web_wallet_sign returned null');
      return sig;
    } finally {
      if (sigPtr) this.module._free(sigPtr);
      this.module._free(dataPtr);
    }
  }

  /**
   * Release the underlying WASM-heap allocation. Safe to call more
   * than once. After `free()`, the address / publicKey getters and
   * `sign` throw — the libwally + secp256k1 process-wide contexts
   * survive for any subsequent `Wallet.fromMnemonic` call.
   */
  free(): void {
    if (this.handle) {
      this.exports.walletFree(this.handle);
      this.handle = 0;
    }
  }

  private ensureLive(): void {
    if (!this.handle) throw new Error('Wallet has been freed');
  }
}
