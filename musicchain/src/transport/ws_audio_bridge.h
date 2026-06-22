#pragma once
//
// ws_audio_bridge.h — browser-facing WebSocket bridge that pulls audio
// bytes off a swarm peer (a phone, a home node, another mini-node) and
// streams them back over a single WebSocket connection.
//
// The bridge is the third WebSocket-shaped surface on the mini-node:
//
//   * ws_bridge.cpp        — JSON-envelope verb surface (home node only,
//                            speaks RatsApi::dispatch_for_bridge).
//   * ws_tcp_relay.cpp     — transparent byte pump from browser-librats
//                            to the local librats TCP listener.
//   * ws_audio_bridge.cpp  — this file. JSON request in, binary audio
//                            chunks back, terminated by a JSON
//                            "complete" envelope.
//
// Browsers don't talk librats. They CAN talk WebSocket. A web player
// that needs the audio bytes for a song opens a single WS connection
// to the mini-node, sends one `audio.fetch` envelope naming the peer
// and content_hash, and reads back binary frames carrying the raw
// audio bytes (decode-ready, no per-chunk envelope) until the bridge
// closes the conversation with a "complete" text frame.
//
// Threading model mirrors WsTcpRelay: one accept thread + one worker
// thread per upgraded connection, each driving its own select() loop
// so a slow browser can't stall the others. Binary chunks arriving
// off the librats backplane (via the mini-node's existing
// `rats_set_binary_callback`) are demultiplexed to the right worker
// via a static stream_id → WsConn registry — see register_stream /
// dispatch_chunk in the .cpp file.
//
// See ws_audio_bridge.cpp for the full wire-format / error-envelope
// spec.
//

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <thread>

// Forward declare the librats client handle so the header doesn't drag
// the entire librats_c API into every translation unit. The .cpp file
// includes the real librats_c.h; the typedef there is identical (also
// `void*`), and C++ allows the same typedef-name to be re-declared to
// the same type in the same TU.
typedef void* rats_client_t;

namespace mc::transport {

/// `WsAudioBridge` accepts `ws://<vps>:<port>/` connections from
/// browsers, parses an `audio.fetch` JSON envelope, then streams the
/// requested song bytes back over the same WebSocket.
///
/// Use:
///   WsAudioBridge bridge;
///   bridge.start(8082, g_client);   // port + the mini-node's librats
///                                   // handle (the same handle used
///                                   // for relay forwards / replies).
///   ...
///   bridge.stop();
///
/// Tightly bound to the mini-node's existing librats callback wiring:
/// when a binary chunk arrives on `rats_set_binary_callback` that
/// doesn't carry the relay-binary tag, the mini-node's relay handler
/// hands it to `dispatch_chunk()` so the bridge can demux it onto the
/// right WS connection. Same goes for `dispatch_reply()` — the
/// mini-node's `on_relay_reply` calls it for `musicchain.reply`
/// envelopes whose req_id didn't match a pending relay (i.e. the
/// candidate audio.fetch reply lives here).
class WsAudioBridge {
public:
    WsAudioBridge() = default;
    ~WsAudioBridge();

    WsAudioBridge(const WsAudioBridge&) = delete;
    WsAudioBridge& operator=(const WsAudioBridge&) = delete;

    /// Bind + listen on `port` (0.0.0.0). Stores `rats` so worker
    /// threads can `rats_send_message` and `rats_send_binary`. Spawns
    /// the accept thread. Returns true on success. Repeat calls
    /// without an intervening stop() are no-ops.
    bool start(uint16_t port, rats_client_t rats);

    /// Tell the accept thread to exit, close the listen socket. Per-
    /// connection worker threads observe the stop flag and drop their
    /// sockets shortly after.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    uint16_t port() const   { return port_; }

    // ---- mini-node integration hooks --------------------------------
    //
    // These two static entry points let the mini-node's existing
    // librats callbacks ferry traffic into whichever WsAudioBridge is
    // currently running. They are no-ops when no bridge is active.

    /// Try to match a `musicchain.reply` envelope to a pending
    /// audio.fetch open. Returns true if the reply was consumed by the
    /// bridge (the mini-node should drop it from its own dispatch);
    /// false to let the rest of the pipeline have a crack.
    static bool dispatch_reply(const std::string& req_id,
                               const std::string& envelope_json);

    /// Try to demux a direct (non-relay-tagged) binary chunk into a
    /// known stream. Returns true if the chunk matched a registered
    /// stream_id; false otherwise. The buffer layout is the raw librats
    /// wire format: bytes 0..3 = stream_id LE, 4..7 = seq LE, 8 = eof
    /// flag, 9..N = payload (see _AudioReceiver in
    /// musicchain_player/.../rats_client.dart).
    static bool dispatch_chunk(const uint8_t* data, size_t size);

private:
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
    uint16_t          port_ = 0;
    // Native socket as uintptr_t so this header doesn't drag winsock
    // or sys/socket into every translation unit.
    uintptr_t         listen_fd_ = static_cast<uintptr_t>(-1);
    rats_client_t     rats_ = nullptr;

    void accept_loop_();
};

} // namespace mc::transport
