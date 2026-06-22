#pragma once
//
// ws_bridge.h — minimal WebSocket-over-TCP bridge for the web player.
//
// STATUS: LEGACY / OPTIONAL.
//
// As of 2026-06-21 the canonical path for browsers is:
//
//     browser → mini-node WS gateway (default ws://<vps>:8082)
//               → relay.forward over rats → home node
//
// i.e. the web player no longer connects to this bridge in the
// default deployment. The mini-node gateway lives in a separate
// process and is what the Vite dev proxy and production frontend
// both point at (see musicchain_web/vite.config.ts).
//
// This bridge is kept operational because:
//   * Full-node operators may want to expose ws:// directly to
//     browsers on their own LAN during local development (no need
//     to spin up a mini-node).
//   * The dispatch path it exercises (RatsApi::dispatch_for_bridge +
//     g_ws_reply_sink) is shared with other internal callers and
//     should keep getting integration test coverage.
//
// If you are looking for the production browser entry point: it is
// NOT here. Go to the mini-node gateway implementation. Treat any
// new feature work on this file as opt-in for full nodes that want
// to self-serve the web player.
//
// ----------------------------------------------------------------
//
// Browsers can't talk librats (no UDP, no DHT, no native socket layer
// at all that exposes raw frames). They CAN talk WebSocket. This
// listener accepts ws:// upgrades from the browser, parses each
// inbound frame as a JSON envelope `{req_id, type, body}` and routes
// it into RatsApi::dispatch_for_bridge() — the same verb surface every
// other peer hits, including the swarm / chain / session verbs.
// Replies are JSON envelopes pushed back over the same socket.
//
// Single-threaded select() loop. Per-connection state lives in
// WsConn (handshake buffer, frame assembly buffer, outbound queue).
// The loop runs on its own thread so the rest of the node doesn't
// block on browser traffic. No TLS — production deploys put this
// behind nginx / Caddy / Cloudflare for wss://; in-house dev hits
// ws:// directly.
//
// Frame protocol implemented: RFC 6455 client-to-server text frames
// up to 64 KiB (payload length 0/126 forms; 127 / 64-bit is rejected
// to keep the parser small — env arrives < 16 KiB). Server-to-client
// frames are always unmasked text per spec. Continuation, binary,
// close, ping, pong opcodes are handled.
//
// SHA-1 for the Sec-WebSocket-Accept header comes from OpenSSL,
// which is already linked into the static musicchain library — no
// new dependency.
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace mc::api { class RatsApi; }

namespace mc::transport {

/// `WsBridge` opens a TCP listen socket on `port`, accepts WebSocket
/// upgrade requests, and forwards every JSON envelope to a dispatch
/// callback. The bridge is stateless beyond per-connection frame
/// reassembly — there's no auth, no session, no rate-limit (yet).
///
/// Use:
///   WsBridge bridge(api);
///   bridge.start(9090);
///   ...
///   bridge.stop();
class WsBridge {
public:
    explicit WsBridge(mc::api::RatsApi& api);
    ~WsBridge();

    /// Bind + listen on `port` (0.0.0.0). Spawns an accept/IO thread.
    /// Returns true on success. Repeat calls without an intervening
    /// stop() are no-ops.
    bool start(uint16_t port);

    /// Tell the IO thread to exit, close every connection, and
    /// release the listen socket. Joins the thread.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    uint16_t port() const   { return port_; }

private:
    mc::api::RatsApi&      api_;
    std::atomic<bool>      running_{false};
    std::thread            io_thread_;
    uint16_t               port_ = 0;
    // Native socket type kept as uintptr_t so we don't drag winsock or
    // sys/socket into the header.
    uintptr_t              listen_fd_ = static_cast<uintptr_t>(-1);

    void io_loop_();
};

} // namespace mc::transport
