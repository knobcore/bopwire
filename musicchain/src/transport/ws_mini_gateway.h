#pragma once
//
// ws_mini_gateway.h — unified JSON-envelope + binary-frame WebSocket
// gateway for browser clients that connect directly to the mini-node
// (no librats-WASM on the browser side).
//
// Browsers can't talk librats (no UDP, no DHT, no native socket layer
// at all that exposes raw frames). The previous solution — running
// librats-WASM in the browser and shimming its TCP sockets through
// the ws_tcp_relay byte pump — was heavy and prone to NAT / handshake
// quirks. WsMiniGateway replaces that approach with a single
// WebSocket surface that speaks the mini-node's JSON RPC envelope
// directly. The browser side becomes a thin client: just JSON in,
// JSON or binary frames back.
//
// Wire protocol (see ws_mini_gateway.cpp for the full spec):
//
//   * URL  : ws://<vps>:<port>/   (default 8082)
//   * Any subprotocol accepted; we ignore Sec-WebSocket-Protocol.
//   * RFC 6455 framing. TEXT frames carry JSON envelopes; BINARY
//     frames out carry audio chunks the audio.fetch handler hands
//     us. Inbound binary frames are rejected.
//   * JSON envelope shape: {"req_id", "type", "body"}.
//
// Dispatch:
//
//   * "routes.get" / "mininodes.list" / "mini.hello" / "stun.observe"
//     — answered locally from the mini-node's state by stub handlers
//     that pull from the same globals the librats RPC handler uses.
//     Each verb maps to an inline lambda in ws_mini_gateway.cpp.
//   * "audio.fetch" — delegated to a hook a separate agent installs
//     (set_audio_fetch_handler); the hook owns the binary streaming
//     for the duration of the request and ends with a "complete"
//     envelope on the same WS connection.
//   * Anything else — wrapped as `relay.forward {target_peer_id,
//     type, body}` over the existing librats client (g_client) to
//     some full node. Replies (`musicchain.reply` carrying the
//     re-injected req_id) are routed back to this WS connection
//     via a static req_id → conn registry.
//
// Threading: one accept thread on the listen socket, plus one I/O
// thread that drives select() across every accepted connection.
// JSON dispatch runs synchronously on the I/O thread; long-running
// handlers (notably audio.fetch) should hand control to their own
// worker via the binary-frame helper exposed below.
//

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

// Forward declare the librats client handle so this header doesn't
// drag deps/librats/src/librats_c.h into every translation unit.
// The .cpp file includes the real header; the typedef is the same.
typedef void* rats_client_t;

namespace mc::transport {

/// Stable identifier for one upgraded WebSocket connection. Lifetimes:
/// allocated when the upgrade completes, freed when the I/O loop
/// closes the connection. Handler hooks (audio.fetch in particular)
/// should treat the id as opaque and call WsMiniGateway::sendBinary /
/// sendText with it.
using WsMiniConnId = uint64_t;

/// Handler signature for the audio.fetch verb. The hook receives the
/// connection id (so it can stream binary chunks back), the parsed
/// JSON request envelope, and the librats client (so it can talk to
/// the swarm peer). The hook is expected to drive the whole flow
/// (send stream.open, accept the peer's reply via dispatch_reply
/// / dispatch_chunk on the audio bridge — or however the new agent
/// wires it up) and finish by calling WsMiniGateway::finishRequest
/// when done. If no hook is installed, audio.fetch returns a
/// "not_implemented" envelope.
using AudioFetchHandler = std::function<void(
    WsMiniConnId conn_id,
    const std::string& req_id,
    const std::string& peer_id_hex,    // 40-hex swarm peer
    const std::string& content_hash    // 64-hex song hash
)>;

class WsMiniGateway {
public:
    WsMiniGateway() = default;
    ~WsMiniGateway();

    WsMiniGateway(const WsMiniGateway&) = delete;
    WsMiniGateway& operator=(const WsMiniGateway&) = delete;

    /// Bind + listen on `port` (0.0.0.0). Stores `rats` so dispatch
    /// can `rats_send_message` for forwards. Spawns the I/O thread.
    /// Returns true on success. Repeat calls without an intervening
    /// stop() are no-ops.
    bool start(uint16_t port, rats_client_t rats);

    /// Tell the I/O thread to exit, close every connection. Joins
    /// the worker.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    uint16_t port() const { return port_; }

    // ---- Outbound helpers used by handler hooks ------------------
    //
    // Each one is safe to call from any thread; they enqueue the
    // frame onto the connection's outbox and the I/O loop flushes
    // it the next tick. Returns false if the conn_id is no longer
    // registered (e.g. the browser hung up).

    /// Send a binary WebSocket frame to one connection. Used by the
    /// audio.fetch handler to stream raw audio bytes out.
    static bool sendBinary(WsMiniConnId conn_id,
                           const uint8_t* data, size_t size);

    /// Send a text frame carrying an arbitrary string (usually a JSON
    /// envelope). Used by handlers to send intermediate or final
    /// status envelopes.
    static bool sendText(WsMiniConnId conn_id, const std::string& text);

    /// Mark the connection for close after the outbox drains. Lets a
    /// handler signal "I'm done; please close".
    static bool closeConn(WsMiniConnId conn_id);

    // ---- Hook installation ---------------------------------------

    /// Install the audio.fetch handler. Pass `nullptr` to reset to
    /// the built-in "not_implemented" reply.
    static void set_audio_fetch_handler(AudioFetchHandler handler);

private:
    std::atomic<bool> running_{false};
    std::thread       io_thread_;
    uint16_t          port_ = 0;
    // Native socket kept as uintptr_t so this header doesn't drag
    // winsock / sys/socket into every translation unit.
    uintptr_t         listen_fd_ = static_cast<uintptr_t>(-1);
    rats_client_t     rats_ = nullptr;

    void io_loop_();
};

/// Dispatch hook the mini-node main calls when an inbound librats
/// `musicchain.reply` envelope might be a relayed answer to a gateway
/// JSON request. Returns true if the gateway claimed it (and forwarded
/// to the originating browser); false to let other consumers see it.
bool ws_mini_gateway_dispatch_reply(const std::string& req_id,
                                    const std::string& envelope_json);

// Lookups the mini-node main defines for the gateway + handlers to call
// into. Defined in tools/mini_node.cpp; declared here so any
// translation unit that includes this header (the gateway, the audio
// fetch handler, etc.) can resolve them. Don't add stateful helpers
// here — pure read-only views over the mini-node's own state only.
namespace mini_state {

/// Snapshot of the routes table as JSON. Same shape `routes_json()`
/// returns over the librats RPC path.
std::string routes_json();

/// JSON array of every known mini-node peer (rats_peer_id +
/// public_address) plus self. Same shape `mininodes.list` returns.
std::string mininodes_list_json();

/// Count of routes currently in the table — read for `mini.hello`.
uint64_t routes_count();

/// Pick a full-node peer to send a relay.forward at. Returns the
/// peer's rats_peer_id, or "" if no full node currently reachable.
/// Selection policy: any direct route's rats_peer_id is preferred
/// over an Unknown / Relay route.
std::string pick_full_node_peer_id();

/// This mini-node's own librats peer id, or "" if not running.
std::string our_peer_id();

} // namespace mini_state

} // namespace mc::transport
