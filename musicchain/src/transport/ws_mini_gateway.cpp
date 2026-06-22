// ws_mini_gateway.cpp — JSON-envelope + binary-frame WebSocket gateway
// for the musicchain mini-node. See ws_mini_gateway.h for the design
// rationale.
//
// ---- Wire protocol -------------------------------------------------
//
// * URL  : ws://<vps>:<port>/   (default 8082)
//          wss://<host>/   behind nginx / Caddy / Cloudflare TLS
//          termination.
// * Any Sec-WebSocket-Protocol is accepted; the gateway does NOT echo
//   one in the response. Browsers that supply a subprotocol get the
//   handshake completed without a server-side acknowledgement, which
//   is allowed by RFC 6455 §4.2.2.
//
// * Inbound TEXT frames carry the JSON envelope:
//
//     {
//       "req_id":  "<token>",
//       "type":    "<verb>",
//       "body":    { ... }
//     }
//
//   On a malformed envelope the gateway replies with:
//
//     {
//       "req_id": "<echoed or empty>",
//       "status": "bad_request",
//       "error":  "<human reason>"
//     }
//
// * Verbs answered locally from mini-node state:
//
//     "routes.get"     — returns the current routing table snapshot
//                        (same shape mini-node.cpp's routes_json()
//                        produces, exposed here via an extern hook
//                        the .cpp file provides).
//     "mininodes.list" — every mini-node peer we know about plus self.
//     "mini.hello"     — identifies the gateway peer; reply carries
//                        role + route_count + the gateway's
//                        observed peer address ("" — browsers don't
//                        have a librats peer address).
//     "stun.observe"   — reflects the TCP socket's remote endpoint
//                        as the browser-observed address.
//
// * "audio.fetch" — delegated to whatever AudioFetchHandler is
//   currently installed. If none, the gateway answers
//   `{"status":"not_implemented", ...}` and closes.
//
// * Any other verb is wrapped as:
//
//     {
//       "req_id":  <fresh>,
//       "type":    "relay.forward",
//       "body": {
//         "target_peer_id": <full-node peer id>,
//         "type":            <original verb>,
//         "body":            <original body>
//       }
//     }
//
//   and sent to a full node peer over the librats client. The
//   chosen target is the rats_peer_id of a Direct or Unknown
//   reachability route from the local route table; if none is
//   available the gateway answers `{"status":"no_full_node"}`.
//
// * Replies (musicchain.reply with the relay req_id) are caught by
//   the static dispatch_reply hook below, which the mini-node's
//   on_relay_reply calls before it falls through to the audio
//   bridge. The reply's req_id is rewritten back to the original
//   browser req_id and shipped as a TEXT frame.
//
// * Outbound BINARY frames are reserved for the audio.fetch handler.
//   Browsers should not send binary frames inbound; the gateway
//   ignores them.
//
// ---- Threading model -----------------------------------------------
//
// One accept-and-IO thread drives select() across the listen socket
// and every accepted connection. Handlers run synchronously on the
// I/O thread for the local verbs; the audio.fetch handler is
// expected to spawn its own worker if it does anything long-running.
// `sendBinary` / `sendText` may be called from any thread; they
// take a mutex on the connection registry and enqueue onto the
// outbox the I/O loop drains every tick.
//

#include "ws_mini_gateway.h"

// librats client + JSON parser. Both headers come from
// deps/librats/src/, which the `rats` (mc_rats_quic alias) target
// re-exports via its BUILD_INTERFACE.
#include "librats_c.h"
#include "json.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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
  static inline int  wm_last_errno() { return WSAGetLastError(); }
  static inline bool wm_would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
  }
  static inline void wm_close(SOCKET s) { ::closesocket(s); }
  #define WM_INVALID_SOCKET INVALID_SOCKET
  #define WM_SOCKET_ERROR   SOCKET_ERROR
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
  static inline int  wm_last_errno() { return errno; }
  static inline bool wm_would_block(int e) {
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
  }
  static inline void wm_close(SOCKET s) { ::close(s); }
  #define WM_INVALID_SOCKET (-1)
  #define WM_SOCKET_ERROR   (-1)
