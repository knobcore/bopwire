/*
 * fingerprinter.ts — TypeScript wrapper around the chromaprint WASM module
 * the home node's `fingerprint.submit` RPC expects.
 *
 * Flow when the user picks a local audio file in the library UI:
 *
 *   1. file.arrayBuffer()                     — raw bytes
 *   2. SHA-256(bytes) → contentHash (hex)     — the chain-side identifier;
 *                                               always available even if
 *                                               chromaprint failed to load
 *   3. AudioContext.decodeAudioData(bytes)    — browser-native codec stack
 *                                               (mp3/aac/ogg/flac/wav)
 *   4. mix to mono → resample to 11025 Hz     — chromaprint's expected rate
 *      → convert Float32 → Int16 PCM            (same params as Android +
 *                                                home-node fingerprinters)
 *   5. mc_web_chromaprint_new / start / feed
 *      / finish / get_compressed              — base64-encoded compressed
 *                                               Chromaprint v2 fingerprint
 *   6. SHA-256(compressed bytes) → fingerprintHash (hex)
 *
 * If wasm/chromaprint.js can't be loaded (404 in dev, build skipped, etc.)
 * the loader throws FingerprinterUnavailableError so library.ts can fall
 * back to hash-only registration (`fingerprint.submitHashOnly`) instead of
 * blocking the upload UI.
 */

import type { ChromaprintEmscriptenModule } from '@wasm/chromaprint.js';

/** Result of fingerprinting a single local file. Field shapes match the
 *  arguments fingerprint.submit takes on the home node JSON-RPC. */
export interface FingerprintResult {
  /** Base64-encoded compressed Chromaprint v2 fingerprint, exactly the
   *  format the home node + AcoustID expect. */
  compressed: string;
  /** SHA-256(compressed-string-bytes) in lowercase hex. Used as the
   *  fingerprint's content-addressed key in the home node's index. */
  fingerprintHash: string;
  /** SHA-256(raw-file-bytes) in lowercase hex. The chain-side identifier
   *  for the underlying audio blob. Always populated, even on fingerprint
   *  failure (in which case fingerprintFile() rethrows but callers can
   *  fall back to a hash-only submit by retrying with hashOnly()). */
  contentHash: string;
  /** Duration of the decoded audio in milliseconds. */
  durationMs: number;
  /** Original sample rate as reported by the decoder. */
  sampleRate: number;
  /** Original channel count as reported by the decoder. */
  channelCount: number;
}

/** Thrown when the chromaprint WASM module can't be located or
 *  instantiated. library.ts catches this specifically and falls back to
 *  hash-only registration. Distinct from generic decode/runtime errors so
 *  the UI can show "file unsupported" vs. "fingerprinter offline". */
export class FingerprinterUnavailableError extends Error {
  /**
   * Underlying cause (the original import / instantiation error). Kept on
   * a custom property because the standard ErrorOptions.cause path needs
   * `lib: ES2022.Error` which is already in tsconfig — but we surface it
   * explicitly so callers can introspect without TS having to widen the
   * Error base type.
   */
  public readonly originalError: unknown;

  public constructor(message: string, originalError?: unknown) {
    super(message);
    this.name = 'FingerprinterUnavailableError';
    this.originalError = originalError;
  }
}

// ---------------------------------------------------------------------------
// Lazy WASM loader
// ---------------------------------------------------------------------------
//
// chromaprint.js is built with -sMODULARIZE=1 -sEXPORT_ES6=1, factory name
// createChromaprintModule. We load it via a dynamic `import('@wasm/...')`
// so a missing build artifact surfaces as a typed
// FingerprinterUnavailableError instead of crashing the bundle eagerly.

/** Bundled cwrap-resolved entry points. Holds the live module so we can
 *  keep memory views and heap allocators around for repeated calls. */
interface ChromaprintApi {
  module: ChromaprintEmscriptenModule;
  new_: () => number;
  start: (ctx: number, sampleRate: number, channels: number) => number;
  feed: (ctx: number, samplesPtr: number, count: number) => number;
  finish: (ctx: number) => number;
  getCompressed: (ctx: number) => number;
  free: (ctx: number) => void;
}

let g_apiPromise: Promise<ChromaprintApi> | null = null;

/** Lazy, idempotent loader. First caller kicks off the dynamic import;
 *  subsequent callers await the same promise. On failure the promise is
 *  *not* cached so a later retry (e.g. after the user reloads to pick up
 *  a new build) can succeed. */
