/**
 * node_main.cpp - Entry point for musicchain-node
 *
 * Usage:
 *   musicchain-node start [options]
 *   musicchain-node status
 *   musicchain-node peers
 *   musicchain-node sync-status
 *   musicchain-node stop
 *   musicchain-node rebuild-index
 *   musicchain-node verify-chain
 */

#include "../src/core/chain.h"
#include "../src/storage/database.h"
#include "../src/network/manager.h"
#include "../src/network/rats_link.h"
#include "../src/api/server.h"
#include "../src/api/rats_api.h"
// h3_server include removed: the standalone HTTP/3 listener was retired
// when verbs moved to librats RPC. Restore behind MC_WITH_H3 when bringing
// it back.
#include "../src/consensus/candidate.h"
#include "../src/consensus/validator.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/hash.h"
#include "node_tui.h"

#include "../deps/librats/src/librats_c.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <cstring>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ---- Configuration --------------------------------------------------

static mc::net::NodeConfig load_config(const std::string& path) {
    mc::net::NodeConfig cfg;
    if (!fs::exists(path)) return cfg;
    std::ifstream f(path);
    json j;
    f >> j;
    if (j.contains("data_dir"))        cfg.data_dir = j["data_dir"];
    if (j.contains("p2p_port"))        cfg.p2p_port = j["p2p_port"];
    if (j.contains("api_port"))        cfg.api_port = j["api_port"];
    if (j.contains("max_peers"))       cfg.max_peers = j["max_peers"];
    if (j.contains("validator_enabled")) cfg.validator_enabled = j["validator_enabled"];
    if (j.contains("log_level"))       cfg.log_level = j["log_level"];
    if (j.contains("seed_nodes")) {
        for (auto& s : j["seed_nodes"]) cfg.seed_nodes.push_back(s);
    }
    if (j.contains("registry_url"))  cfg.registry_url  = j["registry_url"];
    return cfg;
}

// ---- Signal handling ------------------------------------------------

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_relaxed);
}

// ---- Subcommand: start ----------------------------------------------

