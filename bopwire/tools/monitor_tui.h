#pragma once
//
// Shared, read-only monitor TUI for the PUBLIC binaries — the full node
// (bopwire-node) and the mini-node (bopwire-mini-node) render the exact
// same interface through this one module. It is deliberately chain-free:
// it only touches std::function callbacks + curses, so the mini-node (which
// does NOT link the full chain/database engine) can use it too. The full
// node feeds it via a thin adapter (node_tui_adapter.cpp) that reads
// Chain/Database/etc.; the mini feeds it from its librats globals.
//
// There is intentionally NO wallet/founder/moderator/bootstrap surface
// here — that all lives in the private bopwire-admin TUI. The only wallet
// affordance is optional export/import of the node's OWN identity seed
// (a BIP39 mnemonic on disk), shown only when `seed_path` is set.
//
// F1 status · F2 logs · X export · P import · Q quit.
//
#include <atomic>
#include <functional>
#include <string>

namespace mc::ui {

// Every field is a display-string callback (empty return => the row is
// hidden) or a small scalar. This keeps the monitor totally agnostic to
// how the caller formats balances / counts, and lets "not applicable on
// this node kind" simply render as an absent row.
struct MonitorState {
    std::string title = "bopwire";     // header bar label

    // Optional export/import target: the node's own identity mnemonic on
    // disk. Empty => the X/P chips are hidden (e.g. the full node, whose
    // node.key is a raw key, not a portable mnemonic). NEVER point this at
    // founder.seed — founder wallet portability is an admin-only concern.
    std::string seed_path;

    // Status rows (F1). Each returns a display string; empty => row hidden.
    std::function<std::string()> wallet_address;
    std::function<std::string()> balance;
    std::function<std::string()> escrow;
    std::function<std::string()> chain_height;
    std::function<std::string()> songs;
    std::function<std::string()> peers;
    std::function<std::string()> routes;
    std::function<std::string()> players;
    std::function<std::string()> rats_port;
};

// Run the monitor on the caller's thread. Blocks until Q / Ctrl-C or
// `running` flips false. The callbacks in `st` are polled on each redraw,
// so the objects they capture must outlive the call.
void run_monitor_tui(const MonitorState& st, std::atomic<bool>& running);

// Divert std::cout / std::cerr into the in-memory ring that the F2 logs
// page tails. Call before run_monitor_tui so startup logs are captured.
void monitor_start_log_capture();
void monitor_stop_log_capture();

} // namespace mc::ui