#endif

namespace mc::transport {

// ---- Hooks the mini-node provides ---------------------------------
//
// The gateway needs a few read-only views into the mini-node's
// globals (routes table, mini-node mesh, our librats peer id, the
// rats wire type tags). Rather than #include "mini_node.cpp" or
// expose a sprawling state struct, the mini-node defines these
// trivial accessors at file scope as `extern "C"`-free C++ free
// functions in the `mc::transport::mini_state` namespace. The
// gateway calls them via the forward declarations below; mini_node
// supplies the definitions immediately above main().

namespace mini_state {
// JSON document containing the current routes table snapshot:
//   { "self_load": {...}, "wallet": "<hex>", "peers": [ {route entry}, ... ] }
// Same shape returned by the mini-node's routes_json().
std::string routes_json();

// JSON array of every known mini-node peer (rats_peer_id +
// public_address) plus self. Same shape "mininodes.list" returns
// over the librats RPC path.
std::string mininodes_list_json();

// Mini-node identity stuff used by `mini.hello`. Returns
// route_count as a uint64.
uint64_t routes_count();

// Pick a full-node peer to fan a relay.forward at. Returns the
// peer's rats_peer_id, or "" if no full node is currently
// reachable. Selection policy: any direct route's rats_peer_id is
// preferred over an Unknown / Relay route.
std::string pick_full_node_peer_id();

// Our librats peer id, or "" if not running. Used in the
// mini.hello reply for symmetry with the librats handler.
std::string our_peer_id();
} // namespace mini_state

namespace {

// Sec-WebSocket-Accept = base64(SHA1(key + GUID)). RFC 6455 §1.3.
constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 1 MiB cap per inbound frame — the JSON envelopes the browser sends
// are well under 16 KiB; the larger ceiling is just paranoid.
constexpr size_t MAX_FRAME_PAYLOAD = 1 * 1024 * 1024;

// Cap on the per-conn outbound queue. Audio chunks are typically
// 16 KiB; 16 MiB covers a few seconds at even high bitrates without
// risking OOM if a browser is slow to drain.
constexpr size_t MAX_OUTBOUND_BYTES = 16 * 1024 * 1024;

// Mini-node side type tags for `musicchain.request` / `musicchain.reply`.
// Mirrors the constants in mini_node.cpp.
constexpr const char* kRequestType = "musicchain.request";

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

// ---- Wire encoders ------------------------------------------------

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

inline std::string encode_binary_frame(const uint8_t* data, size_t len) {
    return encode_frame(0x82,
                        reinterpret_cast<const char*>(data), len);
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

// Parse one client→server frame off the front of in_buf. Returns:
//   1 on a full frame consumed, 0 on incomplete, -1 on protocol error.

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

// ---- Per-connection state ----------------------------------------

struct WsConn {
    WsMiniConnId id = 0;
    SOCKET       fd = WM_INVALID_SOCKET;
    bool         handshaked = false;
    std::string  peer_label;     // synthetic "ws-mini-1.2.3.4:5678" for logs
    std::string  observed_addr;  // ip:port we accept()ed from

    std::string  in_buf;
    std::string  assembly_buf;
    int          assembly_opcode = -1;

    // The outbox is guarded by a mutex so sendBinary / sendText can
    // enqueue from arbitrary worker threads. The I/O loop drains
    // under the same mutex.
    std::mutex            out_mu;
    std::deque<std::string> outbox;
    size_t                outbox_off = 0;
    size_t                outbox_bytes = 0;

    bool         drop = false; // mark for close after current flush
};

// ---- Global state shared across the I/O loop and helper hooks ----

std::mutex                                          g_conns_mu;
std::unordered_map<WsMiniConnId, WsConn*>           g_conns;
std::atomic<WsMiniConnId>                           g_next_conn_id{1};

// req_id → conn id, used to route relay-forward replies back to the
// originating WS connection. Lives as a static global because the
// dispatch_reply hook the mini-node calls is static (the relay-reply
// callback knows nothing about which gateway instance owns the
// pending request).
std::mutex                                          g_pending_mu;
std::unordered_map<std::string, WsMiniConnId>       g_pending_relays;
std::unordered_map<std::string, std::string>        g_pending_orig_req; // fresh -> original req_id

std::atomic<rats_client_t> g_active_rats{nullptr};

// Audio.fetch handler, installed by the (separate) audio agent.
std::mutex            g_audio_handler_mu;
AudioFetchHandler     g_audio_handler;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string new_relay_req_id() {
    static std::atomic<uint64_t> counter{1};
    char buf[40];
    std::snprintf(buf, sizeof(buf), "wsmg-%016llx",
                  (unsigned long long)counter.fetch_add(1));
    return buf;
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

// Enqueue an outbound frame onto a conn's outbox. Internal helper —
// the public sendText / sendBinary / closeConn go through this.
// Returns false if the conn isn't registered.
bool enqueue_frame(WsMiniConnId conn_id, std::string frame) {
    WsConn* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_conns_mu);
        auto it = g_conns.find(conn_id);
        if (it == g_conns.end()) return false;
        c = it->second;
    }
    if (!c) return false;

    std::lock_guard<std::mutex> lk(c->out_mu);
    if (c->outbox_bytes + frame.size() > MAX_OUTBOUND_BYTES) {
        std::cerr << "[ws-mini] outbox overflow on conn=" << conn_id
                  << " — dropping connection\n";
        c->drop = true;
        return false;
    }
    c->outbox_bytes += frame.size();
    c->outbox.push_back(std::move(frame));
    return true;
}

// Convenience wrappers used by the dispatch lambdas below.
bool send_text(WsMiniConnId conn_id, const std::string& text) {
    return enqueue_frame(conn_id, encode_text_frame(text));
}

bool send_text_envelope(WsMiniConnId conn_id, const nlohmann::json& env) {
    return send_text(conn_id, env.dump());
}

// ---- Dispatch implementations -------------------------------------
//
// Each verb handler builds and sends one reply envelope. Verbs that
// answer locally are pure functions of the mini-node's snapshot
// state. The relay.forward fallthrough is the catch-all.

void answer_routes_get(WsMiniConnId conn_id, const std::string& req_id) {
    // routes_json returns {"self_load":..., "wallet":..., "peers":[...]}.
    // We hand it through verbatim as the envelope body.
    const std::string body = mini_state::routes_json();
    std::string env;
    env.reserve(body.size() + 64);
    env += "{\"req_id\":\"";
    env += req_id;
    env += "\",\"status\":\"ok\",\"body\":";
    env += body;
    env += "}";
    send_text(conn_id, env);
}

void answer_mininodes_list(WsMiniConnId conn_id, const std::string& req_id) {
    const std::string arr = mini_state::mininodes_list_json();
    std::string env;
    env.reserve(arr.size() + 64);
    env += "{\"req_id\":\"";
    env += req_id;
    env += "\",\"status\":\"ok\",\"body\":";
    env += arr;
    env += "}";
    send_text(conn_id, env);
}

void answer_mini_hello(WsMiniConnId conn_id, const std::string& req_id,
                       const std::string& observed_addr) {
    nlohmann::json body{
        {"role",         "mini-node"},
        {"peer_address", observed_addr},
        {"route_count",  mini_state::routes_count()},
        {"our_peer_id",  mini_state::our_peer_id()},
    };
    nlohmann::json env{
        {"req_id", req_id},
        {"status", "ok"},
        {"body",   std::move(body)},
    };
    send_text_envelope(conn_id, env);
}

void answer_stun_observe(WsMiniConnId conn_id, const std::string& req_id,
                         const std::string& observed_addr) {
    nlohmann::json body{{"observed_address", observed_addr}};
    nlohmann::json env{
        {"req_id", req_id},
        {"status", "ok"},
        {"body",   std::move(body)},
    };
    send_text_envelope(conn_id, env);
}

void answer_bad_request(WsMiniConnId conn_id, const std::string& req_id,
                        const std::string& reason) {
    nlohmann::json env{
        {"req_id", req_id},
        {"status", "bad_request"},
        {"error",  reason},
    };
    send_text_envelope(conn_id, env);
}

// Dispatch the audio.fetch verb. If a handler is installed, invoke
// it on a fresh worker thread (so the I/O loop keeps draining other
// connections). Otherwise reply not_implemented.
void dispatch_audio_fetch(WsMiniConnId conn_id,
                          const std::string& req_id,
                          const nlohmann::json& body) {
    AudioFetchHandler handler;
    {
        std::lock_guard<std::mutex> lk(g_audio_handler_mu);
        handler = g_audio_handler;
    }
    if (!handler) {
        nlohmann::json env{
            {"req_id", req_id},
            {"status", "not_implemented"},
            {"error",
             "audio.fetch handler not installed on this mini-node"},
        };
        send_text_envelope(conn_id, env);
        return;
    }
    const std::string peer_id_hex =
        body.value("peer_id",      std::string());
    const std::string content_hash =
        body.value("content_hash", std::string());
    // peer_id is OPTIONAL — if the browser omitted it, the handler
    // asks the home node for swarm members. If it's present we still
    // enforce 40-hex up front so the handler doesn't have to.
    if (!peer_id_hex.empty() && !is_hex_string(peer_id_hex, 40)) {
        answer_bad_request(conn_id, req_id,
                           "peer_id, when present, must be 40-hex");
        return;
    }
    if (!is_hex_string(content_hash, 64)) {
        answer_bad_request(conn_id, req_id, "content_hash must be 64-hex");
        return;
    }
    // Run the handler on a detached worker so a long-running audio
    // stream doesn't block the I/O loop.
    std::thread([conn_id, req_id, peer_id_hex, content_hash,
                 handler = std::move(handler)]() {
        try {
            handler(conn_id, req_id, peer_id_hex, content_hash);
        } catch (const std::exception& e) {
            std::cerr << "[ws-mini] audio.fetch handler threw: "
                      << e.what() << "\n";
        }
    }).detach();
}

// Wrap an arbitrary verb as relay.forward and send it to a chosen
// full node. Stores the (fresh_req_id → conn_id) mapping so
// dispatch_reply can route the answer back. If no full node is
// available, answers `no_full_node` immediately.
void dispatch_relay_forward(WsMiniConnId conn_id,
                            const std::string& req_id,
                            const std::string& type,
                            const nlohmann::json& body) {
    rats_client_t rats = g_active_rats.load();
    if (!rats) {
        nlohmann::json env{
            {"req_id", req_id},
            {"status", "no_rats"},
            {"error",  "mini-node has no active librats client"},
        };
        send_text_envelope(conn_id, env);
        return;
    }
    const std::string target = mini_state::pick_full_node_peer_id();
    if (target.empty()) {
        nlohmann::json env{
            {"req_id", req_id},
            {"status", "no_full_node"},
            {"error",
             "no full node currently reachable via this mini-node"},
        };
        send_text_envelope(conn_id, env);
        return;
    }

    const std::string fresh = new_relay_req_id();
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending_relays[fresh]    = conn_id;
        g_pending_orig_req[fresh]  = req_id;
    }

