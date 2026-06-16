#include "h3_server.h"
#include "cert_util.h"
#include "../api/server.h"

#include <msh3.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::transport {

namespace {

// ---- Per-request state captured across HEADER + DATA events ----------

struct H3Request {
    std::string  method;
    std::string  path;        // includes "?query"
    std::string  body;
    bool         received_done = false;
};

std::mutex                                            g_req_mu;
std::unordered_map<MSH3_REQUEST*, std::unique_ptr<H3Request>> g_reqs;

H3Request* get_req(MSH3_REQUEST* r) {
    std::lock_guard<std::mutex> lk(g_req_mu);
    auto it = g_reqs.find(r);
    return it == g_reqs.end() ? nullptr : it->second.get();
}

// ---- Path parsing ----------------------------------------------------

std::vector<std::string> split_segments(const std::string& path) {
    std::vector<std::string> out;
    size_t pos = 0;
    // Strip leading "/api/v1" if present so verbs are addressed by /status etc.
    const std::string prefix = "/api/v1";
    if (path.rfind(prefix, 0) == 0) pos = prefix.size();
    while (pos < path.size()) {
        if (path[pos] == '/') { ++pos; continue; }
        if (path[pos] == '?') break;
        auto end = path.find_first_of("/?", pos);
        if (end == std::string::npos) end = path.size();
        out.emplace_back(path.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query(const std::string& path) {
    std::unordered_map<std::string, std::string> q;
    auto qpos = path.find('?');
    if (qpos == std::string::npos) return q;
    size_t pos = qpos + 1;
    while (pos < path.size()) {
        auto amp = path.find('&', pos);
        if (amp == std::string::npos) amp = path.size();
        auto eq = path.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            q[path.substr(pos, eq - pos)] = path.substr(eq + 1, amp - eq - 1);
        }
        pos = amp + 1;
    }
    return q;
}

H3Response reply_json(std::pair<int, std::string> r) {
    H3Response out;
    out.status = r.first;
    out.body.assign(r.second.begin(), r.second.end());
    return out;
}

H3Response do_dispatch(mc::api::HttpServer& verbs,
                       const std::string& method,
                       const std::string& path,
                       const std::string& body) {
    auto segs = split_segments(path);
    auto qs   = parse_query(path);

    if (segs.empty()) {
        return reply_json({404, R"({"error":"not found"})"});
    }

    if (method == "GET") {
        if (segs[0] == "status" && segs.size() == 1) return reply_json(verbs.verb_status());
        if (segs[0] == "songs") {
            if (segs.size() == 1) return reply_json(verbs.verb_songs_list());
            if (segs.size() == 2 && segs[1] == "search") {
                auto a = qs.find("artist"); auto g = qs.find("genre"); auto q = qs.find("q");
                if (a != qs.end()) return reply_json(verbs.verb_songs_search_artist(a->second));
                if (g != qs.end()) return reply_json(verbs.verb_songs_search_genre(g->second));
                if (q != qs.end()) return reply_json(verbs.verb_songs_search_query(q->second));
                return reply_json(verbs.verb_songs_search_query(""));
            }
            if (segs.size() == 2) return reply_json(verbs.verb_song_get(segs[1]));
            if (segs.size() == 3 && segs[2] == "stream") {
                auto a = verbs.verb_song_audio(segs[1]);
                H3Response out;
                out.status = a.status;
                out.content_type = a.status == 200 ? a.content_type : "application/json";
                if (a.status == 200) out.body = std::move(a.bytes);
                else out.body.assign(a.error_json.begin(), a.error_json.end());
                return out;
            }
        }
        if (segs[0] == "balances" && segs.size() == 2)
            return reply_json(verbs.verb_wallet_balance(segs[1]));
        if (segs[0] == "wallet" && segs.size() == 3 && segs[2] == "nonce")
            return reply_json(verbs.verb_wallet_nonce(segs[1]));
        if (segs[0] == "upload" && segs.size() == 2)
            return reply_json(verbs.verb_upload_status(segs[1]));
    }

    if (method == "POST") {
        if (segs[0] == "sessions" && segs.size() == 2 && segs[1] == "start")
            return reply_json(verbs.verb_session_start(body));
        if (segs[0] == "sessions" && segs.size() == 3 && segs[2] == "heartbeat")
            return reply_json(verbs.verb_session_heartbeat(segs[1], body));
        if (segs[0] == "sessions" && segs.size() == 3 && segs[2] == "complete")
            return reply_json(verbs.verb_session_complete(segs[1], body));
    }

    return reply_json({404, R"({"error":"unknown route"})"});
}

// ---- msh3 request callback ------------------------------------------

void send_response(MSH3_REQUEST* req, H3Response&& resp) {
    char status_str[8];
    std::snprintf(status_str, sizeof(status_str), "%d", resp.status);
    char clen_str[32];
    std::snprintf(clen_str, sizeof(clen_str), "%zu", resp.body.size());

    MSH3_HEADER headers[3] = {
        { ":status",        7, status_str, std::strlen(status_str) },
        { "content-type",  12, resp.content_type.c_str(), resp.content_type.size() },
        { "content-length",14, clen_str, std::strlen(clen_str) },
    };
    auto* keep = new std::vector<uint8_t>(std::move(resp.body));
    MsH3RequestSend(req, MSH3_REQUEST_SEND_FLAG_FIN, headers, 3,
                     keep->data(), (uint32_t)keep->size(), keep);
}

MSH3_STATUS MSH3_CALL on_request_event(MSH3_REQUEST* req, void* ctx,
                                       MSH3_REQUEST_EVENT* ev) {
    auto* server = static_cast<H3Server*>(ctx);

    switch (ev->Type) {
        case MSH3_REQUEST_EVENT_HEADER_RECEIVED: {
            auto* h = ev->HEADER_RECEIVED.Header;
            std::string name(h->Name,  h->NameLength);
            std::string value(h->Value, h->ValueLength);
            auto* r = get_req(req);
            if (!r) {
                std::lock_guard<std::mutex> lk(g_req_mu);
                g_reqs[req] = std::make_unique<H3Request>();
                r = g_reqs[req].get();
            }
            if (name == ":method") r->method = value;
            else if (name == ":path") r->path = value;
            break;
        }
        case MSH3_REQUEST_EVENT_DATA_RECEIVED: {
            auto* r = get_req(req);
            if (!r) break;
            const auto& d = ev->DATA_RECEIVED;
            r->body.append(reinterpret_cast<const char*>(d.Data), d.Length);
            break;
        }
        case MSH3_REQUEST_EVENT_PEER_SEND_SHUTDOWN: {
            auto* r = get_req(req);
            if (!r) { MsH3RequestShutdown(req, MSH3_REQUEST_SHUTDOWN_FLAG_ABORT, 0); break; }
            if (!server) break;
            H3Response resp = do_dispatch(server->verbs(), r->method, r->path, r->body);
            send_response(req, std::move(resp));
            break;
        }
        case MSH3_REQUEST_EVENT_SEND_COMPLETE: {
            if (ev->SEND_COMPLETE.ClientContext) {
                delete static_cast<std::vector<uint8_t>*>(ev->SEND_COMPLETE.ClientContext);
            }
            break;
        }
        case MSH3_REQUEST_EVENT_SHUTDOWN_COMPLETE: {
            std::lock_guard<std::mutex> lk(g_req_mu);
            g_reqs.erase(req);
            MsH3RequestClose(req);
            break;
        }
        default: break;
    }
    return MSH3_STATUS_SUCCESS;
}

// ---- msh3 connection callback (just steers requests to our handler) --

MSH3_STATUS MSH3_CALL on_conn_event(MSH3_CONNECTION* /*conn*/, void* ctx,
                                    MSH3_CONNECTION_EVENT* ev) {
    if (ev->Type == MSH3_CONNECTION_EVENT_NEW_REQUEST) {
        MsH3RequestSetCallbackHandler(ev->NEW_REQUEST.Request,
                                       on_request_event, ctx);
    }
    return MSH3_STATUS_SUCCESS;
}

// Shared between listener callback and H3Server::start so the static
// listener callback can hand each new connection its configuration.
MSH3_CONFIGURATION* g_active_config = nullptr;

MSH3_STATUS MSH3_CALL on_listener_event(MSH3_LISTENER* /*l*/, void* ctx,
                                        MSH3_LISTENER_EVENT* ev) {
    if (ev->Type != MSH3_LISTENER_EVENT_NEW_CONNECTION) return MSH3_STATUS_SUCCESS;
    MSH3_CONNECTION* conn = ev->NEW_CONNECTION.Connection;
    MsH3ConnectionSetCallbackHandler(conn, on_conn_event, ctx);
    if (!g_active_config) return MSH3_STATUS_INVALID_STATE;
    return MsH3ConnectionSetConfiguration(conn, g_active_config);
}

} // namespace

H3Server::H3Server(mc::api::HttpServer& verbs) : verbs_(verbs) {}
H3Server::~H3Server() { stop(); }

H3Response H3Server::dispatch(const std::string& method,
                              const std::string& path,
                              const std::string& body) {
    return do_dispatch(verbs_, method, path, body);
}

bool H3Server::start(uint16_t port) {
    if (listener_) return true;
    api_ = MsH3ApiOpen();
    if (!api_) { std::cerr << "[h3] MsH3ApiOpen failed\n"; return false; }
    config_ = MsH3ConfigurationOpen(static_cast<MSH3_API*>(api_), nullptr, 0);
    if (!config_) { std::cerr << "[h3] MsH3ConfigurationOpen failed\n"; return false; }
    g_active_config = static_cast<MSH3_CONFIGURATION*>(config_);

    auto cf = make_self_signed_files();
    if (!cf.ok) { std::cerr << "[h3] make_self_signed_files failed\n"; return false; }
    MSH3_CERTIFICATE_FILE cert_file{ cf.key_path.c_str(), cf.cert_path.c_str() };
    MSH3_CREDENTIAL_CONFIG cred{};
    cred.Type            = MSH3_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.CertificateFile = &cert_file;

    auto rc = MsH3ConfigurationLoadCredential(static_cast<MSH3_CONFIGURATION*>(config_), &cred);
    std::remove(cf.cert_path.c_str());
    std::remove(cf.key_path.c_str());
    if (MSH3_FAILED(rc)) {
        std::cerr << "[h3] ConfigurationLoadCredential failed: 0x" << std::hex << rc << std::dec << "\n";
        return false;
    }

    MSH3_ADDR addr{};
    MSH3_SET_PORT(&addr, port);
    listener_ = MsH3ListenerOpen(static_cast<MSH3_API*>(api_),
                                  &addr, on_listener_event, this);
    if (!listener_) {
        std::cerr << "[h3] MsH3ListenerOpen on port " << port << " failed\n";
        return false;
    }
    port_ = port;
    std::cout << "[h3] HTTP/3 listening on UDP " << port << "\n";
    return true;
}

void H3Server::stop() {
    if (listener_) { MsH3ListenerClose(static_cast<MSH3_LISTENER*>(listener_)); listener_ = nullptr; }
    if (config_)   { MsH3ConfigurationClose(static_cast<MSH3_CONFIGURATION*>(config_)); config_ = nullptr; }
    if (api_)      { MsH3ApiClose(static_cast<MSH3_API*>(api_)); api_ = nullptr; }
    g_active_config = nullptr;
}

} // namespace mc::transport
