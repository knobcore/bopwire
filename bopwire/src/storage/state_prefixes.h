#pragma once
#include <string>
#include <cstddef>

namespace mc {

// The EXACT multiset of key-prefixes the committed state_root commits to.
// This ONE list must equal the accumulator hook set == clear_derived_state's
// state set == the snapshot dumper's set, so they can never drift (P4). "v:" is
// on-chain-only now (the node-local self-register was removed), so it is safe in
// a shared root. "c:total_supply" is a single STATE key, not a prefix (handled
// explicitly below). "fph:" is deliberately NOT here — it is cleared on rebuild
// but excluded from the root (derivable from "f:").
inline constexpr const char* kStatePrefixes[] = {
    "a:", "s:", "f:", "i:", "u:", "us:", "va:", "v:", "bh:",
    "nv:", "sm:", "ia:", "ig:",
    "founder:", "mlvl:", "mpub:", "mact:", "slashed:",
    "un:", "addrun:",
    "prop:", "propstatus:", "propvote:",
    // Record-label state. Written ONLY by the consensus apply path
    // (Chain::apply_moderator_op TAG_LABEL_EDIT -> set_label / assign_artist_label,
    // chain.cpp:586/597) and load-bearing for consensus: execute_proposal
    // RELEASE_ESCROW routes escrow into a: balances (which ARE rooted) via
    // get_label / get_artist_label, so these MUST be in the root and cleared by
    // rebuild — otherwise a reorg leaves stale label rows and two honest nodes
    // credit escrow differently. ("art_label:" does not start with "label:", so
    // both are listed explicitly.) NOTE: "d:" (song-hidden) is deliberately NOT
    // here — it is written by non-consensus paths (API/DMCA/deep-audit) and read
    // only for local display/serving, never in an apply/state_root path, so
    // rooting it would fork on every local hide.
    "label:", "art_label:"
};

// True iff `key` is part of the committed state: it matches one of
// kStatePrefixes, or is the single "c:total_supply" key. Used identically by the
// accumulator hook, clear_derived_state, and the snapshot dumper.
inline bool has_state_prefix(const std::string& key) {
    if (key == "c:total_supply") return true;
    for (const char* p : kStatePrefixes) {
        const std::size_t n = std::char_traits<char>::length(p);
        if (key.size() >= n && key.compare(0, n, p) == 0) return true;
    }
    return false;
}

} // namespace mc