static int cmd_start(const std::vector<std::string>& args, const char* exe_path = nullptr) {
    mc::net::NodeConfig cfg;
    std::string config_path;
    // The TUI is now the default surface for `start`. Pass --no-tui or
    // --daemon when launching from systemd / service manager / Windows
    // service so the binary stays a plain log-only daemon and doesn't
    // leave the controlling terminal in raw mode.
    bool tui_mode = true;

    // Parse arguments
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i+1 < args.size())         { config_path = args[++i]; }
        else if (args[i] == "--data-dir" && i+1 < args.size())  { cfg.data_dir = args[++i]; }
        else if (args[i] == "--p2p-port" && i+1 < args.size())  { cfg.p2p_port = static_cast<uint16_t>(std::stoi(args[++i])); }
        else if (args[i] == "--api-port" && i+1 < args.size())  { cfg.api_port = static_cast<uint16_t>(std::stoi(args[++i])); }
        else if (args[i] == "--no-tui" || args[i] == "--daemon"
                                       || args[i] == "--quiet") { tui_mode = false; }
        else if (args[i] == "--tui")                            { tui_mode = true; }
    }

    // Divert all log output into the TUI's in-memory ring BEFORE chain
    // / database / network bringup starts spewing. Two layers needed:
    // (1) the librats DLL has its own std::cout / std::cerr instances
    //     inside mc_rats.dll's CRT — the EXE-side rdbuf swap can't reach
    //     them, so we use librats's own config to silence its console
    //     logger entirely.
    // (2) our own code (chain, rats_api, swarm) writes via the EXE's
    //     std::cout / std::cerr; the rdbuf swap inside start_log_capture
    //     diverts those into the ring.
    if (tui_mode) {
        rats_set_console_logging_enabled(0);
        rats_set_logging_enabled(0);
        mc::ui::start_log_capture();
    }

    // Load from file if provided (args override)
    if (!config_path.empty()) {
        auto file_cfg = load_config(config_path);
        if (cfg.data_dir.empty())  cfg.data_dir  = file_cfg.data_dir;
        if (cfg.p2p_port == 9333 && file_cfg.p2p_port != 9333) cfg.p2p_port = file_cfg.p2p_port;
        if (cfg.api_port == 9334 && file_cfg.api_port != 9334) cfg.api_port = file_cfg.api_port;
        if (cfg.seed_nodes.empty())    cfg.seed_nodes    = file_cfg.seed_nodes;
        if (cfg.registry_url.empty())  cfg.registry_url  = file_cfg.registry_url;
        if (!file_cfg.data_dir.empty() && cfg.data_dir.empty()) cfg.data_dir = file_cfg.data_dir;
    }

    if (cfg.data_dir.empty()) cfg.data_dir = "./data";

    // Create directories
    std::cerr << "[dbg] creating dirs\n";
    fs::create_directories(cfg.data_dir + "/blockchain.db");
    fs::create_directories(cfg.data_dir + "/keys");
    fs::create_directories(cfg.data_dir + "/logs");

    std::cout << "[node] data_dir : " << cfg.data_dir << "\n";
    std::cout << "[node] rats port: " << cfg.rats_port
              << " (UDP/QUIC — all traffic, RPC and h3 verbs)\n";
    std::cerr << "[dbg] opening database\n";

    // Open database
    mc::Database db(cfg.data_dir + "/blockchain.db");
    std::cerr << "[dbg] database opened\n";
    std::cout << "[node] database opened\n";

    // Load or generate keypair
    std::cerr << "[dbg] loading keypair\n";
    auto keypair = mc::crypto::load_or_generate_node_keypair(cfg.data_dir + "/keys");
    std::cerr << "[dbg] keypair loaded\n";
    cfg.node_id = mc::crypto::sha256(keypair.public_key.data(), 33);
    std::cout << "[node] node_id: " << mc::crypto::to_hex(cfg.node_id) << "\n";
    std::cout << "[node] address: " << mc::crypto::to_hex(keypair.address.data(), 20) << "\n";

    // Bootstrap first moderator wallet (runs once, guarded by sentinel key)
    std::cerr << "[dbg] moderator bootstrap\n";
    {
        auto sentinel = db.get("moderator_initialized");
        if (!sentinel) {
            leveldb::WriteBatch mod_batch;
            auto mod_kp = mc::crypto::generate_keypair();
            std::ofstream mod_f(cfg.data_dir + "/moderator.txt");
            mod_f << "Private Key: " << mc::crypto::to_hex(mod_kp.private_key.data(),
                                                            mod_kp.private_key.size()) << "\n";
            mod_f << "Address:     " << mc::crypto::to_hex(mod_kp.address.data(), 20) << "\n";
            mod_f.close();
            db.add_moderator(mod_batch, mod_kp.address);
            db.put_batch(mod_batch, "moderator_initialized", {1});
            db.write(mod_batch);
            std::cout << "[node] moderator wallet created — " << cfg.data_dir << "/moderator.txt\n";
            std::cout << "[node] moderator address: "
                      << mc::crypto::to_hex(mod_kp.address.data(), 20) << "\n";
        }
    }

    std::cerr << "[dbg] initializing chain\n";
    // Initialize chain
    mc::Chain chain(db);
    if (!chain.init()) {
        std::cerr << "[node] chain init failed\n";
        return 1;
    }
    std::cerr << "[dbg] chain init ok\n";
    std::cout << "[node] chain height: " << chain.tip().height << "\n";

    // Candidate manager + upload worker
    std::cerr << "[dbg] creating candidate manager\n";
    mc::CandidateManager candidates;

    // Network manager
    std::cerr << "[dbg] creating network manager\n";
    mc::net::NetworkManager network(chain, candidates, cfg, keypair);
    network.set_block_handler([&chain](mc::Block block) {
        std::string err;
        if (chain.connect_block(block)) {
            std::cout << "[chain] connected block at height " << chain.tip().height << "\n";
        } else {
            std::cerr << "[chain] failed to connect block\n";
        }
    });

    std::cerr << "[dbg] starting network\n";
    if (!network.start()) {
        std::cerr << "[node] network start failed\n";
        return 1;
    }
    std::cerr << "[dbg] network started\n";
    std::cout << "[node] P2P listening on port " << cfg.p2p_port << "\n";

    // Start upload worker thread
    std::cerr << "[dbg] starting upload worker\n";
    candidates.start(chain, db, network, cfg, keypair);

    (void)exe_path;  // bootstrap file no longer shipped — kept for ABI

    // HTTP gossip mesh has been retired — peer discovery now happens via the
    // VPS mini-node's librats `routes.get` RPC. Block sync between full
    // nodes still uses the legacy TCP mesh in NetworkManager.

    // HTTP API server — kept for diagnostics and legacy clients. The new
    // librats RPC layer (RatsApi) reuses these same verb handlers, so we
    // create it first and hand a reference to RatsApi.
    mc::api::HttpServer api(chain, candidates, network, db, cfg, keypair);

    // The standalone HTTP/3 server on cfg.api_port is gone — every verb in
    // HttpServer is now reachable over the same librats QUIC RPC channel
    // (see RatsApi below) on UDP/443. Players, full nodes and the mini-node
    // all share that one socket; no second port to forward.

    // QUIC NAT punchthrough — uses 85.239.238.226 as the STUN + rendezvous
    // so this node can be reached by other nodes and phone clients even
    // from behind a NAT, without any UPnP support. Also publishes our
    // routing record (node_id + STUN-discovered public address + api_port)
    // to the VPS mini-node every 15 minutes via the MC_ROUTES_TOPIC topic.
    mc::net::RatsLink rats(cfg.rats_port,
                            mc::crypto::to_hex(cfg.node_id),
                            cfg.api_port);
    mc::api::RatsApi rats_api(api, chain, candidates, network, db, cfg, keypair);
    if (rats.start()) {
        std::cout << "[node] rats link active on port " << cfg.rats_port
                  << " (VPS " << mc::net::MC_VPS_HOST << ":"
                  << mc::net::MC_VPS_RATS_PORT << ")\n";
        const std::string pub = rats.public_address();
        if (!pub.empty())
            std::cout << "[node] public address (STUN): " << pub << "\n";

        // Boot the BitTorrent-compatible DHT so players that come up
        // without an active VPS link can still discover swarm peers by
        // content_hash. The dht_port=0 hint lets librats pick an
        // ephemeral UDP socket; we never have to expose it through a
        // firewall because peers reach this node via the DHT routing
        // table, not a fixed port. Failure here is non-fatal: the
        // SwarmIndex + VPS routes still mediate discovery.
        if (rats_start_dht_discovery(rats.client(), 0) != 0) {
            std::cerr << "[node] start_dht_discovery failed — DHT swarm "
                         "discovery unavailable, falling back to VPS\n";
        } else {
            std::cout << "[node] DHT swarm discovery active "
                      << "(routing table " << rats_get_dht_routing_table_size(rats.client())
                      << " entries)\n";
        }

        rats_api.start(rats.client());
    } else {
        std::cerr << "[node] rats link failed to start — continuing without NAT punch\n";
    }

    // UPnP removed. NAT traversal is now mc_rats_quic's job (QUIC peers all
    // connect outbound to the mini-node first; inbound flows tunnel via the
    // relay if reachability probing showed this node is firewalled).

    // Start the HTTP API server (it was constructed earlier so RatsApi
    // could borrow its verb handlers).
    if (!api.start()) {
        std::cerr << "[node] API server start failed\n";
        return 1;
    }
    std::cout << "[node] API listening on port " << cfg.api_port << "\n";

    // Register signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (tui_mode) {
        // Kick a background maintenance thread so the TUI's redraw loop
        // doesn't have to busy-poll candidates.cleanup_expired().
        std::thread janitor([&]{
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                candidates.cleanup_expired();
            }
        });
        mc::ui::run_tui(api, rats_api, chain, db, rats_api.swarm_index(),
                        network, keypair, cfg.data_dir, g_running);
        if (janitor.joinable()) janitor.join();
    } else {
        std::cout << "[node] running. Press Ctrl+C to stop.\n";
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            candidates.cleanup_expired();
        }
    }

    std::cout << "[node] shutting down...\n";
    rats_api.stop();
    rats.stop();
    api.stop();
    network.stop();
    return 0;
}