async function loadChromaprint(): Promise<ChromaprintApi> {
  if (g_apiPromise) return g_apiPromise;

  const promise = (async (): Promise<ChromaprintApi> => {
    let factory: (
      opts?: Record<string, unknown>,
    ) => Promise<ChromaprintEmscriptenModule>;
    try {
      // Dynamic import so a missing wasm/chromaprint.js becomes a
      // catchable promise rejection instead of a bundle-load failure.
      // The vite.config.ts publicDir serves wasm/ at the root, so this
      // alias resolves to /chromaprint.js at runtime. The emscripten
      // glue handles fetching the sibling chromaprint.wasm itself via
      // its own URL resolution against import.meta.url.
      const mod = await import('@wasm/chromaprint.js');
      factory = mod.default;
    } catch (err) {
      throw new FingerprinterUnavailableError(
        'chromaprint.js failed to load — was build.sh run with chromaprint sources present?',
        err,
      );
    }

    let module: ChromaprintEmscriptenModule;
    try {
      module = await factory();
    } catch (err) {
      throw new FingerprinterUnavailableError(
        'chromaprint WASM module failed to instantiate',
        err,
      );
    }

    // cwrap signatures mirror native/chromaprint_glue.c. Pointer-sized
    // ints in WASM32 are 'number' on the JS side.
    const new_ = module.cwrap('mc_web_chromaprint_new', 'number', []) as () => number;
    const start = module.cwrap(
      'mc_web_chromaprint_start',
      'number',
      ['number', 'number', 'number'],
    ) as (ctx: number, sr: number, ch: number) => number;
    const feed = module.cwrap(
      'mc_web_chromaprint_feed',
      'number',
      ['number', 'number', 'number'],
    ) as (ctx: number, ptr: number, n: number) => number;
    const finish = module.cwrap(
      'mc_web_chromaprint_finish',
      'number',
      ['number'],
    ) as (ctx: number) => number;
    const getCompressed = module.cwrap(
      'mc_web_chromaprint_get_compressed',
      'number',
      ['number'],
    ) as (ctx: number) => number;
    const free = module.cwrap(
      'mc_web_chromaprint_free',
      null,
      ['number'],
    ) as (ctx: number) => void;

    return { module, new_, start, feed, finish, getCompressed, free };
  })();

  // Drop the cached promise on failure so callers can retry later.
  promise.catch(() => {
    if (g_apiPromise === promise) g_apiPromise = null;
  });

  g_apiPromise = promise;
  return promise;
}

// ---------------------------------------------------------------------------
// Audio preprocessing
// ---------------------------------------------------------------------------

/** chromaprint expects 11025 Hz mono. The Android player + home node both
 *  feed it at that rate, so matching the constant here keeps fingerprints
 *  byte-identical across platforms for the same source audio. */
const CHROMAPRINT_SAMPLE_RATE = 11025;

/** AudioContext we reuse across calls. The first decodeAudioData() spins
 *  up the platform decoder threads; subsequent calls reuse them. Lazily
 *  constructed because Safari refuses to instantiate an AudioContext from
 *  module top-level. */
let g_audioCtx: AudioContext | null = null;

function getAudioContext(): AudioContext {
  if (!g_audioCtx) {
    // Sample rate here is the *context* rate, not the decoder's output
    // rate; decodeAudioData() preserves the file's native rate. Picking
    // 48 kHz keeps the context cheap (the player UI doesn't actually
    // play through this context — it's just a decode harness).
    const ctor = globalThis as unknown as {
      AudioContext?: typeof AudioContext;
      webkitAudioContext?: typeof AudioContext;
    };
    const AC = ctor.AudioContext ?? ctor.webkitAudioContext;
    if (!AC) throw new Error('Web Audio API not available in this browser');
    g_audioCtx = new AC({ sampleRate: 48000 });
  }
  return g_audioCtx;
}

/**
 * Mix every channel down to a single Float32 stream by averaging samples
 * across channels. Matches AudioProcessor's downmix in the Android +
 * home-node fingerprinters (simple sum / N, not loudness-weighted).
 */
