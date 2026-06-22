#pragma once
//
// ws_tcp_relay.h — transparent WebSocket-to-TCP byte pump for the
// mini-node.
//
// Browser-side librats compiled with emscripten's `-lwebsocket.js`
// translates every BSD `connect(sockfd, host, port)` call into a
// `new WebSocket("ws://host:port/")` (or wss://) — the JavaScript
// shim then ferries application bytes over binary WebSocket frames
// in BOTH directions. Each WS frame is one chunk of opaque payload
// the application is expected to forward verbatim — there is no
// length prefix, no JSON envelope, no protocol on top.
//
// `WsTcpRelay` is the matching server side: it listens on a public
// TCP port, performs the RFC 6455 upgrade handshake, and then pumps
// raw bytes between the upgraded WebSocket and a fresh outbound TCP
// socket to `localhost:tcp_target_port` (the mini-node's librats
// listener on `cfg.rats_port`, by default 8080).
//
// This is intentionally NOT the same machinery as `ws_bridge.cpp` —
// that gateway speaks JSON envelopes into `RatsApi::dispatch_for_
// bridge`. The relay here is dumber and lower-level: WS bytes in →
// TCP bytes out, no parsing.
//
// Threading model: one accept thread plus one worker thread per
// upgraded connection. Each worker uses `select()` so it can wake
// on either side of the byte pump. The connection thread cleans
// up its own pair of sockets on exit.
//
// Limits / not implemented:
//   * 16 MiB cap per incoming WS frame (browser librats chunks well
//     below this).
//   * No fragmentation across many frames on the read side — each
//     frame is forwarded as a single write() call.
//   * No TLS — production deploys terminate wss:// at nginx /
//     Cloudflare / Caddy in front of this listener.
//   * No subprotocol pinning. Emscripten typically sends
//     "binary" but we accept anything.
//
// Manual end-to-end test plan:
//
//   1. Start the mini-node:
//        musicchain-mini-node --rats-port 8080
//      It binds the librats TCP listener on 8080 AND
//      the WsTcpRelay on 8081 (the default).
//
//   2. From a browser (or a Node.js process with the `ws` package),
//      open `ws://<mini-node-host>:8081/`. Send a binary frame
//      containing the librats handshake bytes a fresh TCP peer
//      would send. Verify the librats listener sees a new peer
//      and that bytes flowing back appear as binary WebSocket
//      frames on the client.
//
//   3. The browser-side emscripten shim produces the URL string
//      `ws://<host>:<port>/` for every `connect()` call. The
//      mini-node operator points the browser librats at:
//        ws://<vps-public-ip>:8081/
//      (TLS-terminated by the front proxy: wss://.../)
//

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace mc::transport {

/// `WsTcpRelay` pumps raw bytes between a browser WebSocket and a
/// real TCP listener on the same host.
///
/// Use:
///   WsTcpRelay relay;
///   relay.start(8081, 8080);  // listen on 8081, forward to 8080
///   ...
///   relay.stop();
class WsTcpRelay {
public:
    WsTcpRelay() = default;
    ~WsTcpRelay();

    WsTcpRelay(const WsTcpRelay&) = delete;
    WsTcpRelay& operator=(const WsTcpRelay&) = delete;

    /// Bind + listen on `ws_port` (0.0.0.0). Every accepted
    /// upgrade opens a paired outbound TCP socket to
    /// `127.0.0.1:tcp_target_port`. Spawns the accept thread.
    /// Returns true on success. Repeat calls without an
    /// intervening stop() are no-ops.
    bool start(uint16_t ws_port, uint16_t tcp_target_port);

    /// Tell the accept thread to exit, close the listen socket.
    /// Per-connection worker threads detect the stop flag and
    /// drop their sockets shortly after.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    uint16_t ws_port()      const { return ws_port_; }
    uint16_t tcp_port()     const { return tcp_target_port_; }

private:
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
    uint16_t          ws_port_         = 0;
    uint16_t          tcp_target_port_ = 0;
    // Native socket kept as uintptr_t so the header doesn't drag
    // winsock / sys/socket into every translation unit.
    uintptr_t         listen_fd_ = static_cast<uintptr_t>(-1);

    void accept_loop_();
};

} // namespace mc::transport
