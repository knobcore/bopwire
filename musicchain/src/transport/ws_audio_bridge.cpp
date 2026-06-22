// ws_audio_bridge.cpp — browser-facing audio fetch over WebSocket.
//
// ---- Wire protocol -------------------------------------------------
//
// 1. Path / framing
//
//   * URL  : ws://<vps-host>:<audio_bridge_port>/   (default 8082)
//            wss://<host>/   behind nginx / Caddy / Cloudflare TLS
//            termination.
//   * RFC 6455 WebSocket framing. The bridge accepts a single
//     client→server TEXT frame carrying the JSON request envelope,
//     then sends back zero or more BINARY frames (each one audio bytes)
//     followed by a final TEXT frame (the "complete" envelope) before
//     closing the socket. Maximum inbound frame: 16 MiB. Outbound
//     binary frames match the librats chunk size (16 KiB by default).
//
// 2. Inbound request envelope (TEXT frame, JSON)
//
//   {
//     "req_id":  "<8-byte hex>",        // echoed in every reply
//     "type":    "audio.fetch",
//     "body": {
//       "peer_id":      "<40-hex of swarm peer>",
//       "content_hash": "<64-hex SHA-256 content hash>"
//     }
//   }
//
//   Both peer_id and content_hash are validated for the right length
//   and hex shape. Any other shape gets the bad-request reply below
//   and the socket closes.
//
// 3. Reply envelopes (TEXT frames, JSON)
//
//   Acknowledgement after the peer answers stream.open with bytes:
//
//     {
//       "req_id": "...",
//       "status": "ok",
//       "body":   { "total_bytes": <n>, "stream_id": <n> }
//     }
//
//   Followed by N BINARY WS frames carrying the raw audio bytes in
//   order. Each binary frame's payload is the audio bytes only — no
//   stream_id / seq / eof prefix. The browser reassembles by
//   concatenation; total_bytes from the ok envelope drives the
//   progress UI.
//
//   Final TEXT frame after all bytes have been delivered:
//
//     {
//       "req_id": "...",
//       "status": "complete",
//       "body":   { "sent": <n> }
//     }
//
//   On error:
//
//     {
//       "req_id": "...",
//       "status": "<short_token>",      // not_matched / open_timeout /
//                                        // peer_send_failed / bad_request
//                                        // / no_rats / shutdown
//       "error":  "<human readable>"
//     }
//
//   Then the bridge closes the socket.
//
// 4. Lifecycle / cleanup
//
//   * If the browser closes mid-stream the bridge sends a fire-and-
//     forget `stream.close { stream_id }` to the peer so it stops
//     pushing chunks. Best-effort — the peer also has its own per-
//     stream timeout.
//   * If the peer's `stream.open` reply doesn't arrive within
//     kOpenTimeoutMs the bridge sends `open_timeout` and drops the
//     socket. The browser is expected to retry with a different
//     swarm member.
//   * If the peer answers `{ matched: false }` (the bytes are no
//     longer in its library) the bridge translates that to
//     `not_matched`.
//
// 5. Integration with the mini-node's librats callbacks
//
//   * `WsAudioBridge::dispatch_reply` is called from `on_relay_reply`
//     for every `musicchain.reply` whose req_id didn't match a
//     pending relay entry. If the req_id is one the bridge minted for
//     `stream.open`, it consumes the reply.
//   * `WsAudioBridge::dispatch_chunk` is called from `on_relay_binary`
//     for every direct (non-relay-tagged) binary chunk. The chunk
//     layout matches the player's `_AudioReceiver` decoder: bytes
//     0..3 = stream_id LE, 4..7 = seq LE, 8 = eof, 9..N = payload.
//

#include "ws_audio_bridge.h"

// librats client + JSON parser. Both headers come from
// deps/librats/src/, which the `rats` (mc_rats_quic alias) target
// re-exports via its BUILD_INTERFACE — so this same include works
// whether ws_audio_bridge.cpp is compiled into the musicchain
// library or directly into musicchain-mini-node.
#include "librats_c.h"
#include "json.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t_compat = int;
  static inline int  wa_last_errno() { return WSAGetLastError(); }
  static inline bool wa_would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
  }
  static inline void wa_close(SOCKET s) { ::closesocket(s); }
  #define WA_INVALID_SOCKET INVALID_SOCKET
  #define WA_SOCKET_ERROR   SOCKET_ERROR
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using SOCKET = int;
  using socklen_t_compat = socklen_t;
  static inline int  wa_last_errno() { return errno; }
  static inline bool wa_would_block(int e) {
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
  }
  static inline void wa_close(SOCKET s) { ::close(s); }
  #define WA_INVALID_SOCKET (-1)
  #define WA_SOCKET_ERROR   (-1)
