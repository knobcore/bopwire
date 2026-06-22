import { defineConfig } from 'vitest/config';
import { resolve } from 'node:path';

// Vitest config for the smoke-test suite. We re-state the @/@wasm
// aliases here (rather than `extends: './vite.config.ts'`) because the
// main Vite config sets `root: 'public/'` for the dev server, which
// vitest doesn't need and which would change how it resolves the
// project's tsconfig + node_modules.
//
// Tests run in the Node environment (`environment: 'node'`). The
// wallet.wasm payload is loaded from disk in tests/wallet.test.ts via
// `__setWalletModuleOptionsForTest({ wasmBinary })` so the Emscripten
// glue doesn't try to fetch() the .wasm file (which would fail under
// Node, where there's no browser fetch and no served origin).
export default defineConfig({
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
      '@wasm': resolve(__dirname, 'wasm'),
    },
  },
  test: {
    environment: 'node',
    // tests/qrcode.test.ts is a hand-rolled `process.exit(1)` script
    // pre-dating this vitest setup — it runs under `npx tsx` directly
    // and would crash the vitest runner on the first assert. Only pick
    // up files that use the vitest API (describe/it).
    include: ['tests/wallet.test.ts'],
    // The first WASM compile under vitest is slow (~1.5s on a cold
    // CPU). Bump the default per-test timeout so the address-derivation
    // test doesn't flake when run alongside a hot typescript-eslint pass.
    testTimeout: 15_000,
    hookTimeout: 15_000,
  },
});