function mixToMono(buffer: AudioBuffer): Float32Array {
  const channels = buffer.numberOfChannels;
  const frames = buffer.length;
  if (channels === 1) {
    // getChannelData returns a view onto the underlying buffer; copy so
    // we own the storage for the downstream resampler.
    return new Float32Array(buffer.getChannelData(0));
  }
  const mono = new Float32Array(frames);
  for (let ch = 0; ch < channels; ++ch) {
    const data = buffer.getChannelData(ch);
    for (let i = 0; i < frames; ++i) {
      mono[i] += data[i];
    }
  }
  const scale = 1 / channels;
  for (let i = 0; i < frames; ++i) mono[i] *= scale;
  return mono;
}

/**
 * Linear-interpolation resampler to 11025 Hz. This isn't audiophile-
 * grade, but chromaprint applies its own internal resampler + low-pass
 * after we feed it anyway — what matters is that the *count* of samples
 * matches the rate we declare in chromaprint_start(). Using linear
 * interp here keeps the implementation tiny and matches what the
 * Android player's audio_processor uses for the same pre-decode step.
 */
function resampleTo11025(input: Float32Array, srcRate: number): Float32Array {
  if (srcRate === CHROMAPRINT_SAMPLE_RATE) return input;
  const ratio = srcRate / CHROMAPRINT_SAMPLE_RATE;
  const outLen = Math.floor(input.length / ratio);
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; ++i) {
    const srcPos = i * ratio;
    const i0 = Math.floor(srcPos);
    const i1 = Math.min(i0 + 1, input.length - 1);
    const t = srcPos - i0;
    out[i] = input[i0] * (1 - t) + input[i1] * t;
  }
  return out;
}

/**
 * Float32 [-1, 1] → Int16 [-32768, 32767]. Clamps before scaling because
 * decodeAudioData can occasionally emit values fractionally above 1.0 for
 * lossy codecs that overshoot during reconstruction.
 */
function floatToInt16(input: Float32Array): Int16Array {
  const out = new Int16Array(input.length);
  for (let i = 0; i < input.length; ++i) {
    let s = input[i];
    if (s > 1) s = 1;
    else if (s < -1) s = -1;
    out[i] = s < 0 ? Math.round(s * 32768) : Math.round(s * 32767);
  }
  return out;
}

// ---------------------------------------------------------------------------
// SHA-256 helpers
// ---------------------------------------------------------------------------

const HEX_CHARS = '0123456789abcdef';

function bytesToHex(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; ++i) {
    const b = bytes[i];
    s += HEX_CHARS[b >> 4] + HEX_CHARS[b & 0x0f];
  }
  return s;
}

/**
 * SHA-256 over an ArrayBuffer or a typed-array view. Always passes a
 * fresh ArrayBuffer to crypto.subtle so the call typechecks under
 * strict-mode lib.dom.d.ts (which now distinguishes ArrayBuffer from
 * SharedArrayBuffer in BufferSource). For Uint8Array inputs we slice
 * the underlying buffer to match the view's offset+length window.
 */