#endif

namespace mc::transport {

namespace {

// Sec-WebSocket-Accept = base64(SHA1(key + GUID)). RFC 6455 §1.3.
constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 16 MiB cap per inbound frame — the same ceiling WsTcpRelay uses.
// The request envelope is < 1 KiB in practice; the cap defends against
// a hostile / runaway client.
constexpr size_t MAX_FRAME_PAYLOAD = 16 * 1024 * 1024;

// How long the bridge waits for the peer's `stream.open` reply before
// giving up and telling the browser to try another swarm member. The
// home node + player normally answer in <500 ms; cellular peers via
// relay can take a couple of seconds.
constexpr int kOpenTimeoutMs = 8000;

// Ceiling on the per-connection outbound queue. A slow browser that
// can't drain at audio bit-rate will hit this and get dropped before
// it OOMs the mini-node. 16 MiB covers a few seconds of even high-
// bitrate audio at normal upload speeds.
constexpr size_t MAX_OUTBOUND_BYTES = 16 * 1024 * 1024;

// Mini-node side type tag for `musicchain.request` / `musicchain.reply`.
// Mirrors the kRequestType / kReplyType constants in mini_node.cpp.
constexpr const char* kRequestType = "musicchain.request";

// Two helpers shared with WsTcpRelay / WsBridge — duplicated here on
// purpose so this file stays self-contained and we don't grow yet
// another "common ws utilities" header.

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* kTab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTab[(n >> 18) & 0x3F]);
        out.push_back(kTab[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kTab[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kTab[n & 0x3F]        : '=');
    }
    return out;
}

std::string compute_accept_key(const std::string& client_key) {
    std::string combined = client_key + WS_GUID;
    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const uint8_t*>(combined.data()),
         combined.size(), digest);
    return base64_encode(digest, SHA_DIGEST_LENGTH);
}

std::string find_header(const std::string& req, const std::string& name) {
    size_t pos = 0;
    while (pos < req.size()) {
        size_t end = req.find("\r\n", pos);
        if (end == std::string::npos) break;
        if (end == pos) break;
        const std::string line = req.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'
                                  || v.back() == '\r')) v.pop_back();
            for (auto& c : k) if (c >= 'A' && c <= 'Z') c += 32;
            std::string nm = name;
            for (auto& c : nm) if (c >= 'A' && c <= 'Z') c += 32;
            if (k == nm) return v;
        }
        pos = end + 2;
    }
    return {};
}

