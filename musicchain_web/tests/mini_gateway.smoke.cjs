// mini_gateway.smoke.cjs — Smoke test for the mini-node WebSocket gateway.
//
// The mini-node exposes a plain JSON-over-WebSocket gateway on
// ws://85.239.238.226:8082/ that serves musicchain RPC verbs WITHOUT
// going through the librats WASM layer. This is the path browser
// clients use when they want routing info without booting the wasm
// transport (e.g. a thin status pane, a CLI, this smoke).
//
// Steps:
//   1. Dial ws://85.239.238.226:8082/ with the `ws` npm package (same
//      package the wasm WebSocket polyfill uses; already pinned at
//      8.21.0 in node_modules, installed via `npm install --no-save ws`).
//   2. Send a single text frame containing the JSON envelope
//        {"req_id": "<random hex>", "type": "routes.get", "body": {}}
//      This is the verb the browser uses to discover home nodes; the
//      mini-node answers it directly from its local route table.
//   3. Wait for one text frame back, JSON-parse it.
//   4. Assert envelope.req_id matches what we sent, envelope.status === "ok"
//      and envelope.body.peers is a non-empty array.
//   5. Print a one-line summary of each route (peer_id / reachability /
//      public_address) and exit 0.
//
// Exits non-zero with a clear "[smoke] FAIL: ..." line on any failure:
//   - WebSocket dial error / refusal       → gateway not listening
//   - Reply timeout (default 10s)          → gateway accepted but never replied
//   - Bad JSON / missing fields            → wire format regression
//   - status !== "ok" or peers empty       → gateway up but no home nodes
//                                            registered (mini-node alive,
//                                            but the chain is empty)
//
// Verb under test: routes.get
//   request body:  {} (no parameters)
//   reply body:    { peers: [
//                     { rats_peer_id: "<40 hex>",
//                       reachability: "public"|"private"|...,
//                       public_address: "ip:port" },
//                     ...
//                   ] }
//
// Run:  node tests/mini_gateway.smoke.cjs

'use strict';

const crypto = require('crypto');

let WebSocketCtor;
try {
  const WS = require('ws');
  WebSocketCtor = WS.WebSocket || WS;
} catch (e) {
  console.error('[smoke] FAIL: the `ws` npm package is required for the Node smoke.');
  console.error('         Install with: npm install --no-save ws');
  process.exit(2);
}

const GATEWAY_URL  = 'ws://85.239.238.226:8082/';
const DIAL_TIMEOUT = 10_000;
const REPLY_TIMEOUT = 10_000;

function newReqId() {
  return crypto.randomBytes(8).toString('hex');
}

function fail(msg) {
  console.error('[smoke] FAIL: ' + msg);
  process.exit(1);
}

async function main() {
  console.log('[smoke] mini-node WebSocket gateway probe');
  console.log(`[smoke] target: ${GATEWAY_URL}`);
  console.log('[smoke] verb:   routes.get');

  const reqId = newReqId();
  const envelope = { req_id: reqId, type: 'routes.get', body: {} };
  const frame = JSON.stringify(envelope);

  // 1. Dial -------------------------------------------------------------
  console.log('[smoke] dialing…');
  const ws = new WebSocketCtor(GATEWAY_URL);

  await new Promise((resolve, reject) => {
    const dialTimer = setTimeout(() => {
      try { ws.terminate ? ws.terminate() : ws.close(); } catch (_) {}
      reject(new Error(`dial timeout after ${DIAL_TIMEOUT}ms — gateway not answering on ${GATEWAY_URL}`));
    }, DIAL_TIMEOUT);
    ws.once('open', () => { clearTimeout(dialTimer); resolve(); });
    ws.once('error', (err) => {
      clearTimeout(dialTimer);
      reject(new Error(`dial error: ${err && err.message ? err.message : err}`));
    });
  }).catch((e) => fail(e.message));

  console.log('[smoke] connected');

  // 2. Send envelope ----------------------------------------------------
  console.log(`[smoke] → ${frame}`);
  await new Promise((resolve, reject) => {
    ws.send(frame, (err) => (err ? reject(err) : resolve()));
  }).catch((e) => {
    try { ws.terminate ? ws.terminate() : ws.close(); } catch (_) {}
    fail(`send failed: ${e.message}`);
  });

  // 3. Await one reply --------------------------------------------------
  const reply = await new Promise((resolve, reject) => {
    const replyTimer = setTimeout(() => {
      reject(new Error(`reply timeout after ${REPLY_TIMEOUT}ms — gateway accepted the WS handshake but never answered routes.get`));
    }, REPLY_TIMEOUT);
    ws.once('message', (data, isBinary) => {
      clearTimeout(replyTimer);
      if (isBinary) {
        reject(new Error('expected a text frame, got a binary frame'));
        return;
      }
      const text = (typeof data === 'string') ? data : data.toString('utf8');
      resolve(text);
    });
    ws.once('close', (code, reason) => {
      clearTimeout(replyTimer);
      reject(new Error(`socket closed before reply: code=${code} reason="${reason || ''}"`));
    });
    ws.once('error', (err) => {
      clearTimeout(replyTimer);
      reject(new Error(`socket error before reply: ${err && err.message ? err.message : err}`));
    });
  }).catch((e) => {
    try { ws.terminate ? ws.terminate() : ws.close(); } catch (_) {}
    fail(e.message);
  });

  console.log(`[smoke] ← ${reply.length > 400 ? reply.substring(0, 400) + '… (' + reply.length + ' bytes)' : reply}`);

  try { ws.close(); } catch (_) {}

  // 4. Validate ---------------------------------------------------------
  let env;
  try {
    env = JSON.parse(reply);
  } catch (e) {
    fail(`reply is not valid JSON: ${e.message}`);
  }

  if (!env || typeof env !== 'object') {
    fail(`reply is not a JSON object: ${reply}`);
  }
  if (env.req_id !== reqId) {
    fail(`reply req_id mismatch: sent="${reqId}" got="${env.req_id}"`);
  }
  if (env.status !== 'ok') {
    fail(`reply status="${env.status}" (expected "ok"); error="${env.error || '(none)'}"`);
  }
  const body = env.body || {};
  const peers = body.peers;
  if (!Array.isArray(peers)) {
    fail(`reply body.peers is not an array: ${JSON.stringify(body)}`);
  }
  if (peers.length === 0) {
    fail('reply body.peers is EMPTY — gateway alive but no home nodes registered.\n' +
         '  • The home node has not published a route to this mini-node.\n' +
         '  • Check the home node TUI/log for "[rats] VPS link up" / "publishing route".');
  }

  // 5. Summary ----------------------------------------------------------
  console.log(`[smoke] routes.get returned ${peers.length} peer(s):`);
  for (const p of peers) {
    const pid   = (p.rats_peer_id || p.peer_id || '').substring(0, 16);
    const reach = p.reachability || '?';
    const pub   = p.public_address || p.address || '?';
    console.log(`         - ${pid}…  reach=${reach}  pub=${pub}`);
  }

  console.log('[smoke] PASS — mini-node WebSocket gateway is alive');
  process.exit(0);
}

main().catch((err) => {
  console.error('[smoke] uncaught:', err && err.stack ? err.stack : err);
  process.exit(1);
});
