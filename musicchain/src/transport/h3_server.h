#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc::api { class HttpServer; }

namespace mc::transport {

/// HTTP-shaped response produced by routing an HTTP request to the
/// underlying HttpServer verb container.
struct H3Response {
    int                  status        = 0;
    std::string          content_type  = "application/json";
    std::vector<uint8_t> body;
};

/// HTTP/3 server. Two roles:
///   • `start(port)` binds an on-wire msh3 listener (real HTTP/3 over QUIC)
///     so curl --http3 / browsers / other HTTP/3 clients can hit the API
///     directly. Optional — most musicchain traffic now rides librats.
///   • `dispatch(method, path, body)` exposes the routing logic so the
///     `h3.request` librats verb (src/api/rats_api.cpp) can tunnel HTTP-
///     shaped calls through the UDP/443 librats QUIC channel.
class H3Server {
public:
    explicit H3Server(mc::api::HttpServer& verbs);
    ~H3Server();

    /// Start the on-wire HTTP/3 listener on `port` (UDP). Returns false on
    /// bind failure. Skip this call if you only want the dispatch path.
    bool start(uint16_t port);
    void stop();

    bool is_running() const { return listener_ != nullptr; }
    mc::api::HttpServer& verbs() { return verbs_; }

    /// Translate (method, path-with-query, body) into a routed response.
    /// Used by both the on-wire HTTP/3 layer and the `h3.request` rats verb.
    H3Response dispatch(const std::string& method,
                        const std::string& path,
                        const std::string& body);

private:
    mc::api::HttpServer& verbs_;
    void*  api_       = nullptr;  // MSH3_API*
    void*  config_    = nullptr;  // MSH3_CONFIGURATION*
    void*  listener_  = nullptr;  // MSH3_LISTENER*
    uint16_t port_    = 0;
};

} // namespace mc::transport
