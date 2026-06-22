// Vite-served entry. The HTML's <script type="module" src="/main.ts">
// points here (because index.html lives under /public, which Vite serves
// at /). All real bootstrap logic — wallet WASM preload, NodeClient
// connect, wallet-gate → home-shell handoff — lives in ../src/main.ts so
// the typechecker (tsconfig `include: ["src/**/*.ts"]`) sees it. Keep
// this file as a single import so adding logic here doesn't accidentally
// bypass the typecheck.
import '../src/main.ts';
