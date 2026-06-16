#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <vector>

namespace mc::net {

class NetworkManager;  // forward-declared to avoid circular include

/// Self-healing HTTP gossip mesh.
///
/// On startup:
///   1. Load previously-known peers from disk and inject them into the DHT.
///   2. Crawl + announce to the configured bootstrap node (if any).
///
/// Every 5 minutes (gossip round):
///   - Sync new blocks from every known peer via GET /api/v1/sync/blocks.
///   - GET /api/v1/net/dht-peers from every known peer → inject new peers.
///   - POST /api/v1/net/announce to every known peer → they learn about us.
///   - Persist the updated peer list to disk.
///
/// No external tunnel or relay is required. Nodes behind firewalls participate
/// by making outbound connections only; the mesh self-organises around them.
class HttpGossip {
public:
    HttpGossip(NetworkManager&    network,
               const std::string& own_node_id_hex,
               const std::string& own_ipv6,
               uint16_t           own_api_port,
               const std::string& bootstrap_url,  // may be empty
               const std::string& peers_file);    // path for persistence
    ~HttpGossip();

    void start();
    void stop();

    /// Called from the main loop with IPv4 addresses discovered via librats
    /// rendezvous. Probes the node on the given api_port and, if alive,
    /// injects it into the gossip mesh.
    void inject_rats_peer(const std::string& ipv4, uint16_t api_port);

private:
    NetworkManager& network_;
    std::string     own_node_id_hex_;
    std::string     own_ipv6_;
    uint16_t        own_api_port_;
    std::string     bootstrap_url_;
    std::string     peers_file_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    void run();
    void gossip_round();

    // HTTP helpers (blocking, short-timeout; called from gossip thread)
    std::string http_get(const std::string& url);
    bool        http_post_json(const std::string& url, const std::string& body);
    bool        http_post_announce(const std::string& base_url);

    // Pull any blocks newer than after_height from a peer and submit locally
    void sync_blocks_from_peer(const std::string& api_url, uint32_t after_height);

    // Crawl one node's peer list; inject all discovered peers
    void crawl_peer(const std::string& api_url);

    // Disk persistence
    void load_peers_from_file();
    void save_peers_to_file();
};

} // namespace mc::net