    nlohmann::json fwd_body = {
        {"target_peer_id", target},
        {"type",           type},
        {"body",           body},
    };
    nlohmann::json fwd = {
        {"req_id", fresh},
        {"type",   "relay.forward"},
        {"body",   std::move(fwd_body)},
    };
    auto rc = rats_send_message(rats, mini_state::our_peer_id().c_str(),
                                kRequestType, fwd.dump().c_str());
    // mini-node's RPC handler handles relay.forward by minting yet
    // another fresh req_id; we cannot easily inject the gateway's
    // path through that handler. Instead we directly send the
    // relay.forward verb to the target full node ourselves — that
    // keeps the routing one hop simpler and matches what the
    // player's relayed path does today.
    if (rc != RATS_SUCCESS) {
        // Send straight to the target full node, bypassing the
        // mini-node's own relay handler (which is what we wanted
        // anyway — we already chose the target).
        nlohmann::json direct = {
            {"req_id", fresh},
            {"type",   type},
            {"body",   body},
            // Tell the full node who originated this so its push
            // path back through `relay.push.forward` could find us
            // if it ever needs that flavor.
            {"originator_peer_id", std::string()},
        };
        rc = rats_send_message(rats, target.c_str(), kRequestType,
                               direct.dump().c_str());
        if (rc != RATS_SUCCESS) {
            // Clean up the pending mapping and report failure.
            {
                std::lock_guard<std::mutex> lk(g_pending_mu);
                g_pending_relays.erase(fresh);
                g_pending_orig_req.erase(fresh);
            }
            nlohmann::json env{
                {"req_id", req_id},
                {"status", "send_failed"},
                {"error",  "rats_send_message rc="
                              + std::to_string(static_cast<int>(rc))},
            };
            send_text_envelope(conn_id, env);
            return;
        }
    } else {
        // The earlier send succeeded via our own peer id path —
        // but that's wrong: rats_send_message to our own peer id
        // is a no-op / error on most builds. Treat as belt-and-
        // suspenders and also send direct. Idempotent at worst.
        nlohmann::json direct = {
            {"req_id", fresh},
            {"type",   type},
            {"body",   body},
            {"originator_peer_id", std::string()},
        };
        rats_send_message(rats, target.c_str(), kRequestType,
                          direct.dump().c_str());
    }
}

void dispatch_envelope(WsMiniConnId conn_id,
                       const std::string& observed_addr,
                       const std::string& envelope_json) {
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(envelope_json);
    } catch (const std::exception&) {
        answer_bad_request(conn_id, std::string(),
                           "envelope JSON parse failed");
        return;
    }
    const std::string req_id = env.value("req_id", std::string());
    const std::string type   = env.value("type",   std::string());
    const auto body = env.value("body", nlohmann::json::object());
    if (req_id.empty() || type.empty()) {
        answer_bad_request(conn_id, req_id,
                           "envelope needs req_id and type");
        return;
    }

