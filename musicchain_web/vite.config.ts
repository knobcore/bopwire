import { defineConfig } from 'vite';
import { resolve } from 'node:path';

// Vite dev + production build config.
//
// - root      = `public/` so index.html is served at /
// - publicDir = `wasm/`   so the WASM module + glue are served at /wallet.*
//   alongside the JS bundle
// - The dev server exposes a /musicchain WebSocket proxy → mini-node
//   gateway, which the browser NodeClient connects to for chain RPC.
//   Default target is the live VPS mini-node (ws://85.239.238.226:8082);
//   override with MUSICCHAIN_MINI_WS for local dev (e.g. point at the
//   home node's ws_bridge on ws://localhost:9090).
//
// To run the dev server: `bash build.sh && npm install && npm run dev`
export default defineConfig({
  root: resolve(__dirname, 'public'),
  publicDir: resolve(__dirname, 'wasm'),
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
      '@wasm': resolve(__dirname, 'wasm'),
    },
  },
  build: {
    outDir: resolve(__dirname, 'dist'),
    emptyOutDir: true,
    sourcemap: true,
    target: 'es2022',
  },
  server: {
    port: 5173,
    strictPort: true,
    headers: {
      // SharedArrayBuffer requires these for libwally's wasm threads,
      // though we ship a single-threaded build by default so they're
      // primarily defensive against future multi-threaded WASMs.
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    // Forward WebSocket upgrade requests on /musicchain to a
    // MINI-NODE WebSocket gateway (the lightweight relay we deploy
    // on VPSes). The browser's NodeClient defaults to
    // wss://<host>/musicchain, which on the dev server becomes
    // ws://localhost:5173/musicchain — this proxy hop is what makes
    // that URL actually reach a mini-node out on the Internet.
    //
    // Default target is the live VPS mini-node at
    // 85.239.238.226:8082. Override by exporting
    // MUSICCHAIN_MINI_WS=ws://localhost:9090 before `npm run dev`
    // to point the dev browser at the home node's ws_bridge for
    // local testing (port 9090), or at a different mini-node.
    //
    // History: prior to 2026-06-21 this proxy targeted the home
    // node's ws_bridge (ws_port 9090) by default and was driven by
    // MUSICCHAIN_HOME. The browser client is now expected to talk
    // to mini-node gateways, so the default flipped to the VPS.
    proxy: {
      '/musicchain': {
        target: process.env['MUSICCHAIN_MINI_WS'] ?? 'ws://85.239.238.226:8082',
        ws: true,
        rewrite: () => '/',
        changeOrigin: true,
      },
    },
  },
});
