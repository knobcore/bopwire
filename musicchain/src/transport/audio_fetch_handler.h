#pragma once
//
// audio_fetch_handler.h — `audio.fetch` verb handler for the mini-node's
// new WebSocket gateway (ws_mini_gateway).
//
// The gateway shell (ws_mini_gateway.h/.cpp) owns the WebSocket accept
// loop, RFC 6455 framing, and envelope routing. When it sees an inbound
// text frame whose `type` field is `audio.fetch`, it hands the request
// off to this handler. The handler:
//
//   1. Validates content_hash (64 hex) and peer_id (40 hex if present).
//   2. If peer_id is omitted, asks the home node for swarm members and
//      picks the first reachable one.
//   3. Sends `stream.open { content_hash }` to that peer via the
//      mini-node's g_client (rats_send_message + "musicchain.request").
//   4. Awaits the peer's `musicchain.reply` envelope.
//   5. On success emits a text frame {req_id, status:"ok", body:{stream_id,
//      total_bytes}} to the browser, then forwards every binary chunk
//      arriving on rats_set_binary_callback for that stream_id as a
//      binary WS frame.
//   6. After total_bytes have been delivered (or eof seen), emits a
//      final {req_id, status:"complete", body:{sent:<n>}} text frame.
//
// On WS close (or gateway shutdown), the handler unregisters its
// stream_id and sends a fire-and-forget `stream.close { stream_id }`
// to the peer.
//
// Chunk wire format (mirrors _AudioReceiver in
// musicchain_player/lib/src/services/rats_client.dart):
//   bytes 0..3 = stream_id (little-endian)
//   bytes 4..7 = seq        (little-endian)
//   byte   8   = eof flag
//   bytes 9..N = payload
//
// The handler strips the 9-byte header before forwarding the payload to
// the browser, matching the WsAudioBridge convention so existing
// browser clients keep working.
//
// Why a standalone handler:
//   This file is shipped before the parallel ws_mini_gateway agent
//   merges so its progress doesn't block the audio.fetch verb. The
//   gateway includes this header and invokes start_audio_fetch() with
//   two callbacks (send_text / send_binary) that it implements against
//   its own per-connection write queue. The handler never touches the
//   socket directly — it speaks through the callbacks.
//

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Forward-declare the librats handle so this header doesn't drag in
// librats_c.h everywhere. Same typedef as ws_audio_bridge.h.
typedef void* rats_client_t;

namespace mc::transport {

/// Callback the gateway hands the handler so it can emit a JSON text
/// frame to the requesting WS connection. The handler passes a dumped
/// JSON string; the gateway is responsible for wrapping it in a TEXT
/// (0x81) WebSocket frame and pushing onto the per-connection write
/// queue.
using SendTextFn   = std::function<void(const std::string& json)>;

/// Callback the gateway hands the handler so it can emit a binary frame
/// carrying raw audio bytes to the requesting WS connection. The
/// handler hands ownership-free pointers; the gateway must copy if it
/// wants to defer the write past the call.
using SendBinaryFn = std::function<void(const uint8_t* data, size_t size)>;

/// Handle returned by start_audio_fetch(). The gateway holds onto it
/// for the lifetime of the WebSocket connection and calls cancel() on
/// WS close (or gateway stop) so the handler can unregister its
/// stream_id and send `stream.close` to the peer.
///
/// The handle owns a worker thread internally; destroying it without
/// calling cancel() first is fine — the destructor joins the worker.
class AudioFetchHandle {
public:
    AudioFetchHandle();
    ~AudioFetchHandle();

    AudioFetchHandle(const AudioFetchHandle&) = delete;
    AudioFetchHandle& operator=(const AudioFetchHandle&) = delete;

    /// Tell the handler the WS connection is going away. Sends a
    /// fire-and-forget `stream.close` to the peer if a stream was
    /// opened, unregisters from the global stream registry, signals the
    /// worker thread to exit. Safe to call from any thread; idempotent.
    void cancel();

    /// True once start_audio_fetch's worker has finished — either by
    /// emitting the final "complete" envelope or by hitting an error /
    /// cancel.
    bool finished() const;

private:
    friend std::unique_ptr<AudioFetchHandle> start_audio_fetch(
        rats_client_t, const std::string&,
        const std::string&, const std::string&,
        SendTextFn, SendBinaryFn);
    friend bool dispatch_audio_fetch_reply(const std::string&,
                                           const std::string&);
    friend bool dispatch_audio_fetch_chunk(const uint8_t*, size_t);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Kick off an `audio.fetch` exchange.
///
/// Args:
///   rats          — the mini-node's live librats client (g_client).
///                   May be nullptr; the handler responds with a
///                   `no_rats` error envelope and stops.
///   req_id        — the browser-supplied req_id, echoed in every reply.
///   content_hash  — 64-hex content hash from body.content_hash.
///   peer_id       — 40-hex swarm peer id, OR empty string when the
///                   browser didn't name one. If empty, the handler
///                   asks the home node for swarm members and picks
///                   the first reachable peer.
///   send_text     — gateway callback that wraps a JSON string in a
///                   WS text frame and queues it for the connection.
///   send_bin      — gateway callback that wraps a buffer in a WS
///                   binary frame and queues it for the connection.
///
/// Returns a handle the gateway owns until WS close. Never null. If
/// validation fails (bad-shape req_id / content_hash / peer_id) the
/// handler still returns a handle but its worker emits a bad_request
/// envelope and exits immediately — uniform error path keeps the
/// gateway free of pre-validation duplication.
std::unique_ptr<AudioFetchHandle> start_audio_fetch(
    rats_client_t           rats,
    const std::string&      req_id,
    const std::string&      content_hash,
    const std::string&      peer_id,        // empty => ask home node
    SendTextFn              send_text,
    SendBinaryFn            send_bin);

/// Static hook called from the mini-node's on_relay_reply for every
/// `musicchain.reply` envelope whose req_id didn't match a pending
/// relay entry. If the req_id is one this handler minted for
/// stream.open, it consumes the reply and returns true; otherwise
/// returns false so the rest of the pipeline (WsAudioBridge,
/// gateway-other-verbs) gets a crack.
bool dispatch_audio_fetch_reply(const std::string& req_id,
                                const std::string& envelope_json);

/// Static hook called from on_relay_binary for every non-relay-tagged
/// binary chunk. The chunk layout matches the player's _AudioReceiver
/// (stream_id LE(4) + seq LE(4) + eof(1) + payload). Returns true if
/// the chunk matched a registered stream_id; false otherwise.
bool dispatch_audio_fetch_chunk(const uint8_t* data, size_t size);

} // namespace mc::transport