async function sha256Hex(data: ArrayBuffer | Uint8Array): Promise<string> {
  let buf: ArrayBuffer;
  if (data instanceof Uint8Array) {
    // .slice() on the backing buffer yields a plain ArrayBuffer even if
    // the source happened to be a SharedArrayBuffer view.
    const sliced = data.buffer.slice(
      data.byteOffset,
      data.byteOffset + data.byteLength,
    );
    buf = sliced as ArrayBuffer;
  } else {
    buf = data;
  }
  const digest = await crypto.subtle.digest('SHA-256', buf);
  return bytesToHex(new Uint8Array(digest));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Decode + fingerprint a local audio file.
 *
 * Throws FingerprinterUnavailableError if the chromaprint WASM module
 * can't be loaded. Throws a generic Error if the file isn't decodable by
 * the browser's WebAudio decoder (e.g. WMA on Chrome) — in that case the
 * caller already has contentHash from the file's bytes but should report
 * the failure rather than silently fall back, since unsupported codecs
 * usually point at a user-facing bug.
 */
export async function fingerprintFile(file: File | Blob): Promise<FingerprintResult> {
  // Step 1: read bytes. We need the ArrayBuffer twice (once for SHA-256,
  // once for decodeAudioData — which consumes its argument), so capture
  // it once and slice for the decode call.
  const bytes = await file.arrayBuffer();

  // Step 2: SHA-256 of the raw file bytes → contentHash. Kick this off
  // in parallel with the decode to overlap the work; both are CPU-bound
  // but run on different threads (subtle.crypto offloads to a worker).
  const contentHashPromise = sha256Hex(bytes);

  // Step 3: decode via WebAudio. decodeAudioData mutates / detaches its
  // input on some browsers, so hand it a fresh copy.
  let audioBuffer: AudioBuffer;
  try {
    audioBuffer = await getAudioContext().decodeAudioData(bytes.slice(0));
  } catch (err) {
    // Surface a real Error with the contentHash already computed so the
    // caller can decide whether to hash-only-register the file anyway.
    const contentHash = await contentHashPromise;
    const name = file instanceof File ? file.name : 'blob';
    const e = new Error(
      `decodeAudioData failed for ${name} — codec unsupported by this browser? (${String(err)})`,
    ) as Error & { contentHash: string };
    e.contentHash = contentHash;
    throw e;
  }

  // Step 4: mix to mono, resample to 11025 Hz, convert to int16.
  const mono = mixToMono(audioBuffer);
  const resampled = resampleTo11025(mono, audioBuffer.sampleRate);
  const pcm = floatToInt16(resampled);

  // Step 5: drive chromaprint. Load the WASM module lazily; this is the
  // call that throws FingerprinterUnavailableError if the build is
  // missing.
  const api = await loadChromaprint();

  // Allocate the PCM buffer on the WASM heap and memcpy. Doing it in
  // one big chunk rather than streaming keeps the JS↔WASM boundary
  // crossings to a single feed call per file, which is the cheapest
  // path for typical 3-7 minute tracks (≈ 8-15 MB of int16 PCM).
  const samplesPtr = api.module._malloc(pcm.length * 2);
  if (!samplesPtr) {
    throw new Error('chromaprint: WASM heap allocation failed (file too large?)');
  }
  let ctx = 0;
  let compressedPtr = 0;
  try {
    api.module.HEAP16.set(pcm, samplesPtr >> 1);

    ctx = api.new_();
    if (!ctx) throw new Error('chromaprint_new returned NULL');

    if (!api.start(ctx, CHROMAPRINT_SAMPLE_RATE, 1)) {
      throw new Error('chromaprint_start failed');
    }
    if (!api.feed(ctx, samplesPtr, pcm.length)) {
      throw new Error('chromaprint_feed failed');
    }
    if (!api.finish(ctx)) {
      throw new Error('chromaprint_finish failed');
    }

    compressedPtr = api.getCompressed(ctx);
    if (!compressedPtr) {
      throw new Error('chromaprint_get_compressed returned NULL');
    }
    const compressed = api.module.UTF8ToString(compressedPtr);

    // Step 6: SHA-256 of the compressed base64 string's UTF-8 bytes.
    // The home node hashes the literal string the same way (TextEncoder
    // equivalent on the Dart side) so the hashes agree across platforms.
    const fingerprintHashBytes = new TextEncoder().encode(compressed);
    const [fingerprintHash, contentHash] = await Promise.all([
      sha256Hex(fingerprintHashBytes),
      contentHashPromise,
    ]);

    return {
      compressed,
      fingerprintHash,
      contentHash,
      durationMs: Math.round(audioBuffer.duration * 1000),
      sampleRate: audioBuffer.sampleRate,
      channelCount: audioBuffer.numberOfChannels,
    };
  } finally {
    if (compressedPtr) api.module._free(compressedPtr);
    if (ctx) api.free(ctx);
    api.module._free(samplesPtr);
  }
}

/**
 * Returns just the SHA-256 of the file's raw bytes — the fallback path
 * library.ts takes when fingerprintFile() throws
 * FingerprinterUnavailableError. Cheap, no WASM, no decode.
 */
export async function hashOnly(file: File | Blob): Promise<{ contentHash: string }> {
  const bytes = await file.arrayBuffer();
  return { contentHash: await sha256Hex(bytes) };
}

/**
 * Eagerly warm the chromaprint module so the first fingerprint call
 * doesn't pay the load cost on the user's interaction. Safe to call
 * multiple times. Returns true if the module is ready, false if it's
 * unavailable (so the UI can pre-hide the fingerprint progress chip).
 */
export async function preloadFingerprinter(): Promise<boolean> {
  try {
    await loadChromaprint();
    return true;
  } catch (err) {
    if (err instanceof FingerprinterUnavailableError) return false;
    throw err;
  }
}
