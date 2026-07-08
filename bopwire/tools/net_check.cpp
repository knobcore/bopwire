#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "net_check.h"

#include "../src/network/upnp.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace mc::net {
namespace {

uint16_t parse_port(const std::string& ipport) {
    const auto c = ipport.rfind(':');
    if (c == std::string::npos) return 0;
    return static_cast<uint16_t>(std::atoi(ipport.substr(c + 1).c_str()));
}

// Measure up + down throughput against Cloudflare's public speed endpoints.
// Returns min(up, down) in bits/sec, or 0 on failure. Certificate
// verification is disabled — this is a bandwidth probe, not a trust
// operation, and a serving node shouldn't fail setup over a CA-bundle quirk.
uint64_t speed_test_bps() {
    const size_t N = 12ull * 1000 * 1000;   // 12 MB each way
    httplib::SSLClient cli("speed.cloudflare.com", 443);
    cli.enable_server_certificate_verification(false);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);

    uint64_t down_bps = 0, up_bps = 0;

    // Download
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Get(("/__down?bytes=" + std::to_string(N)).c_str());
        const auto t1 = std::chrono::steady_clock::now();
        if (res && res->status == 200 && !res->body.empty()) {
            const double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0.0)
                down_bps = static_cast<uint64_t>(res->body.size() * 8.0 / secs);
        }
    }
    // Upload
    {
        std::string payload(N, 'x');
        const auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Post("/__up", payload, "application/octet-stream");
        const auto t1 = std::chrono::steady_clock::now();
        if (res && (res->status == 200 || res->status == 204)) {
            const double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0.0)
                up_bps = static_cast<uint64_t>(payload.size() * 8.0 / secs);
        }
    }

    if (down_bps == 0 && up_bps == 0) return 0;
    if (down_bps == 0) return up_bps;
    if (up_bps   == 0) return down_bps;
    return std::min(down_bps, up_bps);
}

} // namespace

NetCheckResult run_net_check(uint16_t rats_port, uint16_t api_port,
                             std::function<std::string()> public_addr) {
    NetCheckResult r;

    std::cout << "[netcheck] opening ports via UPnP...\n";
    UpnpMapper mapper({
        {rats_port, "TCP", "bopwire-p2p"},
        {api_port,  "TCP", "bopwire-api"},
    });
    r.upnp_mapped = mapper.start();
    if (r.upnp_mapped)
        std::cout << "[netcheck] UPnP IGD found; ports mapped (WAN "
                  << mapper.external_ip() << ")\n";
    else
        std::cout << "[netcheck] no UPnP IGD (or mapping failed)\n";

    // Poll the STUN-observed public address (observed asynchronously once the
    // rats link connects to the rendezvous).
    std::string pub;
    for (int i = 0; i < 60 && pub.empty(); ++i) {
        pub = public_addr ? public_addr() : std::string();
        if (pub.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!pub.empty()) {
        r.stun_port_match = (parse_port(pub) == rats_port);
        std::cout << "[netcheck] STUN public=" << pub << " (listen port "
                  << (r.stun_port_match ? "matches -> reachable"
                                        : "REMAPPED by NAT") << ")\n";
    } else {
        std::cout << "[netcheck] no STUN public address observed\n";
    }

    // Reachable if the NAT preserves the port (public IP / full-cone) OR UPnP
    // opened a mapping for us.
    r.reachable = r.stun_port_match || r.upnp_mapped;

    if (r.reachable) {
        std::cout << "[netcheck] reachable; running speed test...\n";
        const uint64_t bps = speed_test_bps();
        if (bps > 0) {
            const uint64_t reserve = 10ull * 1000 * 1000;   // 10 Mbit headroom
            r.max_bandwidth_bps = (bps > reserve) ? (bps - reserve) : (bps / 2);
            r.measured = true;
            std::cout << "[netcheck] measured ~" << (bps / 1000000)
                      << " Mbps -> load ceiling " << (r.max_bandwidth_bps / 1000000)
                      << " Mbps (after 10 Mbit reserve)\n";
        } else {
            std::cout << "[netcheck] speed test failed; keeping configured capacity\n";
        }
    }
    return r;
}

} // namespace mc::net