// ---- Subcommand: status ---------------------------------------------

static int cmd_status(const std::vector<std::string>& /*args*/) {
    // Simplified: just print "OK". Full implementation would query the running node's API.
    std::cout << "musicchain-node status: use 'curl http://localhost:9334/api/v1/status'\n";
    return 0;
}

// ---- Subcommand: peers ----------------------------------------------

static int cmd_peers(const std::vector<std::string>& /*args*/) {
    std::cout << "musicchain-node peers: use 'curl http://localhost:9334/api/v1/peers'\n";
    return 0;
}

// ---- Subcommand: verify-chain ---------------------------------------

static int cmd_verify_chain(const std::vector<std::string>& args) {
    std::string data_dir = "./data";
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == "--data-dir" && i+1 < args.size()) data_dir = args[++i];

    mc::Database db(data_dir + "/blockchain.db");
    mc::Chain chain(db);
    if (!chain.init()) { std::cerr << "chain init failed\n"; return 1; }

    std::cout << "Verifying " << chain.tip().height << " blocks...\n";
    for (uint32_t h = 1; h <= chain.tip().height; ++h) {
        auto hash  = chain.get_block_hash(h);
        if (!hash) { std::cerr << "missing block at height " << h << "\n"; return 1; }
        auto block = chain.get_block(*hash);
        if (!block) { std::cerr << "cannot load block at height " << h << "\n"; return 1; }
        if (!block->validate()) { std::cerr << "invalid block at height " << h << "\n"; return 1; }
        if (h % 1000 == 0) std::cout << "  verified " << h << " / " << chain.tip().height << "\n";
    }
    std::cout << "Chain verification OK.\n";
    return 0;
}