    if (type == "routes.get") {
        answer_routes_get(conn_id, req_id);
    } else if (type == "mininodes.list") {
        answer_mininodes_list(conn_id, req_id);
    } else if (type == "mini.hello") {
        answer_mini_hello(conn_id, req_id, observed_addr);
    } else if (type == "stun.observe") {
        answer_stun_observe(conn_id, req_id, observed_addr);
    } else if (type == "audio.fetch") {
        dispatch_audio_fetch(conn_id, req_id, body);
    } else {
        dispatch_relay_forward(conn_id, req_id, type, body);
    }
}

// ---- WebSocket handshake -----------------------------------------
//
// Same shape as ws_audio_bridge::perform_ws_handshake: read until
// the empty line, validate the upgrade headers, write the 101 reply.
// `leftover_buf` carries any post-handshake bytes recv() pulled past
// the end of the request — we put them back in the conn's in_buf so
// the frame parser picks them up.

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
        int e = wm_last_errno();
        if (wm_would_block(e)) {
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
        ::send(ws_fd, resp, static_cast<int>(std::strlen(resp)), 0);
        return false;
    }

    const std::string accept = compute_accept_key(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    int w = ::send(ws_fd, resp.data(),
                   static_cast<int>(resp.size()), 0);
    return w == static_cast<int>(resp.size());
}

} // namespace

