#pragma once
#include <cstdint>
#include "../core/block.h"
#include "../storage/database.h"
#include <chrono>
#include <string>

namespace mc {

struct ValidatorInfo {
    Hash256     node_id;
    PubKey33    public_key;
    std::string endpoint;   // "host:port"
    uint64_t    last_seen;  // ms since epoch
    uint64_t    blocks_validated;
    float       reliability_score;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, ValidatorInfo& out);
};

class ValidatorRegistry {
public:
    explicit ValidatorRegistry(Database& db);

    // Register or update a validator
    bool register_validator(const ValidatorInfo& info);

    // Get validator info
    std::optional<ValidatorInfo> get_validator(const Hash256& node_id) const;

    // Get all active validators (last_seen within 60 seconds)
    std::vector<ValidatorInfo> get_active_validators() const;

    // Count active validators
    size_t active_count() const;

    // Update reliability score on confirmation result
    void record_result(const Hash256& node_id, bool success);

    // Update last_seen timestamp
    void touch(const Hash256& node_id);

    static constexpr size_t MIN_VALIDATORS = 5;
    static constexpr uint64_t ACTIVE_TIMEOUT_MS = 60000;

private:
    Database& db_;
};

} // namespace mc
