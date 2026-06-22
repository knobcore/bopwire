// rats.smoke.cjs — Node smoke test for the librats WebAssembly build.
//
// The web player consumes librats through Module.cwrap() against the
// librats_c.h C ABI, exactly the way the Android JNI library + Windows
// DLL do. This smoke test exercises the bare minimum of that surface
// to prove the build actually links and runs:
//
//   1. Load the ES-module rats.js + the sibling rats.wasm via Node's
//      dynamic `import()` from inside CommonJS (Node 18+ supports the
//      ESM-from-CJS bridge).
//   2. Instantiate the module factory `createRatsModule({...})`. The
//      factory promise resolves once the wasm is compiled and the
//      embedded runtime (FS, SOCKFS, TTY) is initialised.
//   3. cwrap a handful of rats_* entry points and call them:
//        - rats_set_logging_enabled(0)            — silence the build
//        - rats_set_console_logging_enabled(0)
//        - rats_create(0)                          — listen_port=0 →
//                                                    librats picks an
//                                                    OS-assigned port,
//                                                    no real bind needed
//                                                    in wasm
//        - rats_get_our_peer_id(client)           — returns a freshly
//                                                    generated 40-char
//                                                    hex peer ID
//        - rats_string_free(peer_id)              — drop the strdup'd
//                                                    buffer
//        - rats_destroy(client)                   — tear down
//
// If any step throws, the test fails with a non-zero exit code. If
// rats_get_our_peer_id returns a non-empty string that looks like a
// hex peer ID, the test passes.
//
// Run:  node tests/rats.smoke.cjs

'use strict';

const fs = require('fs');
const path = require('path');
const url = require('url');

async function main() {
  const wasmJsPath = path.resolve(__dirname, '..', 'wasm', 'rats.js');
  const wasmBinPath = path.resolve(__dirname, '..', 'wasm', 'rats.wasm');

  // The Emscripten ES module is built with ENVIRONMENT=web,worker — it
  // doesn't carry the Node-side `readBinary()` loader, so locateFile()
  // alone isn't enough. Read the wasm bytes ourselves and pass them
  // through Module.wasmBinary; the factory's instantiator picks that
  // up before falling through to the fetch/file path.
  const wasmBytes = fs.readFileSync(wasmBinPath);

  const factoryModule = await import(url.pathToFileURL(wasmJsPath).href);
  const createRatsModule = factoryModule.default;
  if (typeof createRatsModule !== 'function') {
    throw new Error('rats.js did not export a default factory function');
  }

  const Module = await createRatsModule({
    wasmBinary: wasmBytes,
    locateFile: (file) => {
      if (file === 'rats.wasm') return wasmBinPath;
      return file;
    },
    // Silence the runtime's stdout under success; on failure the
    // factory will throw before this gets a chance to swallow real
    // crash text.
    print: () => {},
    // Pipe wasm-side stderr through so any future assertion message
    // (when we re-enable -sASSERTIONS) shows up in the test log.
    printErr: (msg) => console.error('[wasm-stderr]', msg),
  });

  // Sanity-check the bits of the EXPORTED_RUNTIME_METHODS surface the
  // TypeScript wrapper will use. If any of these are undefined the
  // build flags drifted out from under us.
  for (const fn of ['cwrap', 'ccall', 'UTF8ToString', 'HEAPU8']) {
    if (Module[fn] === undefined) {
      throw new Error(`Module.${fn} missing — EXPORTED_RUNTIME_METHODS misconfigured`);
    }
  }

  // cwrap the small slice of librats_c.h the test drives. Signatures
  // mirror the upstream header: rats_create returns rats_client_t
  // (opaque pointer, modeled as 'number' in wasm32), rats_destroy
  // returns void, rats_get_our_peer_id returns char* the caller must
  // free with rats_string_free.
  const ratsSetLoggingEnabled = Module.cwrap('rats_set_logging_enabled', null, ['number']);
  const ratsSetConsoleLoggingEnabled = Module.cwrap('rats_set_console_logging_enabled', null, ['number']);
  const ratsCreate = Module.cwrap('rats_create', 'number', ['number']);
  const ratsDestroy = Module.cwrap('rats_destroy', null, ['number']);
  const ratsGetOurPeerId = Module.cwrap('rats_get_our_peer_id', 'number', ['number']);
  const ratsStringFree = Module.cwrap('rats_string_free', null, ['number']);
  const ratsGetPeerCount = Module.cwrap('rats_get_peer_count', 'number', ['number']);
  const ratsGetVersionString = Module.cwrap('rats_get_version_string', 'number', []);

  // 1. Mute the logger before doing anything else — librats writes a
  //    banner on first create() otherwise.
  ratsSetLoggingEnabled(0);
  ratsSetConsoleLoggingEnabled(0);

  // 2. Read the library version so we know we're talking to the real
  //    librats_c.cpp and not a stale shim.
  const versionPtr = ratsGetVersionString();
  if (!versionPtr) {
    throw new Error('rats_get_version_string returned null');
  }
  const versionStr = Module.UTF8ToString(versionPtr);
  console.log(`[smoke] librats version = ${versionStr}`);

  // 3. Construct a client. listen_port=0 → bind to ephemeral.
  const client = ratsCreate(0);
  if (!client) {
    throw new Error('rats_create returned NULL — engine failed to initialise');
  }
  console.log(`[smoke] rats_create OK, handle=0x${client.toString(16)}`);

  // 4. Peer ID — generated deterministically from the client's local
  //    state when create() runs.
  const peerIdPtr = ratsGetOurPeerId(client);
  if (!peerIdPtr) {
    ratsDestroy(client);
    throw new Error('rats_get_our_peer_id returned NULL');
  }
  const peerId = Module.UTF8ToString(peerIdPtr);
  ratsStringFree(peerIdPtr);
  console.log(`[smoke] our peer id = ${peerId}`);
  if (!peerId || peerId.length < 8) {
    ratsDestroy(client);
    throw new Error(`peer id looks degenerate: "${peerId}"`);
  }

  // 5. Confirm we have zero peers — we never called start() / connect().
  const peerCount = ratsGetPeerCount(client);
  console.log(`[smoke] peer count = ${peerCount}`);
  if (peerCount !== 0) {
    ratsDestroy(client);
    throw new Error(`expected 0 peers in fresh client, got ${peerCount}`);
  }

  // 6. Tear the client back down. Must complete without throwing.
  ratsDestroy(client);
  console.log('[smoke] rats_destroy OK');

  console.log('[smoke] PASS');
}

main().catch((err) => {
  console.error('[smoke] FAIL:', err && err.stack ? err.stack : err);
  process.exit(1);
});
