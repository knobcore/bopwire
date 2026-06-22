// ws_bridge.cpp — minimal WebSocket gateway into RatsApi.
//
// LEGACY / OPTIONAL: as of 2026-06-21 browsers reach the chain via a
// separate mini-node WS gateway (default port 8082) and relay.forward
// over rats. This bridge is kept operational so full-node operators
// can still expose ws:// directly to local browsers without running a
// mini-node. See ws_bridge.h for the role split.
//
// See ws_bridge.h for the design rationale. Highlights:
//
//   * One thread, one select() loop, N TCP connections (no per-conn
//     thread spawn — browsers are sticky-low-latency and we expect
//     <100 simultaneous tabs in practice).
//   * RFC 6455 client→server frames up to 64 KiB. Frames with
//     payload-len-form 127 (uint64_t length) are rejected to keep
//     the parser short; the envelopes the web player sends are
//     order-of-1 KiB.
//   * SHA-1 from OpenSSL for the Sec-WebSocket-Accept handshake key.
//   * Replies flow back via RatsApi::dispatch_for_bridge — a thread
//     -local reply sink RatsApi installs around handle_request so
//     send_reply detours into our outbound queue instead of doing
//     rats_send_message.
//
// Not implemented: ws:// permessage-deflate, fragmentation across
// many frames, subprotocols, origin pinning. Add when needed.
//

#include "ws_bridge.h"
#include "../api/rats_api.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  // inet_ntoa is deprecated in favour of inet_ntop / InetNtop. The
  // bridge only uses it for a debug "ws-1.2.3.4:5678" peer-id string;
  // silence the 4996 noise rather than expanding to InetNtopA's wider
  // contract.
  #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t_compat = int;
  // Errno + close + sentinel mapping so the rest of the file is
  // platform-agnostic.
  static inline int  ws_last_errno() { return WSAGetLastError(); }
  static inline bool ws_would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
  }
  static inline void ws_close(SOCKET s) { ::closesocket(s); }
  #define WS_INVALID_SOCKET INVALID_SOCKET
  #define WS_SOCKET_ERROR   SOCKET_ERROR
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
  static inline int  ws_last_errno() { return errno; }
  static inline bool ws_would_block(int e) {
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
  }
  static inline void ws_close(SOCKET s) { ::close(s); }
  #define WS_INVALID_SOCKET (-1)
  #define WS_SOCKET_ERROR   (-1)
#endif

namespace mc::transport {

namespace {

// Sec-WebSocket-Accept = base64(SHA1(key + GUID)). RFC 6455 §1.3.
constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 64 KiB cap per inbound frame. Larger envelopes get the connection
// closed — that's fine; the web player's request bodies are < 4 KiB.
constexpr size_t MAX_FRAME_PAYLOAD = 64 * 1024;

// Cap the outbound queue so a slow browser can't OOM the node. A
// healthy browser drains text frames in microseconds; if it backs up
// past this we just drop the connection.
constexpr size_t MAX_OUTBOUND_BYTES = 4 * 1024 * 1024;

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

// Case-insensitive header lookup. The HTTP upgrade request is short
// and structured; a linear pass over the buffer is fine.
std::string find_header(const std::string& req, const std::string& name) {
    // Walk line-by-line.
    size_t pos = 0;
    while (pos < req.size()) {
        size_t end = req.find("\r\n", pos);
        if (end == std::string::npos) break;
        if (end == pos) break; // empty line = end of headers
        const std::string line = req.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            // Trim
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'
                                  || v.back() == '\r')) v.pop_back();
            // Lowercase header name for compare
            for (auto& c : k) if (c >= 'A' && c <= 'Z') c += 32;
            std::string nm = name;
            for (auto& c : nm) if (c >= 'A' && c <= 'Z') c += 32;
            if (k == nm) return v;
        }
        pos = end + 2;
    }
    return {};
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

// Per-connection state. Lives in the listener's connections_ map by
// fd.
struct WsConn {
    SOCKET fd = WS_INVALID_SOCKET;
    bool   handshaked = false;          // upgrade completed?
    std::string in_buf;                 // pre-handshake HTTP buffer / post-handshake raw frame bytes
    std::string assembly_buf;           // accumulated payload across continuation frames
    int     assembly_opcode = -1;       // opcode of the in-progress msg
    std::deque<std::string> outbox;     // pending text frames, already wire-encoded
    size_t  outbox_off = 0;             // bytes already written from outbox.front()
    size_t  outbox_bytes = 0;           // total queued bytes, gates overflow
    std::string peer_id;                // synthetic peer id for RatsApi attribution
    bool    drop = false;               // marked for close after current flush
};

