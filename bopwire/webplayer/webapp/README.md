# Bopwire web player (Vite + React + Tailwind v4)

The redesigned browser player for bopwire.com. Replaces `../frontend/`
(vanilla JS, kept for reference) with the same feature set on a modern stack:

- **Vite 6** — dev server + static production build (`dist/`, relative asset
  paths, drop-in behind Caddy exactly like the old frontend).
- **React 19** — views (Home / Browse / List), state, and the search drill.
- **Tailwind CSS v4** — design tokens in `src/index.css` (`@theme`), same
  dark/mint/sky language as the native app.
- **No framework in the audio path** — `src/player.js` is a plain singleton
  (ported verbatim from the legacy app.js) that drives the WASM decoders
  (`public/decoders/*`, `public/wasm-player.js`, loaded as plain globals) with
  the `<audio>` fallback + watchdog, and the reward-session lifecycle
  (start / heartbeat / complete / sendBeacon).

Deterministic cover art (`src/art.js`) is byte-for-byte the same algorithm as
the Flutter client (`lib/src/widgets/cover_art.dart`) — keep them in lockstep.

## Develop

```sh
npm install
npm run dev                     # vite dev server
# in another shell: python ../mock_gateway.py   (fake /api on :8091)
# open http://localhost:5173/?gateway=http://localhost:8091
```

## Build + deploy

```sh
npm run build                   # → dist/
# deploy dist/ as the static site (replaces the old frontend/ assets)
```

The gateway URL defaults to `https://api.bopwire.com`; override at runtime
with `?gateway=http://localhost:8090` — no rebuild needed.
