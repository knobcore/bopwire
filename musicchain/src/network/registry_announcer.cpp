#include "registry_announcer.h"
#include "manager.h"
#include "../crypto/hash.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <random>

using json = nlohmann::json;

namespace mc::net {

// ---- curl helpers ---------------------------------------------------

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* buf = static_cast<std::string*>(ud);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ---- Constructor / Destructor ---------------------------------------

HttpGossip::HttpGossip(NetworkManager&    network,
                       const std::string& own_node_id_hex,
                       const std::string& own_ipv6,
                       uint16_t           own_api_port,
                       const std::string& bootstrap_url,
                       const std::string& peers_file)
    : network_(network), own_node_id_hex_(own_node_id_hex),
      own_ipv6_(own_ipv6), own_api_port_(own_api_port),
      bootstrap_url_(bootstrap_url), peers_file_(peers_file) {}

HttpGossip::~HttpGossip() { stop(); }

void HttpGossip::start() {
    running_ = true;
    thread_  = std::thread([this]{ run(); });
}

void HttpGossip::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ---- librats peer injection -----------------------------------------

void HttpGossip::inject_rats_peer(const std::string& ipv4, uint16_t api_port) {
    // Build a candidate URL from the raw IPv4 address and api_port.
    // Announce ourselves to it; if reachable it will call crawl back.
    std::string url = "http://" + ipv4 + ":" + std::to_string(api_port);
    // Quick GET /api/v1/status to verify it's actually a MusicChain node
    std::string status = http_get(url + "/api/v1/status");
    if (status.empty()) return;  // not reachable
    crawl_peer(url);
    http_post_announce(url);
    std::cout << "[gossip] rats peer injected: " << ipv4 << ":" << api_port << "\n";
}

// ---- Main thread ----------------------------------------------------

void HttpGossip::run() {
    // Seed from disk (survived previous run)
    load_peers_from_file();

    // Bootstrap: crawl + announce to the configured well-known node
    if (!bootstrap_url_.empty()) {
        crawl_peer(bootstrap_url_);
        http_post_announce(bootstrap_url_);
    }

    // First gossip round immediately
    gossip_round();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (std::chrono::steady_clock::now() >= deadline) {
            gossip_round();
            deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        }
    }
}

void HttpGossip::gossip_round() {
    auto peers = network_.get_dht_peers();
    if (peers.empty()) return;

    uint32_t local_height = network_.chain_height();

    // Shuffle for fairness; cap at 50 peers per round
    std::shuffle(peers.begin(), peers.end(), std::mt19937{std::random_device{}()});
    if (peers.size() > 50) peers.resize(50);

    for (const auto& p : peers) {
        if (!running_) break;
        std::string url = p.api_url();
        if (url.empty()) continue;

        // Pull blocks we don't have yet, then update peer lists
        sync_blocks_from_peer(url, local_height);
        crawl_peer(url);
        http_post_announce(url);
    }

    save_peers_to_file();
    std::cout << "[gossip] round complete; " << network_.get_dht_peers().size()
              << " known peers, chain height " << network_.chain_height() << "\n";
}

// ---- Block sync -----------------------------------------------------

void HttpGossip::sync_blocks_from_peer(const std::string& peer_url, uint32_t after_height) {
    std::string stripped = peer_url;
    while (!stripped.empty() && stripped.back() == '/') stripped.pop_back();

    std::string url = stripped + "/api/v1/sync/blocks?after_height="
                    + std::to_string(after_height) + "&limit=50";
    std::string body = http_get(url);
    if (body.empty()) return;

    try {
        auto arr = json::parse(body);
        if (!arr.is_array() || arr.empty()) return;

        std::string local_base = "http://localhost:" + std::to_string(own_api_port_);
        for (const auto& entry : arr) {
            if (!running_) break;
            std::string raw_hex = entry.value("raw_hex", "");
            if (raw_hex.empty()) continue;
            // Forward the raw block to the local API for chain connection
            http_post_json(local_base + "/api/v1/sync/block",
                           "{\"raw_hex\":\"" + raw_hex + "\"}");
        }
    } catch (...) {}
}

// ---- Crawl one node -------------------------------------------------

void HttpGossip::crawl_peer(const std::string& api_url) {
    std::string stripped = api_url;
    while (!stripped.empty() && stripped.back() == '/') stripped.pop_back();

    std::string body = http_get(stripped + "/api/v1/net/dht-peers");
    if (body.empty()) return;

    try {
        auto j        = json::parse(body);
        auto peer_arr = j.value("peers", json::array());
        for (const auto& entry : peer_arr) {
            std::string ipv6        = entry.value("ipv6",     "");
            uint16_t    api_port    = static_cast<uint16_t>(entry.value("api_port", 0));
            std::string node_id_hex = entry.value("node_id",  "");
            if (node_id_hex.size() != 64) continue;
            if (ipv6.empty()) continue;
            Hash256 node_id{};
            if (!mc::crypto::parse_hash256(node_id_hex, node_id)) continue;
            network_.inject_peer(ipv6, api_port, node_id);
        }
    } catch (...) {}
}

// ---- HTTP helpers ---------------------------------------------------

std::string HttpGossip::http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string buf;
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,    1L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return buf;
}

bool HttpGossip::http_post_json(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);  // blocks can be large
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

bool HttpGossip::http_post_announce(const std::string& base_url) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/api/v1/net/announce";

    std::ostringstream ss;
    ss << "{\"node_id\":\"" << own_node_id_hex_ << "\","
       << "\"ipv6\":\""     << own_ipv6_         << "\","
       << "\"api_port\":"   << own_api_port_     << "}";
    std::string body = ss.str();

    return http_post_json(url, body);
}

// ---- Persistence ----------------------------------------------------

void HttpGossip::load_peers_from_file() {
    std::ifstream f(peers_file_);
    if (!f.is_open()) return;
    try {
        auto arr = json::parse(f);
        int loaded = 0;
        for (const auto& entry : arr) {
            std::string ipv6        = entry.value("ipv6",    "");
            uint16_t    api_port    = static_cast<uint16_t>(entry.value("api_port", 0));
            std::string node_id_hex = entry.value("node_id", "");
            if (node_id_hex.size() != 64 || ipv6.empty()) continue;
            Hash256 node_id{};
            if (!mc::crypto::parse_hash256(node_id_hex, node_id)) continue;
            network_.inject_peer(ipv6, api_port, node_id);
            ++loaded;
        }
        if (loaded > 0)
            std::cout << "[gossip] loaded " << loaded << " peers from " << peers_file_ << "\n";
    } catch (...) {}
}

void HttpGossip::save_peers_to_file() {
    auto peers = network_.get_dht_peers();
    json arr   = json::array();
    for (const auto& p : peers) {
        if (p.api_port == 0) continue;
        arr.push_back({
            {"node_id",  mc::crypto::to_hex(p.node_id)},
            {"ipv6",     p.ipv6_str()},
            {"api_port", p.api_port},
        });
    }
    std::ofstream f(peers_file_);
    if (f.is_open()) f << arr.dump(2);
}

} // namespace mc::net
