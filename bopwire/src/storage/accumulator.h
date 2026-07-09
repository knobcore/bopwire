#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "../crypto/hash.h"   // Hash256, crypto::sha256

namespace mc {

class Database;

// Additive lattice hash (LtHash) over the committed state (P4). State is a
// multiset of (key,value) leaves; the accumulator is the elementwise sum
// (mod 2^16) of 1024-lane leaf vectors. state_root = SHA256(accumulator bytes).
// Order-INDEPENDENT (add commutes -> root depends only on the final key/value
// set, never on iteration/apply order) and HOMOMORPHIC (subtract old leaf on
// update/delete), so it tracks the post-apply root incrementally inside the
// block batch without re-scanning or reading the unreadable WriteBatch.
struct StateAccumulator {
    std::array<uint16_t, 1024> vec{};
    // block-scoped old-value cache so repeated writes to one key telescope to
    // the committed baseline (same trick as the C2 Ledger overlay).
    std::map<std::string, std::optional<std::vector<uint8_t>>> overlay;
    Database* db = nullptr;

    void copy_from(const std::array<uint16_t, 1024>& committed) {
        vec = committed;
        overlay.clear();
    }
    void on_put(const std::string& k, const std::vector<uint8_t>& v);
    void on_del(const std::string& k);
    Hash256 root() const;

    static std::array<uint16_t, 1024> leaf(const std::string& k,
                                           const std::vector<uint8_t>& v);
    static std::vector<uint8_t> serialize_vec(const std::array<uint16_t, 1024>&);
    static std::array<uint16_t, 1024> deserialize_vec(const std::vector<uint8_t>&);
};

} // namespace mc
