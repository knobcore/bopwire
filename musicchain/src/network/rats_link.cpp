#include "rats_link.h"

// `mc_rats_quic.h` was the old transparent shim that exported the librats C
// API directly. We now link the real librats (deps/librats) compiled with a
// QUIC backend, so include its C bindings header instead.
#include "librats_c.h"

// nlohmann::json is vendored inside librats; reuse it here so we can build
// the JSON envelopes for the stun.observe RPC without dragging in another
// JSON library.
#include "../../deps/librats/src/json.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>

namespace mc::net {

RatsLink::RatsLink(uint16_t listen_port,
                   std::string node_id_hex,
                   uint16_t own_api_port)
    : listen_port_(listen_port),
      node_id_hex_(std::move(node_id_hex)),
      own_api_port_(own_api_port) {}

RatsLink::~RatsLink() { stop(); }

bool RatsLink::start() {
    if (client_) return true;

    client_ = rats_create(listen_port_);
    if (!client_) {
        std::cerr << "[rats] rats_create failed on port " << listen_port_ << "\n";
        return false;
    }

    if (rats_start(client_) != 0) {
        std::cerr << "[rats] rats_start failed\n";
        rats_destroy(client_);
        client_ = nullptr;
        return false;
    }

    // Public address discovery is no longer done by sending raw STUN packets
    // (msquic on the VPS sits on UDP/443 and drops anything that isn't QUIC).
    // Instead, observe_public_address_via_vps() — invoked from route_loop
    // once the VPS rats handshake completes — sends a librats `stun.observe`
    // RPC over the same QUIC connection and stores the address the
    // mini-node sees us from.

    // Capture our own librats peer id so we can advertise it to the mini-node.
    char* pid = rats_get_our_peer_id(client_);
    if (pid) {
        if (*pid) {
            std::lock_guard<std::mutex> lk(pub_mu_);
            rats_peer_id_ = pid;
            std::cout << "[rats] own peer id: " << rats_peer_id_ << "\n";
        }
        rats_string_free(pid);
    }

    // Resolve the bootstrap-VPS list from the env var (or fall back to
    // the legacy single-host constant). Multi-entry support lets us mesh
    // with multiple mini-nodes so any one going down doesn't strand us.
    load_bootstrap_list();
    for (const auto& vps : bootstrap_vps_) {
        if (rats_connect(client_, vps.host.c_str(), vps.port) != 0) {
            std::cerr << "[rats] could not reach VPS bootstrap "
                      << vps.host << ":" << vps.port
                      << " (will retry automatically)\n";
        } else {
            std::cout << "[rats] connected to VPS bootstrap "
                      << vps.host << ":" << vps.port << "\n";
        }
    }

    rats_start_automatic_peer_discovery(client_);

    // Subscribe to the routing topic so we receive other nodes' routes too
    // (mostly informational here — the VPS mini-node is the real aggregator).
    rats_subscribe_to_topic(client_, MC_ROUTES_TOPIC);

    running_ = true;

    // Kick off the 15-minute route-broadcast thread.
    route_thread_running_ = true;
    route_thread_ = std::thread([this]{ route_loop(); });

    return true;
}

void RatsLink::stop() {
    if (!client_) return;
    running_ = false;
    route_thread_running_ = false;
    if (route_thread_.joinable()) route_thread_.join();
    rats_stop_automatic_peer_discovery(client_);
    rats_stop(client_);
    rats_destroy(client_);
    client_ = nullptr;
}

std::string RatsLink::public_address() const {
    std::lock_guard<std::mutex> lk(pub_mu_);
    return public_addr_;
}

std::vector<std::string> RatsLink::peer_ids() const {
    std::vector<std::string> out;
    if (!client_) return out;
    int count = 0;
    char** ids = rats_get_validated_peer_ids(client_, &count);
    if (!ids) return out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (ids[i]) {
            out.emplace_back(ids[i]);
            rats_string_free(ids[i]);
        }
    }
    std::free(ids);
    return out;
}