// ---- Subcommand: rebuild-index --------------------------------------

static int cmd_rebuild_index(const std::vector<std::string>& args) {
    std::string data_dir = "./data";
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == "--data-dir" && i+1 < args.size()) data_dir = args[++i];

    mc::Database db(data_dir + "/blockchain.db");
    mc::Chain chain(db);
    if (!chain.init()) { std::cerr << "chain init failed\n"; return 1; }
    std::cout << "Rebuilding derived state from " << chain.tip().height << " blocks...\n";
    if (!chain.rebuild_derived_state()) { std::cerr << "rebuild failed\n"; return 1; }
    std::cout << "Done.\n";
    return 0;
}

// ---- main -----------------------------------------------------------

int main(int argc, char** argv) {
    // When the node runs under a service manager (or `Start-Process
    // -RedirectStandardOutput`), stdout is fully-buffered and diagnostic
    // [rats-api] / [chain] traces only surface after the buffer fills.
    // Disable that so logs hit disk as they're written.
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    if (argc < 2) {
        std::cerr << "Usage: musicchain-node <command> [options]\n"
                  << "Commands: start, status, peers, sync-status, stop,\n"
                  << "          rebuild-index, verify-chain\n";
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);

    if (command == "start")         return cmd_start(args, argv[0]);
    if (command == "status")        return cmd_status(args);
    if (command == "peers")         return cmd_peers(args);
    if (command == "sync-status")   return cmd_status(args);
    if (command == "verify-chain")  return cmd_verify_chain(args);
    if (command == "rebuild-index") return cmd_rebuild_index(args);
    if (command == "stop") {
        std::cout << "Send SIGTERM to the running node process.\n";
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    return 1;
}