// Wire-encode a server→client text frame (FIN=1, OPCODE=0x1, no
// masking). RFC 6455 §5.2.
std::string encode_text_frame(const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 10);
    out.push_back(static_cast<char>(0x81)); // FIN | opcode=text
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        uint16_t len_be = static_cast<uint16_t>(payload.size());
        out.push_back(static_cast<char>((len_be >> 8) & 0xFF));
        out.push_back(static_cast<char>(len_be & 0xFF));
    } else {
        // Up to 2^63-1 per spec. We cap at 4 GiB to keep the cast
        // straightforward; replies in practice are < 1 MiB.
        out.push_back(static_cast<char>(127));
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    out.append(payload);
    return out;
}

// Wire-encode a server→client close frame (opcode=0x8) with the
// optional 2-byte status code body.
std::string encode_close_frame(uint16_t code = 1000) {
    std::string out;
    out.push_back(static_cast<char>(0x88));
    out.push_back(static_cast<char>(2));
    out.push_back(static_cast<char>((code >> 8) & 0xFF));
    out.push_back(static_cast<char>(code & 0xFF));
    return out;
}

std::string encode_pong_frame(const std::string& body) {
    std::string out;
    out.push_back(static_cast<char>(0x8A));
    if (body.size() < 126) {
        out.push_back(static_cast<char>(body.size()));
    } else {
        // Pings shouldn't be > 125 bytes per spec, but humour the
        // client.
        out.push_back(static_cast<char>(126));
        uint16_t len_be = static_cast<uint16_t>(body.size());
        out.push_back(static_cast<char>((len_be >> 8) & 0xFF));
        out.push_back(static_cast<char>(len_be & 0xFF));
    }
    out.append(body);
    return out;
}

void queue_outbound(WsConn& c, std::string frame) {
    if (c.outbox_bytes + frame.size() > MAX_OUTBOUND_BYTES) {
        std::cerr << "[ws] outbound queue overflow on fd=" << c.fd
                  << " — dropping connection\n";
        c.drop = true;
        return;
    }
    c.outbox_bytes += frame.size();
    c.outbox.push_back(std::move(frame));
}

// Parse one frame off the front of c.in_buf. Returns:
//   1  — one full frame consumed, see out_*
//   0  — incomplete frame, need more bytes
//  -1  — protocol error, close the connection
int parse_one_frame(WsConn& c,
                    int& out_opcode,
                    bool& out_fin,
                    std::string& out_payload) {
    const auto& buf = c.in_buf;
    if (buf.size() < 2) return 0;

    const uint8_t b0 = static_cast<uint8_t>(buf[0]);
    const uint8_t b1 = static_cast<uint8_t>(buf[1]);

    const bool fin   = (b0 & 0x80) != 0;
    const int  opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t   payload_len = b1 & 0x7F;
    size_t     header_len = 2;

    if (!masked) {
        // Clients MUST mask. Reject unmasked client frames.
        return -1;
    }

    if (payload_len == 126) {
        if (buf.size() < 4) return 0;
        payload_len = (static_cast<uint16_t>(static_cast<uint8_t>(buf[2])) << 8)
                    |  static_cast<uint8_t>(buf[3]);
        header_len += 2;
    } else if (payload_len == 127) {
        // 64-bit length form. We reject anything bigger than
        // MAX_FRAME_PAYLOAD anyway; cap the parse here.
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
    c.in_buf.erase(0, header_len + payload_len);
    return 1;
}

} // namespace

} // namespace mc::transport

// ---- thread-local reply sink hook ------------------------------------
//
// RatsApi::handle_request unconditionally calls send_reply, which used
// to mean rats_send_message. We can't change that signature without
// touching every verb branch, so we install a thread-local override:
// when set, send_reply forwards the JSON to the override instead of
// the rats client. The bridge runs handle_request on its own thread
// (the IO loop), so a thread_local is exactly the right scope.
//
// Declared in mc::api so it matches the definition in rats_api.cpp.

namespace mc::api {
extern thread_local std::function<void(const std::string&)>* g_ws_reply_sink;
}

