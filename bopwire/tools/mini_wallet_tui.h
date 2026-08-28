#pragma once
//
// mini_wallet_tui — the ENTIRE interactive surface of bopwire-mini-node.
//
// The mini-node is a relay/rendezvous daemon; its only operator-facing screen
// is a password-protected wallet. There is deliberately NOTHING else here:
// no peers/routes/players panels, no log tail, no chain status, no bootstrap
// surface, and (by design and by policy) NO moderator functionality of any
// kind. If you are tempted to add a panel to this file, it belongs in the
// full node's TUI (tools/monitor_tui.cpp) or the moderator console instead.
//
// Screens:
//   LOCKED    password prompt (masked); nothing is shown until it verifies
//   WALLET    address + balance, and the action keys
//   RECEIVE   the address on its own, to read off / copy
//   SEND      recipient -> amount -> confirm -> re-enter password -> submit
//
// This module is chain-free: it only talks to the caller through
// std::function callbacks, exactly like monitor_tui does, so the mini-node
// (which does not link the chain/database engine) can drive it from its
// librats globals. It never logs, prints, or persists a password, a mnemonic,
// or any other secret — while it is running std::cout/std::cerr are diverted
// to a discard sink so background librats/relay chatter cannot scribble over
// the screen (and so nothing the operator types can leak into a log).
//
#include <atomic>
#include <functional>
#include <string>

namespace mc::ui {

struct WalletTuiState {
    std::string title = "bopwire mini-node wallet";

    // EIP-55 checksummed address of the unlocked wallet (display + receive).
    std::string address;

    // Verify an operator-typed password against the wallet keystore on disk.
    // Used to unlock the screen and to re-authorize each send. Required; if
    // unset the screen starts unlocked and sends are not password-gated.
    std::function<bool(const std::string& password)> verify_password;

    // Spendable balance as a display string (e.g. "12.34000000"). Return an
    // empty string when it could not be fetched (no full node reachable).
    std::function<std::string()> balance;

    // Submit a signed transfer. `amount` is the decimal string the operator
    // typed. Returns true on accepted-by-a-full-node; fills `msg` either way
    // with a short human-readable result (tx hash / rejection reason).
    std::function<bool(const std::string& to_address,
                       const std::string& amount,
                       std::string&       msg)> send;

    // Seconds of no keypresses before the screen re-locks itself. 0 disables.
    int auto_lock_seconds = 300;
};

// Run the wallet screen on the caller's thread. Blocks until Q / Ctrl-C or
// `running` flips false. The callbacks are polled on redraw, so whatever they
// capture must outlive the call.
void run_wallet_tui(const WalletTuiState& st, std::atomic<bool>& running);

} // namespace mc::ui