bool ieq_word(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

void set_nonblocking(SOCKET fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ---- Wire encoders -------------------------------------------------

std::string encode_frame(uint8_t opcode_with_fin,
                         const char* data, size_t len) {
    std::string out;
    out.reserve(len + 10);
    out.push_back(static_cast<char>(opcode_with_fin));
    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        uint16_t len_be = static_cast<uint16_t>(len);
        out.push_back(static_cast<char>((len_be >> 8) & 0xFF));
        out.push_back(static_cast<char>(len_be & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        uint64_t v = len;
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    }
    out.append(data, len);
    return out;
}

inline std::string encode_text_frame(const std::string& payload) {
    return encode_frame(0x81, payload.data(), payload.size());
}

inline std::string encode_binary_frame(const char* data, size_t len) {
    return encode_frame(0x82, data, len);
}

std::string encode_close_frame(uint16_t code = 1000) {
    char body[2];
    body[0] = static_cast<char>((code >> 8) & 0xFF);
    body[1] = static_cast<char>(code & 0xFF);
    return encode_frame(0x88, body, sizeof(body));
}

std::string encode_pong_frame(const std::string& body) {
    return encode_frame(0x8A, body.data(), body.size());
}

// Parse one client→server frame out of `in_buf`. Returns 1 on a full
// frame consumed, 0 on incomplete (need more bytes), -1 on protocol
// error. The caller owns the lifetime of `in_buf` and must erase
// `header_len + payload_len` bytes from its front after consuming.

int parse_one_frame(std::string& in_buf,
                    int& out_opcode,
                    bool& out_fin,
                    std::string& out_payload) {
    const auto& buf = in_buf;
    if (buf.size() < 2) return 0;

    const uint8_t b0 = static_cast<uint8_t>(buf[0]);
    const uint8_t b1 = static_cast<uint8_t>(buf[1]);

    const bool fin    = (b0 & 0x80) != 0;
    const int  opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t   payload_len = b1 & 0x7F;
    size_t     header_len = 2;

    if (!masked) return -1; // clients MUST mask

    if (payload_len == 126) {
        if (buf.size() < 4) return 0;
        payload_len = (static_cast<uint16_t>(static_cast<uint8_t>(buf[2])) << 8)
                    |  static_cast<uint8_t>(buf[3]);
        header_len += 2;
    } else if (payload_len == 127) {
        if (buf.size() < 10) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<uint8_t>(buf[2 + i]);
        }
        payload_len = v;
        header_len += 8;
    }
    if (payload_len > MAX_FRAME_PAYLOAD) return -1;
    if (buf.size() < header_len + 4 + payload_len) return 0;

    uint8_t mask[4];
    std::memcpy(mask, buf.data() + header_len, 4);
    header_len += 4;

    out_payload.assign(payload_len, '\0');
    for (uint64_t i = 0; i < payload_len; ++i) {
        out_payload[i] = static_cast<char>(
            static_cast<uint8_t>(buf[header_len + i]) ^ mask[i & 3]);
    }
    out_opcode = opcode;
    out_fin    = fin;
    in_buf.erase(0, header_len + payload_len);
    return 1;
}

bool send_all(SOCKET fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int r = ::send(fd, data + off, static_cast<int>(len - off), 0);
        if (r > 0) { off += static_cast<size_t>(r); continue; }
        int e = wa_last_errno();
        if (r < 0 && wa_would_block(e)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

bool is_hex_string(const std::string& s, size_t expected_len) {
    if (s.size() != expected_len) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9')
                     || (c >= 'a' && c <= 'f')
                     || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

// ---- Per-connection state -----------------------------------------
//
// One WsConn lives on the worker thread's stack. Pointers into it
// land in the global stream / pending-reply registries; the worker
// holds them there as long as it is the unique owner of the
// underlying object. Both registries are cleared from the worker
// before the WsConn goes out of scope, so dispatch_* never sees a
// dangling pointer.

struct WsConn {
    SOCKET fd = WA_INVALID_SOCKET;
    std::string peer_label;  // synthetic "audio-w-1.2.3.4:5678" for logs

    // Producer (the worker thread) → consumer (the dispatch hooks that
    // fire on the librats binary callback). The mutex guards both
    // wakeup_cv state and queues that flow IN to the worker.
    std::mutex              mu;
    std::condition_variable cv;

    // Inbound JSON reply from the peer (filled in by dispatch_reply).
    bool                    have_reply = false;
    std::string             reply_json;

    // Inbound audio chunks waiting to be flushed onto the WS socket.
    // Each entry is the raw audio payload (no stream_id / seq / eof
    // prefix); the worker wraps it in a binary WS frame and queues
    // for write. A small queue is fine — the worker drains every loop
    // tick.
    struct Chunk { std::vector<uint8_t> bytes; bool eof; };
    std::vector<Chunk>      chunks;

    // Filled in once the peer answers stream.open OK.
    uint32_t                stream_id = 0;
    uint64_t                total_bytes = 0;
    uint64_t                bytes_seen  = 0;
    bool                    have_stream_id = false;

    // Shutdown flag the dispatch hooks set when they want the worker
    // to terminate (rare — used only on bridge stop()).
    std::atomic<bool>       abort{false};
};

// ---- Registries (global, mutex-guarded) ---------------------------
//
// We can't keep these as members of WsAudioBridge because the
// `dispatch_*` hooks are static and the librats callbacks know
// nothing about which bridge instance owns the stream. In practice
// the mini-node only ever runs one bridge so a single static
// registry pair is fine.

std::mutex                                   g_pending_mu;
std::unordered_map<std::string, WsConn*>     g_pending_opens; // req_id -> conn

std::mutex                                   g_stream_mu;
std::unordered_map<uint32_t, WsConn*>        g_streams;       // stream_id -> conn

// Track the live rats client so dispatch_chunk / dispatch_reply can
// short-circuit when the bridge isn't running.
std::atomic<rats_client_t> g_active_rats{nullptr};

void register_pending_open(const std::string& req_id, WsConn* c) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_opens[req_id] = c;
}

void clear_pending_open(const std::string& req_id) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_opens.erase(req_id);
}

void register_stream(uint32_t sid, WsConn* c) {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_streams[sid] = c;
}

void clear_stream(uint32_t sid) {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_streams.erase(sid);
}

// ---- Misc utilities -----------------------------------------------

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string new_req_id() {
    static std::atomic<uint64_t> counter{1};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "wsaud-%010llx",
                  (unsigned long long)counter.fetch_add(1));
    return buf;
}

// One full WebSocket upgrade handshake. Returns true on success;
// `leftover_buf` may carry post-handshake bytes the parser will pick
// up on the next read.
bool perform_ws_handshake(SOCKET ws_fd, std::string& leftover_buf) {
    std::string req;
    char buf[4096];
    for (;;) {
        if (req.size() > 16 * 1024) return false;
        int r = ::recv(ws_fd, buf, sizeof(buf), 0);
        if (r > 0) {
            req.append(buf, static_cast<size_t>(r));
            auto end = req.find("\r\n\r\n");
            if (end != std::string::npos) {
                if (end + 4 < req.size())
                    leftover_buf.assign(req, end + 4, std::string::npos);
                req.resize(end + 4);
                break;
            }
            continue;
        }
        if (r == 0) return false;
        int e = wa_last_errno();
        if (wa_would_block(e)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }

    const std::string upgrade = find_header(req, "Upgrade");
    const std::string conn_h  = find_header(req, "Connection");
    const std::string key     = find_header(req, "Sec-WebSocket-Key");
    bool ok = !upgrade.empty() && !key.empty();
    if (ok && !ieq_word(upgrade, "websocket")) ok = false;
    if (ok && conn_h.find("Upgrade") == std::string::npos
          && conn_h.find("upgrade") == std::string::npos) {
        ok = false;
    }
    if (!ok) {
        const char* resp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 0\r\n\r\n";
        send_all(ws_fd, resp, std::strlen(resp));
        return false;
    }

    const std::string accept = compute_accept_key(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return send_all(ws_fd, resp.data(), resp.size());
}

// Queue (or send synchronously, when small) a JSON status envelope to
// the WS client. Used for the "ok" / "complete" / error envelopes.
void send_text(SOCKET fd, std::string& outbox, const nlohmann::json& env) {
    outbox += encode_text_frame(env.dump());
    // Try a synchronous flush — most replies are < 1 KiB and the
    // socket buffer is empty.
    while (!outbox.empty()) {
        int w = ::send(fd, outbox.data(),
                       static_cast<int>(outbox.size()), 0);
        if (w > 0) {
            outbox.erase(0, static_cast<size_t>(w));
            continue;
        }
        if (w < 0) {
            int e = wa_last_errno();
            if (wa_would_block(e)) break; // leave for the select loop
        }
        break;
    }
}

// Send `stream.close { stream_id }` to the peer so it can stop
// pushing chunks when the browser drops mid-stream. Best-effort.
void send_stream_close(rats_client_t rats,
                       const std::string& peer_id,
                       uint32_t stream_id) {
    if (!rats || peer_id.empty()) return;
    nlohmann::json env = {
        {"req_id", new_req_id()},
        {"type",   "stream.close"},
        {"body",   {{"stream_id", stream_id}}},
    };
    rats_send_message(rats, peer_id.c_str(), kRequestType, env.dump().c_str());
}

// ---- Worker thread per upgraded connection ------------------------
//
// Drives a select() loop until the song is fully streamed or the
// browser drops. Owns its WsConn + the WS socket.

void run_worker(SOCKET ws_fd,
                std::string peer_label,
                std::string post_handshake_buf,
                rats_client_t rats,
                const std::atomic<bool>* bridge_running) {
    set_nonblocking(ws_fd);
    auto conn = std::make_unique<WsConn>();
    conn->fd         = ws_fd;
    conn->peer_label = std::move(peer_label);

    std::string in_buf  = std::move(post_handshake_buf);
    std::string outbox; // server→client encoded frames awaiting flush

    // 1. Read frames until we have the audio.fetch request envelope.
    nlohmann::json req_env;
    std::string    req_id;
    std::string    target_peer;
    std::string    content_hash;
    bool           got_request = false;
    bool           drop_after_flush = false;

    auto fail_close = [&](const std::string& status,
                          const std::string& err) {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", status},
            {"error",  err},
        };
        send_text(ws_fd, outbox, env);
        outbox += encode_close_frame(1000);
        drop_after_flush = true;
    };

    // ---- request-receive loop --------------------------------------
    const uint64_t deadline_recv = now_ms() + 10'000; // 10 s to send the envelope
    while (!got_request && bridge_running->load(std::memory_order_relaxed)) {
        if (now_ms() > deadline_recv) {
            fail_close("bad_request", "no audio.fetch envelope within 10s");
            break;
        }
        fd_set rfds; FD_ZERO(&rfds); FD_SET(ws_fd, &rfds);
        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
        int n = ::select(static_cast<int>(ws_fd) + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) {
            int e = wa_last_errno();
            if (wa_would_block(e)) continue;
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            wa_close(ws_fd);
            return;
        }
        if (n > 0 && FD_ISSET(ws_fd, &rfds)) {
            char rb[4096];
            int r = ::recv(ws_fd, rb, sizeof(rb), 0);
            if (r == 0) { wa_close(ws_fd); return; }
            if (r < 0) {
                int e = wa_last_errno();
                if (!wa_would_block(e)) { wa_close(ws_fd); return; }
            } else {
                in_buf.append(rb, static_cast<size_t>(r));
            }
        }
        for (;;) {
            int  opcode = 0;
            bool fin    = false;
            std::string payload;
            int rc = parse_one_frame(in_buf, opcode, fin, payload);
            if (rc == 0) break;
            if (rc < 0) {
                fail_close("bad_request", "ws protocol error");
                got_request = true;
                break;
            }
            if (opcode == 0x8) {
                // Client closed before sending anything useful.
                outbox += encode_close_frame(1000);
                drop_after_flush = true;
                got_request = true;
                break;
            }
            if (opcode == 0x9) { // ping
                outbox += encode_pong_frame(payload);
                continue;
            }
            if (opcode == 0xA) continue; // pong
            // Text(0x1) / Binary(0x2): treat the first one as the
            // request envelope. Continuations across fragments are
            // not expected here (the envelope fits in one frame).
            if (!fin) {
                fail_close("bad_request", "fragmented envelope unsupported");
                got_request = true;
                break;
            }
            try {
                req_env = nlohmann::json::parse(payload);
            } catch (...) {
                fail_close("bad_request", "envelope JSON parse failed");
                got_request = true;
                break;
            }
            req_id       = req_env.value("req_id", std::string());
            const auto type = req_env.value("type", std::string());
            const auto body = req_env.value("body", nlohmann::json::object());
            if (type != "audio.fetch") {
                fail_close("bad_request",
                           "unknown type, expected audio.fetch");
                got_request = true;
                break;
            }
            target_peer  = body.value("peer_id",      std::string());
            content_hash = body.value("content_hash", std::string());
            if (!is_hex_string(target_peer, 40)) {
                fail_close("bad_request", "peer_id must be 40-hex");
                got_request = true;
                break;
            }
            if (!is_hex_string(content_hash, 64)) {
                fail_close("bad_request", "content_hash must be 64-hex");
                got_request = true;
                break;
            }
            got_request = true;
            break;
        }
        if (drop_after_flush) break;
    }

    // Flush any pending close envelope before bailing.
    if (drop_after_flush) {
        if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
        wa_close(ws_fd);
        return;
    }

    if (!rats) {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", "no_rats"},
            {"error",  "mini-node has no active librats client"},
        };
        send_text(ws_fd, outbox, env);
        if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
        wa_close(ws_fd);
        return;
    }

    // 2. Send stream.open to the peer and register the req_id so the
    //    reply lands in conn->reply_json via dispatch_reply.
    const std::string open_req_id = new_req_id();
    register_pending_open(open_req_id, conn.get());
    {
        nlohmann::json env = {
            {"req_id", open_req_id},
            {"type",   "stream.open"},
            {"body",   {{"content_hash", content_hash}}},
        };
        auto rc = rats_send_message(rats, target_peer.c_str(),
                                    kRequestType, env.dump().c_str());
        if (rc != RATS_SUCCESS) {
            clear_pending_open(open_req_id);
            nlohmann::json err = {
                {"req_id", req_id},
                {"status", "peer_send_failed"},
                {"error",  "rats_send_message rc="
                              + std::to_string(static_cast<int>(rc))},
            };
            send_text(ws_fd, outbox, err);
            outbox += encode_close_frame(1011);
            if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
            wa_close(ws_fd);
            return;
        }
    }

    // 3. Wait for the peer's reply with a deadline. While waiting we
    //    still drain the WS socket so a ping / early close works.
    const uint64_t open_deadline = now_ms() + kOpenTimeoutMs;
    while (bridge_running->load(std::memory_order_relaxed) &&
           !conn->abort.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(conn->mu);
            if (conn->cv.wait_for(lk, std::chrono::milliseconds(100),
                                  [&]{ return conn->have_reply; })) {
                break;
            }
        }
        if (now_ms() > open_deadline) {
            clear_pending_open(open_req_id);
            nlohmann::json env = {
                {"req_id", req_id},
                {"status", "open_timeout"},
                {"error",  "peer did not answer stream.open within "
                              + std::to_string(kOpenTimeoutMs) + "ms"},
            };
            send_text(ws_fd, outbox, env);
            outbox += encode_close_frame(1000);
            if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
            wa_close(ws_fd);
            return;
        }
        // Drain a single recv to catch ping / close. Non-blocking.
        char rb[1024];
        int r = ::recv(ws_fd, rb, sizeof(rb), 0);
        if (r == 0) {
            // Browser hung up — drop the pending open and bail.
            clear_pending_open(open_req_id);
            wa_close(ws_fd);
            return;
        }
        if (r > 0) {
            in_buf.append(rb, static_cast<size_t>(r));
            for (;;) {
                int  opcode = 0;
                bool fin    = false;
                std::string payload;
                int rc = parse_one_frame(in_buf, opcode, fin, payload);
                if (rc <= 0) break;
                if (opcode == 0x8) { conn->abort.store(true); break; }
                if (opcode == 0x9) outbox += encode_pong_frame(payload);
            }
            if (!outbox.empty()) {
                int w = ::send(ws_fd, outbox.data(),
                               static_cast<int>(outbox.size()), 0);
                if (w > 0) outbox.erase(0, static_cast<size_t>(w));
            }
        }
        if (conn->abort.load(std::memory_order_relaxed)) break;
    }

    if (conn->abort.load(std::memory_order_relaxed)) {
        clear_pending_open(open_req_id);
        wa_close(ws_fd);
        return;
    }

    clear_pending_open(open_req_id);

    // 4. Parse the peer's reply.
    nlohmann::json reply;
    try {
        reply = nlohmann::json::parse(conn->reply_json);
    } catch (...) {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", "peer_error"},
            {"error",  "stream.open reply was not JSON"},
        };
        send_text(ws_fd, outbox, env);
        outbox += encode_close_frame(1011);
        if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
        wa_close(ws_fd);
        return;
    }

    const std::string peer_status = reply.value("status", std::string("ok"));
    const auto& peer_body         = reply.value("body", nlohmann::json::object());
    if (peer_status != "ok" || peer_body.value("matched", true) == false) {
        const std::string status =
            peer_body.value("matched", true) == false
                ? std::string("not_matched") : peer_status;
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", status},
            {"error",  reply.value("error",
                          peer_body.value("error",
                              std::string("peer rejected stream.open")))},
        };
        send_text(ws_fd, outbox, env);
        outbox += encode_close_frame(1000);
        if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
        wa_close(ws_fd);
        return;
    }

    // peer answered with stream_id + total_bytes
    if (!peer_body.contains("stream_id") || !peer_body.contains("total_bytes")) {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", "peer_error"},
            {"error",  "stream.open reply missing stream_id/total_bytes"},
        };
        send_text(ws_fd, outbox, env);
        outbox += encode_close_frame(1011);
        if (!outbox.empty()) send_all(ws_fd, outbox.data(), outbox.size());
        wa_close(ws_fd);
        return;
    }
    conn->stream_id     = peer_body["stream_id"].get<uint32_t>();
    conn->total_bytes   = peer_body["total_bytes"].get<uint64_t>();
    conn->have_stream_id = true;
    register_stream(conn->stream_id, conn.get());

    // 5. Send the "ok" envelope to the browser so its UI can show
    //    progress against total_bytes.
    {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", "ok"},
            {"body",   {
                {"total_bytes", conn->total_bytes},
                {"stream_id",   conn->stream_id},
            }},
        };
        send_text(ws_fd, outbox, env);
    }

    // 6. Pump audio chunks until either we've delivered total_bytes,
    //    the browser hangs up, or the bridge stops.
    const uint64_t stall_deadline_each_loop = 60'000; // 60 s stall guard
    uint64_t last_chunk_at = now_ms();
    bool     eof_seen      = false;

    while (bridge_running->load(std::memory_order_relaxed) &&
           !conn->abort.load(std::memory_order_relaxed)) {
        // Pull any chunks the dispatch hook left for us.
        std::vector<WsConn::Chunk> drained;
        {
            std::unique_lock<std::mutex> lk(conn->mu);
            // Short wait so we still drive the WS socket regularly.
            conn->cv.wait_for(lk, std::chrono::milliseconds(50),
                              [&]{ return !conn->chunks.empty()
                                       || conn->abort.load(); });
            if (!conn->chunks.empty()) {
                drained.swap(conn->chunks);
                last_chunk_at = now_ms();
            }
        }
        for (auto& c : drained) {
            conn->bytes_seen += c.bytes.size();
            if (outbox.size() + c.bytes.size() + 10 > MAX_OUTBOUND_BYTES) {
                std::cerr << "[ws-audio] outbox overflow on "
                          << conn->peer_label
                          << " — dropping connection\n";
                conn->abort.store(true);
                break;
            }
            outbox += encode_binary_frame(
                reinterpret_cast<const char*>(c.bytes.data()),
                c.bytes.size());
            if (c.eof) eof_seen = true;
        }

        // Drive the WS socket: read for ping / close, write any
        // queued frames.
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(ws_fd, &rfds);
        if (!outbox.empty()) FD_SET(ws_fd, &wfds);
        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 50 * 1000;
        int n = ::select(static_cast<int>(ws_fd) + 1, &rfds, &wfds,
                         nullptr, &tv);
        if (n < 0) {
            int e = wa_last_errno();
            if (wa_would_block(e)) {
                // Stall guard against an unresponsive peer.
                if (now_ms() - last_chunk_at > stall_deadline_each_loop
                    && !eof_seen) {
                    conn->abort.store(true);
                    break;
                }
                continue;
            }
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            break;
        }

        if (FD_ISSET(ws_fd, &rfds)) {
            char rb[4096];
            int r = ::recv(ws_fd, rb, sizeof(rb), 0);
            if (r == 0) {
                conn->abort.store(true);
                break;
            }
            if (r > 0) {
                in_buf.append(rb, static_cast<size_t>(r));
                for (;;) {
                    int  opcode = 0;
                    bool fin    = false;
                    std::string payload;
                    int rc = parse_one_frame(in_buf, opcode, fin, payload);
                    if (rc <= 0) break;
                    if (opcode == 0x8) { conn->abort.store(true); break; }
                    if (opcode == 0x9) outbox += encode_pong_frame(payload);
                }
                if (conn->abort.load()) break;
            } else if (r < 0) {
                int e = wa_last_errno();
                if (!wa_would_block(e)) { conn->abort.store(true); break; }
            }
        }

        if (FD_ISSET(ws_fd, &wfds) && !outbox.empty()) {
            int w = ::send(ws_fd, outbox.data(),
                           static_cast<int>(outbox.size()), 0);
            if (w > 0) outbox.erase(0, static_cast<size_t>(w));
            else if (w < 0) {
                int e = wa_last_errno();
                if (!wa_would_block(e)) { conn->abort.store(true); break; }
            }
        }

        // Stall guard.
        if (!eof_seen
            && now_ms() - last_chunk_at > stall_deadline_each_loop) {
            std::cerr << "[ws-audio] " << conn->peer_label
                      << " stalled (no chunks for " << stall_deadline_each_loop
                      << " ms) — dropping\n";
            conn->abort.store(true);
            break;
        }

        // Done? eof from the peer is the source of truth; we don't
        // strictly compare bytes_seen against total_bytes because the
        // peer's total can be an upper bound (e.g. variable-bit-rate
        // codecs surfacing approximate sizes).
        if (eof_seen && outbox.empty()) break;
        if (conn->bytes_seen >= conn->total_bytes
            && outbox.empty()
            && conn->total_bytes > 0) {
            // Some peer implementations skip the eof flag on the
            // last chunk; treat byte parity as done.
            break;
        }
    }

    // 7. Tear-down. Unregister the stream first so no further chunks
    //    land on our pointer, then send a status envelope and close.
    if (conn->have_stream_id) clear_stream(conn->stream_id);

    if (!conn->abort.load()) {
        nlohmann::json env = {
            {"req_id", req_id},
            {"status", "complete"},
            {"body",   {{"sent", conn->bytes_seen}}},
        };
        send_text(ws_fd, outbox, env);
        outbox += encode_close_frame(1000);
    } else {
        // Browser disconnected or stall guard fired. Tell the peer to
        // stop pushing chunks. Fire-and-forget; the peer also has its
        // own per-stream timeout.
        if (conn->have_stream_id) {
            send_stream_close(rats, target_peer, conn->stream_id);
        }
    }

    if (!outbox.empty()) {
        // Spin briefly so the close envelope makes it onto the wire.
        int spins = 20;
        while (!outbox.empty() && spins-- > 0) {
            int w = ::send(ws_fd, outbox.data(),
                           static_cast<int>(outbox.size()), 0);
            if (w > 0) outbox.erase(0, static_cast<size_t>(w));
            else break;
        }
    }

    wa_close(ws_fd);
}

} // namespace

