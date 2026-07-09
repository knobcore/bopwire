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
    "prop:", "propstatus:", "propvote:"
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