namespace mc::transport {

WsBridge::WsBridge(mc::api::RatsApi& api) : api_(api) {}
WsBridge::~WsBridge() { stop(); }

bool WsBridge::start(uint16_t port) {
    if (running_.exchange(true)) return true;
    port_ = port;

#ifdef _WIN32
    // WSAStartup is idempotent across the process; librats already
    // called it but a duplicate startup is fine.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == WS_INVALID_SOCKET) {
        std::cerr << "[ws] socket() failed: errno=" << ws_last_errno() << "\n";
        running_.store(false);
        return false;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == WS_SOCKET_ERROR) {
        std::cerr << "[ws] bind() on port " << port << " failed: errno="
                  << ws_last_errno() << "\n";
        ws_close(fd);
        running_.store(false);
        return false;
    }
    if (::listen(fd, 32) == WS_SOCKET_ERROR) {
        std::cerr << "[ws] listen() failed: errno=" << ws_last_errno() << "\n";
        ws_close(fd);
        running_.store(false);
        return false;
    }
    set_nonblocking(fd);
    listen_fd_ = static_cast<uintptr_t>(fd);

    std::cout << "[ws] bridge listening on 0.0.0.0:" << port
              << " (browsers connect ws://<host>:" << port << "/)\n";

    io_thread_ = std::thread([this] { io_loop_(); });
    return true;
}

void WsBridge::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ != static_cast<uintptr_t>(-1)) {
        ws_close(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = static_cast<uintptr_t>(-1);
    }
    if (io_thread_.joinable()) io_thread_.join();
}