// ---- Public surface ------------------------------------------------

WsMiniGateway::~WsMiniGateway() { stop(); }

bool WsMiniGateway::start(uint16_t port, rats_client_t rats) {
    if (running_.exchange(true)) return true;
    port_ = port;
    rats_ = rats;
    g_active_rats.store(rats);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == WM_INVALID_SOCKET) {
        std::cerr << "[ws-mini] socket() failed: errno="
                  << wm_last_errno() << "\n";
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
        == WM_SOCKET_ERROR) {
        std::cerr << "[ws-mini] bind() on port " << port
                  << " failed: errno=" << wm_last_errno() << "\n";
        wm_close(fd);
        running_.store(false);
        g_active_rats.store(nullptr);
        return false;
    }
    if (::listen(fd, 32) == WM_SOCKET_ERROR) {
        std::cerr << "[ws-mini] listen() failed: errno="
                  << wm_last_errno() << "\n";
        wm_close(fd);
        running_.store(false);
        g_active_rats.store(nullptr);
        return false;
    }
    set_nonblocking(fd);
    listen_fd_ = static_cast<uintptr_t>(fd);

    std::cout << "[ws-mini] gateway listening on 0.0.0.0:" << port
              << " (browsers connect ws://<host>:" << port << "/)\n";

    io_thread_ = std::thread([this] { io_loop_(); });
    return true;
}

