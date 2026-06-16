#include "peer.h"
#include <cstring>
#include <stdexcept>

namespace mc::net {

Peer::Peer(uv_loop_t* loop, const PeerInfo& info)
    : loop_(loop), info_(info) {
    uv_tcp_init(loop_, &tcp_);
    tcp_.data = this;
}

Peer::~Peer() {
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&tcp_)))
        uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), nullptr);
}

bool Peer::connect() {
    struct sockaddr_in addr{};
    uv_ip4_addr(info_.host.c_str(), info_.port, &addr);

    auto* req = new uv_connect_t;
    req->data = this;
    int r = uv_tcp_connect(req, &tcp_, reinterpret_cast<const sockaddr*>(&addr),
                            on_connect_cb);
    return r == 0;
}

void Peer::on_connect_cb(uv_connect_t* req, int status) {
    auto* self = static_cast<Peer*>(req->data);
    delete req;
    if (status < 0) {
        if (self->on_disconnect_) self->on_disconnect_(self);
        return;
    }
    self->connected_ = true;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&self->tcp_),
                  alloc_cb, on_read_cb);
}

void Peer::alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    (void)handle;
    buf->base = new char[suggested];
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void Peer::on_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<Peer*>(stream->data);
    if (nread > 0) {
        self->feed_data(reinterpret_cast<const uint8_t*>(buf->base), static_cast<size_t>(nread));
    } else if (nread < 0) {
        self->connected_ = false;
        if (self->on_disconnect_) self->on_disconnect_(self);
    }
    delete[] buf->base;
}

void Peer::feed_data(const uint8_t* data, size_t len) {
    recv_buf_.insert(recv_buf_.end(), data, data + len);
    process_received();
}

void Peer::process_received() {
    while (!recv_buf_.empty()) {
        Message msg;
        size_t consumed = 0;
        if (!Message::parse(recv_buf_.data(), recv_buf_.size(), msg, consumed)) break;
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + consumed);
        if (on_message_) on_message_(this, std::move(msg));
    }
}

void Peer::send(const Message& msg) {
    if (!connected_) return;
    auto data = msg.serialize();
    auto* buf = new uv_buf_t;
    auto* raw = new char[data.size()];
    std::memcpy(raw, data.data(), data.size());
    *buf = uv_buf_init(raw, static_cast<unsigned int>(data.size()));
    auto* req = new uv_write_t;
    req->data = raw;
    uv_write(req, reinterpret_cast<uv_stream_t*>(&tcp_), buf, 1, on_write_cb);
    delete buf;
}

void Peer::on_write_cb(uv_write_t* req, int /*status*/) {
    delete[] static_cast<char*>(req->data);
    delete req;
}

void Peer::close() {
    connected_ = false;
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&tcp_)))
        uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), nullptr);
}

} // namespace mc::net
