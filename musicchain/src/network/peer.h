#pragma once
#include "messages.h"
#include <uv.h>
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace mc::net {

struct PeerInfo {
    std::string host;
    uint16_t    port;
    std::string address() const { return host + ":" + std::to_string(port); }
};

class Peer {
public:
    using MessageHandler = std::function<void(Peer*, Message)>;
    using DisconnectHandler = std::function<void(Peer*)>;

    Peer(uv_loop_t* loop, const PeerInfo& info);
    ~Peer();

    // Initiate outbound connection
    bool connect();

    // Send a message
    void send(const Message& msg);

    // Close connection
    void close();

    bool is_connected() const { return connected_; }

    const PeerInfo& info() const { return info_; }

    void set_message_handler(MessageHandler h)    { on_message_ = std::move(h); }
    void set_disconnect_handler(DisconnectHandler h) { on_disconnect_ = std::move(h); }

    // Called by network manager when data arrives on an accepted connection
    void feed_data(const uint8_t* data, size_t len);

    uint64_t last_ping_ms = 0;
    uint64_t last_pong_ms = 0;

private:
    uv_loop_t*      loop_;
    PeerInfo        info_;
    uv_tcp_t        tcp_{};
    bool            connected_ = false;

    std::vector<uint8_t> recv_buf_;
    MessageHandler       on_message_;
    DisconnectHandler    on_disconnect_;

    static void on_connect_cb(uv_connect_t* req, int status);
    static void on_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void on_write_cb(uv_write_t* req, int status);

    void process_received();
};

} // namespace mc::net
