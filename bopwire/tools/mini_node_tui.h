#pragma once
//
// mini_node_tui — an interactive curses TUI for the mini-node, modelled on the
// full node's node_tui.cpp (curses, F1 status, a footer of actions). Its reason
// to exist is to host wallet Export / Import of the mini-node's seed without a
// full node. node_tui.cpp can't be reused (it is bound to Chain/Database/etc.,
// none of which the mini-node has), so this is a small standalone TUI fed by
// accessors into the mini-node's globals.
//
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace mc::mini {

struct MiniTuiState {
    std::function<int()>         peer_count;      // rats peer count
    std::function<std::size_t()> route_count;     // known full-node routes
    std::function<std::size_t()> player_count;    // connected players
    std::function<std::string()> wallet_address;  // EIP-55 mini identity
    std::string                  seed_path;        // path to mini-node.seed
    int                          rats_port = 0;
};

// Runs the curses TUI on the calling (main) thread until `running` goes false or
// the operator quits (Q). Export/Import use the shared native keystore on the
// mini-node's seed file. NOTE: an import changes the wallet address = the librats
// peer id, so the operator must RESTART the mini-node for it to take effect.
void run_mini_tui(const MiniTuiState& st, std::atomic<bool>& running);

}  // namespace mc::mini