// ---- Static dispatch hooks ----------------------------------------

bool WsAudioBridge::dispatch_reply(const std::string& req_id,
                                   const std::string& envelope_json) {
    if (req_id.empty()) return false;
    WsConn* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        auto it = g_pending_opens.find(req_id);
        if (it == g_pending_opens.end()) return false;
        c = it->second;
        g_pending_opens.erase(it);
    }
    if (!c) return true;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        c->have_reply = true;
        c->reply_json = envelope_json;
    }
    c->cv.notify_all();
    return true;
}

bool WsAudioBridge::dispatch_chunk(const uint8_t* data, size_t size) {
    if (!data || size < 9) return false;
    // First 4 bytes: stream_id LE. Bytes 4..7: seq LE. Byte 8: eof flag.
    // Bytes 9..N: payload.
    const uint32_t sid = static_cast<uint32_t>(data[0])
                       | (static_cast<uint32_t>(data[1]) << 8)
                       | (static_cast<uint32_t>(data[2]) << 16)
                       | (static_cast<uint32_t>(data[3]) << 24);
    const bool eof = data[8] != 0;
    WsConn* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        auto it = g_streams.find(sid);
        if (it == g_streams.end()) return false;
        c = it->second;
    }
    if (!c) return true;

    {
        std::lock_guard<std::mutex> lk(c->mu);
        c->chunks.push_back(WsConn::Chunk{});
        auto& back = c->chunks.back();
        back.bytes.assign(data + 9, data + size);
        back.eof = eof;
    }
    c->cv.notify_all();
    return true;
}