void RatsLink::broadcast(const void* data, size_t size) {
    if (!client_ || !data || size == 0) return;
    rats_broadcast_binary(client_, data, size);
}

std::string RatsLink::build_route_json() const {
    const auto now = std::chrono::system_clock::now();
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count());
    std::string pub, rid;
    {
        std::lock_guard<std::mutex> lk(pub_mu_);
        pub = public_addr_;
        rid = rats_peer_id_;
    }
    std::ostringstream ss;
    ss << "{\"node_id\":\""        << node_id_hex_  << "\","
       << "\"rats_peer_id\":\""    << rid           << "\","
       << "\"public_address\":\""  << pub           << "\","
       << "\"api_port\":"          << own_api_port_ << ","
       << "\"ts\":"                << ts            << "}";
    return ss.str();
}

std::string RatsLink::rats_peer_id() const {
    std::lock_guard<std::mutex> lk(pub_mu_);
    return rats_peer_id_;
}

void RatsLink::publish_route_now() {
    if (!client_) return;
    const auto json = build_route_json();

    // Send directly to every validated peer instead of relying on the
    // GossipSub mesh — librats' GossipSub layer needs a mesh form-up
    // window that takes longer than the 15-minute interval we want.
    // Direct typed messages with rats_send_message reach the connected
    // mini-node immediately on every call.
    int count = 0;
    char** ids = rats_get_validated_peer_ids(client_, &count);
    int delivered = 0;
    if (ids) {
        for (int i = 0; i < count; ++i) {
            if (!ids[i]) continue;
            if (rats_send_message(client_, ids[i],
                                  MC_ROUTES_TOPIC, json.c_str()) == RATS_SUCCESS) {
                ++delivered;
            }
            rats_string_free(ids[i]);
        }
        std::free(ids);
    }
    std::cout << "[rats] route published to " << delivered
              << "/" << count << " peers\n";

    // GossipSub-mesh publish removed 2026-06-03: rats_publish_to_topic with
    // a freshly-connected peer was racing the GossipSub GRAFT handshake and
    // appeared to be triggering a librats disconnect ~1 s later. The direct
    // typed-message send above already delivers the route to the connected
    // mini-node, which is what we actually rely on. If we wire up a real
    // multi-node mesh later, re-enable this *after* the GossipSub
    // grace-period has elapsed.
}

// ---------------------------------------------------------------------
// stun.observe — observe the VPS-side address via a librats RPC. The
// mini-node has a verb that just echoes back the QUIC remote address it
// sees the caller from. We use librats's own typed-message hook for the
// reply rather than building a generic request/reply layer here.


void RatsLink::observe_public_address_via_vps() {
    // Vanilla librats has native STUN support, so we no longer need the
    // VPS hop. We just call its `discover_public_address` API against
    // Google's public STUN server (the canonical default) and store the
    // mapped IP:port for inclusion in our route_publish messages.
    char* result = rats_discover_public_address(client_,
                                                  "stun.l.google.com", 19302, 3000);
    if (!result) return;
    {
        std::lock_guard<std::mutex> lk(pub_mu_);
        public_addr_ = result;
        std::cout << "[rats] public address (native STUN): " << public_addr_ << "\n";
    }
    rats_string_free(result);
}

// Check if we still have at least one validated rats peer (the VPS).
// Used by the watchdog below — when this drops to zero the full node has
// effectively disconnected from the mesh and stream.open / player.locate
// stop reaching anyone.
int RatsLink::validated_peer_count() const {
    if (!client_) return 0;
    int count = 0;
    char** ids = rats_get_validated_peer_ids(client_, &count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            if (ids[i]) rats_string_free(ids[i]);
        }
        std::free(ids);
    }
    return count;
}

