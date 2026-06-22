// ws_tcp_relay.cpp — see ws_tcp_relay.h for the design rationale.
//
// One accept thread takes upgrade requests off the listen socket.
// On a successful WebSocket upgrade it opens a paired TCP socket to
// 127.0.0.1:tcp_target_port (the librats listener on the same VPS)
// and spawns a dedicated worker thread for that connection pair.
// The worker drives a select() loop that:
//
//   * reads bytes from the WebSocket socket, parses one or more
//     RFC 6455 frames, and write()s the payload of every binary
//     frame straight into the paired TCP socket.
//   * reads bytes from the TCP socket and wraps each chunk in a
//     single binary WS frame back out to the browser.
//   * answers ping with pong, drops on close.
//
// Manual end-to-end test plan:
//
//   1. Start the mini-node:
//        musicchain-mini-node --rats-port 8080
//      It binds the librats TCP listener on 8080 AND the WsTcpRelay
//      on 8081 (the default). The relay logs:
//        [ws-tcp] relay listening on 0.0.0.0:8081 -> 127.0.0.1:8080
//
//   2. Drive a browser-side librats compiled with emscripten's
//      `-lwebsocket.js`. Its `connect("vps-host", 8081)` call
//      shims into `new WebSocket("ws://vps-host:8081/")`. The
//      handshake completes, then every BSD `send(fd, buf, len, 0)`
//      becomes a single binary WS frame the relay forwards to
//      the librats listener on 127.0.0.1:8080. Replies from
//      librats flow back as binary WS frames the browser
//      reassembles into `recv(fd, ...)` results.
//
//   3. As a smoke test without a browser, point a tiny Node.js
//      script that uses the `ws` package at ws://localhost:8081/
//      and send a binary `Buffer.from([0xde, 0xad, 0xbe, 0xef])`.
//      Confirm with `netstat -anp tcp` that a fresh outbound TCP
//      flow opens to 127.0.0.1:8080 carrying those bytes.
//
//   4. The browser-side emscripten shim emits the URL string
//        ws://<vps-public-ip>:8081/
//      for every `connect()` call. Production deploys terminate
//      wss:// at nginx / Cloudflare / Caddy in front of this
//      listener.
//

#include "ws_tcp_relay.h"

#include <openssl/sha.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
  static inline int  wr_last_errno() { return WSAGetLastError(); }
  static inline bool wr_would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
  }
  static inline void wr_close(SOCKET s) { ::closesocket(s); }
  #define WR_INVALID_SOCKET INVALID_SOCKET
  #define WR_SOCKET_ERROR   SOCKET_ERROR
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
  static inline int  wr_last_errno() { return errno; }
  static inline bool wr_would_block(int e) {
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
  }
  static inline void wr_close(SOCKET s) { ::close(s); }
  #define WR_INVALID_SOCKET (-1)
  #define WR_SOCKET_ERROR   (-1)
#endif