WsAudioBridge::~WsAudioBridge() { stop(); }

bool WsAudioBridge::start(uint16_t port, rats_client_t rats) {
    if (running_.exchange(true)) return true;
    port_ = port;
    rats_ = rats;
    g_active_rats.store(rats);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == WA_INVALID_SOCKET) {
        std::cerr << "[ws-audio] socket() failed: errno="
                  << wa_last_errno() << "\n";
        running_.store(false);
        g_active_rats.store(nullptr);
        return false;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
        == WA_SOCKET_ERROR) {
        std::cerr << "[ws-audio] bind() on port " << port
                  << " failed: errno=" << wa_last_errno() << "\n";
        wa_close(fd);
        running_.store(false);
        g_active_rats.store(nullptr);
        return false;
    }
    if (::listen(fd, 32) == WA_SOCKET_ERROR) {
        std::cerr << "[ws-audio] listen() failed: errno="
                  << wa_last_errno() << "\n";
        wa_close(fd);
        running_.store(false);
        g_active_rats.store(nullptr);
        return false;
    }
    set_nonblocking(fd);
    listen_fd_ = static_cast<uintptr_t>(fd);

    std::cout << "[ws-audio] audio-fetch bridge listening on 0.0.0.0:"
              << port
              << " (browsers connect ws://<host>:" << port << "/)\n";

    accept_thread_ = std::thread([this] { accept_loop_(); });
    return true;
}