// Dial every bootstrap VPS again. Idempotent in async mode — librats
// throttles duplicates internally. Called from the watchdog tick whenever
// we notice the validated-peer set went empty.
//
// rats_connect returns 1 when librats successfully *initiated* the dial
// (TCP SYN queued, handshake will land later) and 0 when it couldn't
// even resolve / queue the socket. The old polarity was reversed and
// printed the warning on success, which produced an endless "watchdog
// pending" stream in the F2 log even when the redial was actually
// working — only the absence of a follow-up "VPS link up" line was
// the real signal that the handshake hadn't validated yet.
void RatsLink::redial_vps() {
    if (!client_) return;
    if (bootstrap_vps_.empty()) load_bootstrap_list();
    for (const auto& vps : bootstrap_vps_) {
        if (rats_connect(client_, vps.host.c_str(), vps.port) != 0) {
            // librats accepted the dial. The handshake will either land
            // in on_peer_connected (→ "VPS link up") or quietly time
            // out, and the next tick will redial again — no log line
            // each tick keeps the F2 stream readable.
            continue;
        }
        std::cerr << "[rats] watchdog redial of VPS bootstrap "
                  << vps.host << ":" << vps.port
                  << " failed (host unresolved / socket refused)\n";
    }
}

// Populate bootstrap_vps_ from the env var MUSICCHAIN_VPS_BOOTSTRAP, which
// is a comma-separated host:port list (e.g. "vps1.example:8080,vps2.example:8080").
// Falls back to a single entry pointing at MC_VPS_HOST:MC_VPS_RATS_PORT so
// existing single-VPS deploys keep working with no env tweak.
void RatsLink::load_bootstrap_list() {
    bootstrap_vps_.clear();
    const char* env = std::getenv("MUSICCHAIN_VPS_BOOTSTRAP");
    if (env && *env) {
        const std::string s = env;
        size_t start = 0;
        while (start < s.size()) {
            const size_t comma = s.find(',', start);
            const std::string entry = s.substr(start,
                comma == std::string::npos ? std::string::npos : comma - start);
            const auto colon = entry.rfind(':');
            if (colon != std::string::npos) {
                const std::string host = entry.substr(0, colon);
                try {
                    const int port = std::stoi(entry.substr(colon + 1));
                    if (!host.empty() && port > 0 && port < 65536) {
                        bootstrap_vps_.push_back({host,
                            static_cast<uint16_t>(port)});
                    }
                } catch (...) {/* malformed entry — skip */}
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (bootstrap_vps_.empty()) {
        bootstrap_vps_.push_back({MC_VPS_HOST, MC_VPS_RATS_PORT});
    }
}

void RatsLink::route_loop() {
    observe_public_address_via_vps();

    // Single loop that does three things on a fast cadence so a missed
    // handshake (e.g. the VPS was restarting when we first dialed) auto-
    // recovers within seconds instead of needing a manual restart:
    //
    //   1. If we have no validated peer, redial the VPS. This used to
    //      be a one-shot at start() and a 30-90-300-second warmup;
    //      the warmup left the node dead-on-arrival for 30+ s after
    //      any blip.
    //   2. After a handshake first appears, send our route_publish so
    //      the mini-node's routing table sees us immediately.
    //   3. Every 5 minutes of steady-state, republish routes so the
    //      mini-node doesn't time us out.
    constexpr int kTickSeconds          = 3;
    constexpr int kSteadyRepublishTicks = (5 * 60) / kTickSeconds;
    bool was_up        = false;
    int  steady_ticks  = 0;

    while (route_thread_running_) {
        const int peers = validated_peer_count();
        const bool up   = peers > 0;
        if (!up) {
            if (was_up) {
                std::cout << "[rats] VPS link dropped, watchdog dialing\n";
            }
            redial_vps();
            steady_ticks = 0;
        } else {
            if (!was_up) {
                std::cout << "[rats] VPS link up (" << peers
                          << " peer" << (peers == 1 ? "" : "s")
                          << ") — publishing route\n";
                publish_route_now();
                // Re-run STUN now that we have a live link — gives the
                // route record an accurate public_address sooner.
                observe_public_address_via_vps();
                steady_ticks = 0;
            } else if (++steady_ticks >= kSteadyRepublishTicks) {
                publish_route_now();
                steady_ticks = 0;
            }
        }
        was_up = up;
        for (int i = 0; i < kTickSeconds && route_thread_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace mc::net