void WsBridge::io_loop_() {
    std::unordered_map<SOCKET, std::unique_ptr<WsConn>> conns;

    while (running_.load(std::memory_order_relaxed)) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        SOCKET listen_fd = static_cast<SOCKET>(listen_fd_);
        if (listen_fd == WS_INVALID_SOCKET) break;
        FD_SET(listen_fd, &rfds);
        SOCKET max_fd = listen_fd;

        for (auto& [fd, c] : conns) {
            FD_SET(fd, &rfds);
            if (!c->outbox.empty()) FD_SET(fd, &wfds);
            if (fd > max_fd) max_fd = fd;
        }

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000; // 200 ms — responsive to stop()
        int n = ::select(static_cast<int>(max_fd) + 1, &rfds, &wfds, nullptr, &tv);
        if (n < 0) {
            int e = ws_last_errno();
            if (ws_would_block(e)) continue;
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            std::cerr << "[ws] select() failed: errno=" << e << "\n";
            break;
        }

        // ---- accept new connections --------------------------------
        if (FD_ISSET(listen_fd, &rfds)) {
            sockaddr_in cli_addr{};
            socklen_t_compat sl = sizeof(cli_addr);
            SOCKET cfd = ::accept(listen_fd,
                                  reinterpret_cast<sockaddr*>(&cli_addr), &sl);
            if (cfd != WS_INVALID_SOCKET) {
                set_nonblocking(cfd);
                int yes = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&yes), sizeof(yes));
                auto c = std::make_unique<WsConn>();
                c->fd = cfd;
                // Build a synthetic peer id so RatsApi log lines have
                // something to print.
                char buf[64];
                std::snprintf(buf, sizeof(buf), "ws-%s:%u",
                              inet_ntoa(cli_addr.sin_addr),
                              static_cast<unsigned>(ntohs(cli_addr.sin_port)));
                c->peer_id = buf;
                conns.emplace(cfd, std::move(c));
            }
        }

        // ---- per-connection read / handshake / dispatch -----------
        std::vector<SOCKET> to_close;
        for (auto& [fd, c] : conns) {
            if (FD_ISSET(fd, &rfds)) {
                char buf[8192];
                int r = ::recv(fd, buf, sizeof(buf), 0);
                if (r == 0) { to_close.push_back(fd); continue; }
                if (r < 0) {
                    int e = ws_last_errno();
                    if (!ws_would_block(e)) to_close.push_back(fd);
                    continue;
                }
                c->in_buf.append(buf, static_cast<size_t>(r));

                if (!c->handshaked) {
                    // Look for end-of-headers (\r\n\r\n). Cap the
                    // request size — a 16 KiB header is already
                    // pathological for an Upgrade.
                    if (c->in_buf.size() > 16 * 1024) {
                        to_close.push_back(fd);
                        continue;
                    }
                    auto end = c->in_buf.find("\r\n\r\n");
                    if (end == std::string::npos) continue;

                    const std::string req = c->in_buf.substr(0, end + 4);
                    c->in_buf.erase(0, end + 4);

                    // Verify the upgrade fields.
                    const std::string upgrade = find_header(req, "Upgrade");
                    const std::string conn_h  = find_header(req, "Connection");
                    const std::string key     = find_header(req, "Sec-WebSocket-Key");
                    bool ok = !upgrade.empty() && !key.empty();
                    // Case-insensitive "websocket" / "upgrade" check.
                    auto ieq = [](std::string a, std::string b) {
                        if (a.size() != b.size()) return false;
                        for (size_t i = 0; i < a.size(); ++i) {
                            char ca = a[i], cb = b[i];
                            if (ca >= 'A' && ca <= 'Z') ca += 32;
                            if (cb >= 'A' && cb <= 'Z') cb += 32;
                            if (ca != cb) return false;
                        }
                        return true;
                    };
                    if (ok && !ieq(upgrade, "websocket")) ok = false;
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
                    // Send the handshake response synchronously — it's a
                    // single ~100 byte chunk, and at this stage the
                    // socket buffer is guaranteed empty.
                    ::send(fd, resp.data(), static_cast<int>(resp.size()), 0);
                    c->handshaked = true;
                    std::cout << "[ws] handshake ok from " << c->peer_id << "\n";
                }

                // ---- parse frames out of in_buf ------------------
                while (c->handshaked) {
                    int  opcode = 0;
                    bool fin    = false;
                    std::string payload;
                    int rc = parse_one_frame(*c, opcode, fin, payload);
                    if (rc == 0) break; // need more bytes
                    if (rc < 0) {
                        std::cerr << "[ws] protocol error on " << c->peer_id
                                  << ", closing\n";
                        to_close.push_back(fd);
                        break;
                    }

                    // Opcode dispatch.
                    if (opcode == 0x8) {
                        // Close — echo and drop.
                        queue_outbound(*c, encode_close_frame());
                        c->drop = true;
                        break;
                    } else if (opcode == 0x9) {
                        // Ping — echo as pong.
                        queue_outbound(*c, encode_pong_frame(payload));
                        continue;
                    } else if (opcode == 0xA) {
                        // Pong — ignore.
                        continue;
                    }

                    // Text(0x1) / Binary(0x2) / Continuation(0x0).
                    if (opcode == 0x1 || opcode == 0x2) {
                        // Reset assembly state.
                        c->assembly_opcode = opcode;
                        c->assembly_buf.clear();
                    } else if (opcode == 0x0) {
                        if (c->assembly_opcode < 0) {
                            std::cerr << "[ws] orphan continuation\n";
                            to_close.push_back(fd);
                            break;
                        }
                    } else {
                        // Unknown opcode.
                        to_close.push_back(fd);
                        break;
                    }
                    c->assembly_buf.append(payload);
                    if (!fin) continue;

                    // Full message assembled. Dispatch JSON envelope.
                    std::string env = std::move(c->assembly_buf);
                    c->assembly_buf.clear();
                    c->assembly_opcode = -1;

                    // Install the per-call reply sink and route into
                    // RatsApi. The sink captures the conn by raw
                    // pointer — safe because the dispatch runs
                    // synchronously on this same thread, before we
                    // touch `conns` again.
                    WsConn* conn_ptr = c.get();
                    std::function<void(const std::string&)> sink =
                        [conn_ptr](const std::string& reply_json) {
                            queue_outbound(*conn_ptr,
                                           encode_text_frame(reply_json));
                        };
                    mc::api::g_ws_reply_sink = &sink;
                    try {
                        // dispatch_for_bridge is a thin wrapper around
                        // RatsApi::handle_request that doesn't require
                        // a connected rats client.
                        api_.dispatch_for_bridge(conn_ptr->peer_id, env);
                    } catch (const std::exception& e) {
                        std::cerr << "[ws] dispatch threw: " << e.what()
                                  << "\n";
                    }
                    mc::api::g_ws_reply_sink = nullptr;
                }
            }
        }

        // ---- writes -----------------------------------------------
        for (auto& [fd, c] : conns) {
            if (!FD_ISSET(fd, &wfds)) continue;
            while (!c->outbox.empty()) {
                const std::string& front = c->outbox.front();
                const char* data = front.data() + c->outbox_off;
                size_t      remaining = front.size() - c->outbox_off;
                int r = ::send(fd, data, static_cast<int>(remaining), 0);
                if (r < 0) {
                    int e = ws_last_errno();
                    if (!ws_would_block(e)) to_close.push_back(fd);
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
            auto it = conns.find(fd);
            if (it != conns.end()) {
                ws_close(fd);
                conns.erase(it);
            }
        }
    }

    // Loop exit — close every remaining connection.
    for (auto& [fd, _] : conns) ws_close(fd);
}

} // namespace mc::transport