void WsAudioBridge::stop() {
    if (!running_.exchange(false)) return;
    g_active_rats.store(nullptr);

    if (listen_fd_ != static_cast<uintptr_t>(-1)) {
        wa_close(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = static_cast<uintptr_t>(-1);
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    // Snapshot every registered conn, mark them aborted, then wake
    // them so the workers exit promptly. Workers are detached so the
    // bridge can't join them, but they observe the running_ flag
    // through bridge_running on each loop iteration and via the
    // per-conn abort flag we set here.
    std::vector<WsConn*> conns;
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        for (auto& kv : g_streams) conns.push_back(kv.second);
    }
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        for (auto& kv : g_pending_opens) conns.push_back(kv.second);
    }
    for (WsConn* c : conns) {
        if (!c) continue;
        c->abort.store(true);
        c->cv.notify_all();
    }
}

void WsAudioBridge::accept_loop_() {
    while (running_.load(std::memory_order_relaxed)) {
        fd_set rfds; FD_ZERO(&rfds);
        SOCKET lfd = static_cast<SOCKET>(listen_fd_);
        if (lfd == WA_INVALID_SOCKET) break;
        FD_SET(lfd, &rfds);

        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
        int n = ::select(static_cast<int>(lfd) + 1, &rfds, nullptr,
                         nullptr, &tv);
        if (n < 0) {
            int e = wa_last_errno();
            if (wa_would_block(e)) continue;
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            break;
        }
        if (n == 0) continue;
        if (!FD_ISSET(lfd, &rfds)) continue;

        sockaddr_in cli_addr{};
        socklen_t_compat sl = sizeof(cli_addr);
        SOCKET cfd = ::accept(lfd,
                              reinterpret_cast<sockaddr*>(&cli_addr), &sl);
        if (cfd == WA_INVALID_SOCKET) continue;

        int yes = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        char buf[64];
        std::snprintf(buf, sizeof(buf), "audio-w-%s:%u",
                      inet_ntoa(cli_addr.sin_addr),
                      static_cast<unsigned>(ntohs(cli_addr.sin_port)));
        std::string peer_label = buf;

        rats_client_t rats = rats_;
        const std::atomic<bool>* bridge_running = &running_;

        std::thread([cfd, peer_label, rats, bridge_running]() {
            std::string leftover;
            if (!perform_ws_handshake(cfd, leftover)) {
                wa_close(cfd);
                return;
            }
            std::cout << "[ws-audio] handshake ok from " << peer_label
                      << "\n";
            run_worker(cfd, peer_label, std::move(leftover),
                       rats, bridge_running);
        }).detach();
    }
}

} // namespace mc::transport