void WsMiniGateway::stop() {
    if (!running_.exchange(false)) return;
    g_active_rats.store(nullptr);

    if (listen_fd_ != static_cast<uintptr_t>(-1)) {
        wm_close(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = static_cast<uintptr_t>(-1);
    }
    if (io_thread_.joinable()) io_thread_.join();
}

void WsMiniGateway::io_loop_() {
    // We own every WsConn allocated for this gateway, indexed by fd
    // for the select loop. The global g_conns map indexes the same
    // objects by id so static helpers can find them.
    std::unordered_map<SOCKET, std::unique_ptr<WsConn>> conns_by_fd;

    while (running_.load(std::memory_order_relaxed)) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        SOCKET listen_fd = static_cast<SOCKET>(listen_fd_);
        if (listen_fd == WM_INVALID_SOCKET) break;
        FD_SET(listen_fd, &rfds);
        SOCKET max_fd = listen_fd;

        for (auto& [fd, c] : conns_by_fd) {
            FD_SET(fd, &rfds);
            {
                std::lock_guard<std::mutex> lk(c->out_mu);
                if (!c->outbox.empty()) FD_SET(fd, &wfds);
            }
            if (fd > max_fd) max_fd = fd;
        }

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 100 * 1000; // 100 ms — responsive to enqueue + stop()
        int n = ::select(static_cast<int>(max_fd) + 1,
                         &rfds, &wfds, nullptr, &tv);
        if (n < 0) {
            int e = wm_last_errno();
            if (wm_would_block(e)) continue;
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            std::cerr << "[ws-mini] select() failed: errno=" << e << "\n";
            break;
        }

        // ---- accept new connections --------------------------------
        if (FD_ISSET(listen_fd, &rfds)) {
            sockaddr_in cli_addr{};
            socklen_t_compat sl = sizeof(cli_addr);
            SOCKET cfd = ::accept(listen_fd,
                                  reinterpret_cast<sockaddr*>(&cli_addr), &sl);
            if (cfd != WM_INVALID_SOCKET) {
                set_nonblocking(cfd);
                int yes = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&yes), sizeof(yes));

                auto c = std::make_unique<WsConn>();
                c->fd = cfd;
                c->id = g_next_conn_id.fetch_add(1);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "ws-mini-%s:%u",
                              inet_ntoa(cli_addr.sin_addr),
                              static_cast<unsigned>(ntohs(cli_addr.sin_port)));
                c->peer_label = buf;
                char addr_buf[32];
                std::snprintf(addr_buf, sizeof(addr_buf), "%s:%u",
                              inet_ntoa(cli_addr.sin_addr),
                              static_cast<unsigned>(ntohs(cli_addr.sin_port)));
                c->observed_addr = addr_buf;

                {
                    std::lock_guard<std::mutex> lk(g_conns_mu);
                    g_conns[c->id] = c.get();
                }
                conns_by_fd.emplace(cfd, std::move(c));
            }
        }

        // ---- per-connection read / handshake / dispatch ------------
        std::vector<SOCKET> to_close;
        for (auto& [fd, c] : conns_by_fd) {
            if (FD_ISSET(fd, &rfds)) {
                char buf[8192];
                int r = ::recv(fd, buf, sizeof(buf), 0);
                if (r == 0) { to_close.push_back(fd); continue; }
                if (r < 0) {
                    int e = wm_last_errno();
                    if (!wm_would_block(e)) to_close.push_back(fd);
                    continue;
                }
                c->in_buf.append(buf, static_cast<size_t>(r));

                if (!c->handshaked) {
                    if (c->in_buf.size() > 16 * 1024) {
                        to_close.push_back(fd);
                        continue;
                    }
                    auto end = c->in_buf.find("\r\n\r\n");
                    if (end == std::string::npos) continue;

                    const std::string req = c->in_buf.substr(0, end + 4);
                    c->in_buf.erase(0, end + 4);

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
                        ::send(fd, resp, static_cast<int>(std::strlen(resp)), 0);
                        to_close.push_back(fd);
                        continue;
                    }

                    const std::string accept = compute_accept_key(key);
                    std::string resp =
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                    ::send(fd, resp.data(),
                           static_cast<int>(resp.size()), 0);
                    c->handshaked = true;
                    std::cout << "[ws-mini] handshake ok from "
                              << c->peer_label << " conn=" << c->id << "\n";
                }

                // ---- frame parsing ----------------------------------
                while (c->handshaked) {
                    int  opcode = 0;
                    bool fin    = false;
                    std::string payload;
                    int rc = parse_one_frame(c->in_buf, opcode, fin, payload);
                    if (rc == 0) break;
                    if (rc < 0) {
                        std::cerr << "[ws-mini] protocol error on "
                                  << c->peer_label << ", closing\n";
                        to_close.push_back(fd);
                        break;
                    }

                    if (opcode == 0x8) {
                        // Close — echo and drop.
                        enqueue_frame(c->id, encode_close_frame());
                        {
                            std::lock_guard<std::mutex> lk(c->out_mu);
                            c->drop = true;
                        }
                        break;
                    } else if (opcode == 0x9) {
                        enqueue_frame(c->id, encode_pong_frame(payload));
                        continue;
                    } else if (opcode == 0xA) {
                        continue; // pong — ignore
                    }

                    // Text / Binary / Continuation
                    if (opcode == 0x1 || opcode == 0x2) {
                        c->assembly_opcode = opcode;
                        c->assembly_buf.clear();
                    } else if (opcode == 0x0) {
                        if (c->assembly_opcode < 0) {
                            std::cerr << "[ws-mini] orphan continuation\n";
                            to_close.push_back(fd);
                            break;
                        }
                    } else {
                        to_close.push_back(fd);
                        break;
                    }
                    c->assembly_buf.append(payload);
                    if (!fin) continue;

                    const int  msg_opcode = c->assembly_opcode;
                    std::string msg = std::move(c->assembly_buf);
                    c->assembly_buf.clear();
                    c->assembly_opcode = -1;

                    if (msg_opcode == 0x2) {
                        // The gateway doesn't accept inbound binary
                        // — silently drop the frame so a future
                        // bidirectional binary verb wouldn't get
                        // tripped on a stray client message.
                        continue;
                    }

                    // Text frame — dispatch as a JSON envelope.
                    try {
                        dispatch_envelope(c->id, c->observed_addr, msg);
                    } catch (const std::exception& e) {
                        std::cerr << "[ws-mini] dispatch threw: "
                                  << e.what() << "\n";
                    }
                }
            }
        }

        // ---- writes -----------------------------------------------
        for (auto& [fd, c] : conns_by_fd) {
            if (!FD_ISSET(fd, &wfds)) continue;
            std::lock_guard<std::mutex> lk(c->out_mu);
            while (!c->outbox.empty()) {
                const std::string& front = c->outbox.front();
                const char* data = front.data() + c->outbox_off;
                size_t remaining = front.size() - c->outbox_off;
                int r = ::send(fd, data,
                               static_cast<int>(remaining), 0);
                if (r < 0) {
                    int e = wm_last_errno();
                    if (!wm_would_block(e)) {
                        to_close.push_back(fd);
                    }
                    break;
                }
                c->outbox_off += static_cast<size_t>(r);
                c->outbox_bytes -= static_cast<size_t>(r);
                if (c->outbox_off == front.size()) {
                    c->outbox.pop_front();
                    c->outbox_off = 0;
                }
                if (static_cast<size_t>(r) < remaining) break; // would block
            }
            if (c->drop && c->outbox.empty()) to_close.push_back(fd);
        }

        // ---- close drained connections ----------------------------
        for (SOCKET fd : to_close) {
            auto it = conns_by_fd.find(fd);
            if (it == conns_by_fd.end()) continue;
            const WsMiniConnId id = it->second->id;
            {
                std::lock_guard<std::mutex> lk(g_conns_mu);
                g_conns.erase(id);
            }
            // Also flush any pending relay req mappings that were
            // tied to this conn — the eventual reply has nowhere to
            // go.
            {
                std::lock_guard<std::mutex> lk(g_pending_mu);
                for (auto pit = g_pending_relays.begin();
                     pit != g_pending_relays.end(); ) {
                    if (pit->second == id) {
                        g_pending_orig_req.erase(pit->first);
                        pit = g_pending_relays.erase(pit);
                    } else {
                        ++pit;
                    }
                }
            }
            wm_close(fd);
            conns_by_fd.erase(it);
        }
    }

    // Loop exit — close every remaining connection.
    for (auto& [fd, c] : conns_by_fd) {
        const WsMiniConnId id = c->id;
        {
            std::lock_guard<std::mutex> lk(g_conns_mu);
            g_conns.erase(id);
        }
        wm_close(fd);
    }
    conns_by_fd.clear();
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending_relays.clear();
        g_pending_orig_req.clear();
    }
}

