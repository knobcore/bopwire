// h3_smoke — tiny HTTP/3 client that hits a local mc node's HTTPS verb and
// prints the body. Built against msh3. Used to sanity-check the H3Server.

#include <msh3.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex              g_mu;
std::condition_variable g_cv;
bool                    g_done   = false;
int                     g_status = 0;
std::string             g_body;

MSH3_STATUS MSH3_CALL on_req(MSH3_REQUEST* /*req*/, void*, MSH3_REQUEST_EVENT* ev) {
    switch (ev->Type) {
        case MSH3_REQUEST_EVENT_HEADER_RECEIVED: {
            auto* h = ev->HEADER_RECEIVED.Header;
            std::string n(h->Name, h->NameLength);
            std::string v(h->Value, h->ValueLength);
            if (n == ":status") g_status = std::atoi(v.c_str());
            break;
        }
        case MSH3_REQUEST_EVENT_DATA_RECEIVED: {
            const auto& d = ev->DATA_RECEIVED;
            g_body.append(reinterpret_cast<const char*>(d.Data), d.Length);
            // msh3 auto-completes the receive when this callback returns.
            break;
        }
        case MSH3_REQUEST_EVENT_PEER_SEND_SHUTDOWN: {
            // Peer is done sending. Signal main and let it close the
            // connection, which will tear down this request.
            std::lock_guard<std::mutex> lk(g_mu);
            g_done = true;
            g_cv.notify_all();
            break;
        }
        case MSH3_REQUEST_EVENT_SHUTDOWN_COMPLETE: {
            // No-op — request lifetime is managed by the connection close.
            break;
        }
        default: break;
    }
    return MSH3_STATUS_SUCCESS;
}

MSH3_STATUS MSH3_CALL on_conn(MSH3_CONNECTION*, void*, MSH3_CONNECTION_EVENT* ev) {
    if (ev->Type == MSH3_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT) {
        std::fprintf(stderr, "[h3-smoke] transport shutdown 0x%x\n",
                     (unsigned)ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        std::lock_guard<std::mutex> lk(g_mu);
        g_done = true;
        g_cv.notify_all();
    }
    return MSH3_STATUS_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    uint16_t    port = 9334;
    const char* path = "/status";
    if (argc > 1) host = argv[1];
    if (argc > 2) port = (uint16_t)std::atoi(argv[2]);
    if (argc > 3) path = argv[3];

    auto* api = MsH3ApiOpen();
    if (!api) { std::fprintf(stderr, "ApiOpen failed\n"); return 1; }

    MSH3_SETTINGS s{};
    s.IsSet.IdleTimeoutMs = 1; s.IdleTimeoutMs = 5000;
    auto* cfg = MsH3ConfigurationOpen(api, &s, sizeof(s));
    if (!cfg) { std::fprintf(stderr, "ConfigOpen failed\n"); return 1; }

    MSH3_CREDENTIAL_CONFIG cc{};
    cc.Type  = MSH3_CREDENTIAL_TYPE_NONE;
    cc.Flags = (MSH3_CREDENTIAL_FLAGS)(MSH3_CREDENTIAL_FLAG_CLIENT |
                                       MSH3_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
    auto rc = MsH3ConfigurationLoadCredential(cfg, &cc);
    if (MSH3_FAILED(rc)) {
        std::fprintf(stderr, "LoadCredential failed 0x%x\n", (unsigned)rc);
        return 1;
    }

    auto* conn = MsH3ConnectionOpen(api, on_conn, nullptr);
    if (!conn) { std::fprintf(stderr, "ConnectionOpen failed\n"); return 1; }

    MSH3_ADDR addr{};
    addr.Ipv4.sin_family = 2; // AF_INET
    MSH3_SET_PORT(&addr, port);
    inet_pton(2, host, &addr.Ipv4.sin_addr);

    rc = MsH3ConnectionStart(conn, cfg, host, &addr);
    if (MSH3_FAILED(rc)) {
        std::fprintf(stderr, "ConnectionStart failed 0x%x\n", (unsigned)rc);
        return 1;
    }

    auto* req = MsH3RequestOpen(conn, on_req, nullptr, MSH3_REQUEST_FLAG_NONE);
    if (!req) { std::fprintf(stderr, "RequestOpen failed\n"); return 1; }

    std::string host_hdr = std::string(host) + ":" + std::to_string(port);
    MSH3_HEADER headers[] = {
        { ":method",    7, "GET",   3 },
        { ":path",      5, path,    std::strlen(path) },
        { ":scheme",    7, "https", 5 },
        { ":authority", 10, host_hdr.c_str(), host_hdr.size() },
    };
    if (!MsH3RequestSend(req, MSH3_REQUEST_SEND_FLAG_FIN, headers, 4,
                          nullptr, 0, nullptr)) {
        std::fprintf(stderr, "RequestSend failed\n");
        return 1;
    }

    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait_for(lk, std::chrono::seconds(5), [] { return g_done; });

    std::printf("status=%d\n", g_status);
    std::printf("%s\n", g_body.c_str());
    std::fflush(stdout);

    // Note: we deliberately leak the api/cfg/conn handles. msh3 keeps a
    // worker thread alive that may still be running callbacks; tearing
    // it all down from the main thread races with that. This is a smoke
    // test — the process exit cleans up.
    return g_status == 200 ? 0 : 2;
}