namespace mc::transport {

namespace {

// Sec-WebSocket-Accept = base64(SHA1(key + GUID)). RFC 6455 §1.3.
constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 16 MiB cap per incoming WS frame. Browser librats traffic
// is well below this; chunks bigger than 16 MiB usually mean
// either a misbehaving client or an attempt to OOM the relay.
constexpr size_t MAX_FRAME_PAYLOAD = 16 * 1024 * 1024;

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

// Case-insensitive header lookup. The HTTP upgrade request is small;
// a linear pass is fine.
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

// Wire-encode a server→client binary frame (FIN=1, OPCODE=0x2,
// no masking). RFC 6455 §5.2.
std::string encode_binary_frame(const char* data, size_t len) {
    std::string out;
    out.reserve(len + 10);
    out.push_back(static_cast<char>(0x82)); // FIN | opcode=binary
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

// Parse one frame off the front of in_buf. Returns:
//   1  — one full frame consumed, see out_*
//   0  — incomplete frame, need more bytes
//  -1  — protocol error, close the connection
int parse_one_frame(std::string& in_buf,
                    int& out_opcode,
                    bool& out_fin,
                    std::vector<char>& out_payload) {
    const auto& buf = in_buf;
    if (buf.size() < 2) return 0;

    const uint8_t b0 = static_cast<uint8_t>(buf[0]);
    const uint8_t b1 = static_cast<uint8_t>(buf[1]);

    const bool fin    = (b0 & 0x80) != 0;
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

// Send `data` synchronously, looping until everything is gone or
// the socket is dead. Used during the upgrade handshake (a few
// hundred bytes) — the socket is still empty so a blocking-style
// flush is safe.
bool send_all(SOCKET fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int r = ::send(fd, data + off, static_cast<int>(len - off), 0);
        if (r > 0) { off += static_cast<size_t>(r); continue; }
        int e = wr_last_errno();
        if (r < 0 && wr_would_block(e)) {
            // would-block during handshake: spin briefly. We just
            // accepted the connection so this rarely happens.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

// Open + connect a fresh TCP socket to 127.0.0.1:port. Returns
// WR_INVALID_SOCKET on failure.
SOCKET open_paired_tcp(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == WR_INVALID_SOCKET) return WR_INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
    addr.sin_port        = htons(port);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
        == WR_SOCKET_ERROR) {
        wr_close(s);
        return WR_INVALID_SOCKET;
    }

    int yes = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
    return s;
}

// One full WebSocket upgrade handshake. Returns true on success;
// on failure the caller closes `ws_fd`.
bool perform_ws_handshake(SOCKET ws_fd, std::string& leftover_buf) {
    // Read until we see end-of-headers or hit the safety cap.
    std::string req;
    char buf[4096];
    for (;;) {
        if (req.size() > 16 * 1024) return false;
        int r = ::recv(ws_fd, buf, sizeof(buf), 0);
        if (r > 0) {
            req.append(buf, static_cast<size_t>(r));
            auto end = req.find("\r\n\r\n");
            if (end != std::string::npos) {
                // Any bytes past the header belong to the post-
                // handshake frame stream. Hand them back to the
                // worker.
                if (end + 4 < req.size()) {
                    leftover_buf.assign(req, end + 4, std::string::npos);
                }
                req.resize(end + 4);
                break;
            }
            continue;
        }
        if (r == 0) return false;
        int e = wr_last_errno();
        if (wr_would_block(e)) {
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
    // Echo back Sec-WebSocket-Protocol if the client offered one — the
    // emscripten shim sends "binary" by default. Accepting it without
    // policing the value keeps the relay transparent.
    const std::string proto = find_header(req, "Sec-WebSocket-Protocol");
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n";
    if (!proto.empty()) {
        // Pick the first offered subprotocol if a comma-separated
        // list arrived.
        std::string first = proto;
        size_t comma = first.find(',');
        if (comma != std::string::npos) first.resize(comma);
        while (!first.empty() && (first.front() == ' ' || first.front() == '\t'))
            first.erase(first.begin());
        while (!first.empty() && (first.back() == ' ' || first.back() == '\t'))
            first.pop_back();
        if (!first.empty()) {
            resp += "Sec-WebSocket-Protocol: " + first + "\r\n";
        }
    }
    resp += "\r\n";
    return send_all(ws_fd, resp.data(), resp.size());
}

// Per-connection byte-pump worker. Owns both sockets and the
// connection lifetime. Drops both on any error.
void worker_loop(SOCKET ws_fd,
                 SOCKET tcp_fd,
                 std::string post_handshake_buf,
                 const std::atomic<bool>* stop_flag) {
    set_nonblocking(ws_fd);
    set_nonblocking(tcp_fd);

    std::string ws_in_buf = std::move(post_handshake_buf);
    std::string ws_outbox;       // pending bytes to ws_fd (encoded frames)
    std::string tcp_outbox;      // pending bytes to tcp_fd (raw payload)

    bool ws_closed_by_peer = false;

    while (stop_flag->load(std::memory_order_relaxed) == false ||
           !ws_outbox.empty() || !tcp_outbox.empty()) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        // Always watch reads on both sides unless we already saw a
        // close from the WS peer.
        if (!ws_closed_by_peer) FD_SET(ws_fd, &rfds);
        FD_SET(tcp_fd, &rfds);

        if (!ws_outbox.empty())  FD_SET(ws_fd, &wfds);
        if (!tcp_outbox.empty()) FD_SET(tcp_fd, &wfds);

        SOCKET max_fd = (ws_fd > tcp_fd) ? ws_fd : tcp_fd;
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000; // 200 ms — responsive to stop()
        int n = ::select(static_cast<int>(max_fd) + 1, &rfds, &wfds,
                         nullptr, &tv);
        if (n < 0) {
            int e = wr_last_errno();
            if (wr_would_block(e)) continue;
#ifdef _WIN32
            if (e == WSAEINTR) continue;
#else
            if (e == EINTR) continue;
#endif
            break;
        }

        // ---- read from WebSocket --------------------------------
        if (!ws_closed_by_peer && FD_ISSET(ws_fd, &rfds)) {
            char rb[16 * 1024];
            int r = ::recv(ws_fd, rb, sizeof(rb), 0);
            if (r == 0) {
                ws_closed_by_peer = true;
            } else if (r < 0) {
                int e = wr_last_errno();
                if (!wr_would_block(e)) break;
            } else {
                ws_in_buf.append(rb, static_cast<size_t>(r));
            }

            // Drain as many frames as we have buffered. Each frame's
            // payload is forwarded straight onto the TCP socket
            // (queued in `tcp_outbox`; flushed below).
            for (;;) {
                int  opcode = 0;
                bool fin    = false;
                std::vector<char> payload;
                int rc = parse_one_frame(ws_in_buf, opcode, fin, payload);
                if (rc == 0) break;
                if (rc < 0) {
                    // Protocol error — drop both sides.
                    goto teardown;
                }

                if (opcode == 0x8) {
                    // Close — echo and mark for drain+drop.
                    ws_outbox += encode_close_frame();
                    ws_closed_by_peer = true;
                    break;
                } else if (opcode == 0x9) {
                    // Ping — echo as pong.
                    std::string body(payload.data(), payload.size());
                    ws_outbox += encode_pong_frame(body);
                    continue;
                } else if (opcode == 0xA) {
                    // Pong — ignore.
                    continue;
                }
                // Text(0x1) / Binary(0x2) / Continuation(0x0): the
                // relay treats them all as raw bytes to forward.
                // Emscripten only ever sends 0x2 in practice.
                if (!payload.empty()) {
                    tcp_outbox.append(payload.data(), payload.size());
                }
            }
        }

        // ---- read from TCP --------------------------------------
        if (FD_ISSET(tcp_fd, &rfds)) {
            char rb[16 * 1024];
            int r = ::recv(tcp_fd, rb, sizeof(rb), 0);
            if (r == 0) {
                // librats closed the connection — send a WS close
                // and drain.
                ws_outbox += encode_close_frame();
                goto teardown_after_drain;
            } else if (r < 0) {
                int e = wr_last_errno();
                if (!wr_would_block(e)) {
                    ws_outbox += encode_close_frame(1011);
                    goto teardown_after_drain;
                }
            } else if (r > 0) {
                ws_outbox += encode_binary_frame(rb, static_cast<size_t>(r));
            }
        }

        // ---- writes to WebSocket --------------------------------
        if (FD_ISSET(ws_fd, &wfds) && !ws_outbox.empty()) {
            int w = ::send(ws_fd, ws_outbox.data(),
                           static_cast<int>(ws_outbox.size()), 0);
            if (w > 0) ws_outbox.erase(0, static_cast<size_t>(w));
            else if (w < 0) {
                int e = wr_last_errno();
                if (!wr_would_block(e)) break;
            }
        }

        // ---- writes to TCP --------------------------------------
        if (FD_ISSET(tcp_fd, &wfds) && !tcp_outbox.empty()) {
            int w = ::send(tcp_fd, tcp_outbox.data(),
                           static_cast<int>(tcp_outbox.size()), 0);
            if (w > 0) tcp_outbox.erase(0, static_cast<size_t>(w));
            else if (w < 0) {
                int e = wr_last_errno();
                if (!wr_would_block(e)) break;
            }
        }

        // Exit once both sides are dead and we've flushed outboxes.
        if (ws_closed_by_peer && ws_outbox.empty() && tcp_outbox.empty()) break;
    }

teardown_after_drain:
    // Best-effort flush of any remaining outbound bytes. A slow
    // browser shouldn't wedge us forever — the select() above
    // bounds the loop anyway.
    {
        int spins = 50;
        while (!ws_outbox.empty() && spins-- > 0) {
            int w = ::send(ws_fd, ws_outbox.data(),
                           static_cast<int>(ws_outbox.size()), 0);
            if (w > 0) ws_outbox.erase(0, static_cast<size_t>(w));
            else break;
        }
    }

teardown:
    wr_close(ws_fd);
    wr_close(tcp_fd);
}

} // namespace

WsTcpRelay::~WsTcpRelay() { stop(); }

bool WsTcpRelay::start(uint16_t ws_port, uint16_t tcp_target_port) {
    if (running_.exchange(true)) return true;
    ws_port_         = ws_port;
    tcp_target_port_ = tcp_target_port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == WR_INVALID_SOCKET) {
        std::cerr << "[ws-tcp] socket() failed: errno="
                  << wr_last_errno() << "\n";
        running_.store(false);
        return false;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(ws_port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
        == WR_SOCKET_ERROR) {
        std::cerr << "[ws-tcp] bind() on port " << ws_port
                  << " failed: errno=" << wr_last_errno() << "\n";
        wr_close(fd);
        running_.store(false);
        return false;
    }
    if (::listen(fd, 32) == WR_SOCKET_ERROR) {
        std::cerr << "[ws-tcp] listen() failed: errno="
                  << wr_last_errno() << "\n";
        wr_close(fd);
        running_.store(false);
        return false;
    }
    set_nonblocking(fd);
    listen_fd_ = static_cast<uintptr_t>(fd);

    std::cout << "[ws-tcp] relay listening on 0.0.0.0:" << ws_port
              << " -> 127.0.0.1:" << tcp_target_port
              << " (browsers connect ws://<host>:" << ws_port << "/)\n";

    accept_thread_ = std::thread([this] { accept_loop_(); });
    return true;
}

void WsTcpRelay::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ != static_cast<uintptr_t>(-1)) {
        wr_close(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = static_cast<uintptr_t>(-1);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    // Per-connection worker threads are detached — they observe
    // the running_ flag and exit on their own. This matches the
    // home-node ws_bridge contract of "stop() returns once the
    // accept thread is joined; in-flight connections drain
    // asynchronously".
}

void WsTcpRelay::accept_loop_() {
    while (running_.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        SOCKET lfd = static_cast<SOCKET>(listen_fd_);
        if (lfd == WR_INVALID_SOCKET) break;
        FD_SET(lfd, &rfds);

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000;
        int n = ::select(static_cast<int>(lfd) + 1, &rfds, nullptr,
                         nullptr, &tv);
        if (n < 0) {
            int e = wr_last_errno();
            if (wr_would_block(e)) continue;
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
        if (cfd == WR_INVALID_SOCKET) continue;

        // The handshake is read in blocking mode for simplicity.
        // Browser librats sends the upgrade as a single TCP send,
        // so this completes in microseconds.

        int yes = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        // Spawn a dedicated worker thread for this connection. It
        // performs the upgrade handshake, opens the paired TCP
        // socket, then drives the byte pump until either side
        // closes.
        const uint16_t target_port = tcp_target_port_;
        const std::atomic<bool>* stop_flag = &running_;
        std::thread([cfd, target_port, stop_flag]() {
            std::string leftover;
            if (!perform_ws_handshake(cfd, leftover)) {
                wr_close(cfd);
                return;
            }

            SOCKET tcp_fd = open_paired_tcp(target_port);
            if (tcp_fd == WR_INVALID_SOCKET) {
                std::cerr << "[ws-tcp] dial 127.0.0.1:" << target_port
                          << " failed: errno=" << wr_last_errno() << "\n";
                // Tell the browser side this is dead.
                std::string close_frame = encode_close_frame(1011);
                send_all(cfd, close_frame.data(), close_frame.size());
                wr_close(cfd);
                return;
            }

            worker_loop(cfd, tcp_fd, std::move(leftover), stop_flag);
        }).detach();
    }
}

} // namespace mc::transport