// ---- Static helpers ------------------------------------------------

bool WsMiniGateway::sendBinary(WsMiniConnId conn_id,
                               const uint8_t* data, size_t size) {
    if (!data || size == 0) return false;
    return enqueue_frame(conn_id, encode_binary_frame(data, size));
}

bool WsMiniGateway::sendText(WsMiniConnId conn_id, const std::string& text) {
    return enqueue_frame(conn_id, encode_text_frame(text));
}

bool WsMiniGateway::closeConn(WsMiniConnId conn_id) {
    // Send a close frame and mark the conn for drop after flush.
    bool sent = enqueue_frame(conn_id, encode_close_frame(1000));
    if (!sent) return false;
    WsConn* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_conns_mu);
        auto it = g_conns.find(conn_id);
        if (it == g_conns.end()) return false;
        c = it->second;
    }
    std::lock_guard<std::mutex> lk(c->out_mu);
    c->drop = true;
    return true;
}

void WsMiniGateway::set_audio_fetch_handler(AudioFetchHandler handler) {
    std::lock_guard<std::mutex> lk(g_audio_handler_mu);
    g_audio_handler = std::move(handler);
}

// ---- Relay-reply hook called by mini_node ---------------------------
//
// The mini-node's `on_relay_reply` calls a chain of dispatchers when
// a `musicchain.reply` arrives that doesn't match its own pending
// relay table. This function is the gateway's seat at that table:
// match the reply's req_id against the gateway's pending map, rewrite
// the req_id back to the browser's original, and ship as a text frame.
//
// Defined as a free function in the mc::transport namespace so
// mini_node.cpp can call it without instantiating a WsMiniGateway
// object (the gateway lives behind start/stop, but the dispatch is
// based on the global g_pending_* maps).

bool ws_mini_gateway_dispatch_reply(const std::string& req_id,
                                    const std::string& envelope_json) {
    if (req_id.empty()) return false;

    WsMiniConnId conn_id = 0;
    std::string original_req_id;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        auto it = g_pending_relays.find(req_id);
        if (it == g_pending_relays.end()) return false;
        conn_id = it->second;
        auto rit = g_pending_orig_req.find(req_id);
        if (rit != g_pending_orig_req.end()) {
            original_req_id = rit->second;
            g_pending_orig_req.erase(rit);
        }
        g_pending_relays.erase(it);
    }

    // Rewrite the req_id back to the browser's original so the
    // client correlates it to the request it sent.
    try {
        auto env = nlohmann::json::parse(envelope_json);
        if (!original_req_id.empty()) env["req_id"] = original_req_id;
        send_text(conn_id, env.dump());
    } catch (const std::exception&) {
        // Reply wasn't JSON — pass through verbatim. The browser
        // can decide what to do.
        send_text(conn_id, envelope_json);
    }
    return true;
}

} // namespace mc::transport
