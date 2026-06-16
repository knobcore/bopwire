#include "validator.h"
#include "../core/block.h"
#include "../crypto/hash.h"
#include <cstring>
#include <chrono>

namespace mc {

static uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---- ValidatorInfo serialization ------------------------------------

std::vector<uint8_t> ValidatorInfo::serialize() const {
    std::vector<uint8_t> buf;
    write_bytes(buf, node_id.data(), 32);
    write_bytes(buf, public_key.data(), 33);
    write_string16(buf, endpoint);
    write_u64le(buf, last_seen);
    write_u64le(buf, blocks_validated);
    // reliability_score as float (4 bytes)
    uint32_t score_int;
    std::memcpy(&score_int, &reliability_score, sizeof(float));
    write_u32le(buf, score_int);
    return buf;
}

bool ValidatorInfo::deserialize(const uint8_t* data, size_t len, ValidatorInfo& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (!read_bytes(p, end, out.node_id.data(), 32))  return false;
    if (!read_bytes(p, end, out.public_key.data(), 33)) return false;
    if (!read_string16(p, end, out.endpoint)) return false;
    if (!read_u64le(p, end, out.last_seen)) return false;
    if (!read_u64le(p, end, out.blocks_validated)) return false;
    uint32_t score_int = 0;
    if (!read_u32le(p, end, score_int)) return false;
    std::memcpy(&out.reliability_score, &score_int, sizeof(float));
    return true;
}

// ---- ValidatorRegistry ----------------------------------------------

ValidatorRegistry::ValidatorRegistry(Database& db) : db_(db) {}

bool ValidatorRegistry::register_validator(const ValidatorInfo& info) {
    auto data = info.serialize();
    return db_.put("v:" + db_.hex(info.node_id), data);
}

std::optional<ValidatorInfo> ValidatorRegistry::get_validator(const Hash256& node_id) const {
    auto v = db_.get("v:" + db_.hex(node_id));
    if (!v) return std::nullopt;
    ValidatorInfo info;
    if (!ValidatorInfo::deserialize(v->data(), v->size(), info)) return std::nullopt;
    return info;
}

std::vector<ValidatorInfo> ValidatorRegistry::get_active_validators() const {
    std::vector<ValidatorInfo> result;
    uint64_t threshold = now_ms() - ACTIVE_TIMEOUT_MS;
    // Simplified: full iteration requires a public scan method on Database
    // For now return empty
    return result;
}

size_t ValidatorRegistry::active_count() const {
    return get_active_validators().size();
}

void ValidatorRegistry::record_result(const Hash256& node_id, bool success) {
    auto info = get_validator(node_id);
    if (!info) return;
    if (success) info->blocks_validated++;
    info->reliability_score = 0.95f * info->reliability_score + 0.05f * (success ? 1.0f : 0.0f);
    info->last_seen = now_ms();
    register_validator(*info);
}

void ValidatorRegistry::touch(const Hash256& node_id) {
    auto info = get_validator(node_id);
    if (!info) return;
    info->last_seen = now_ms();
    register_validator(*info);
}

} // namespace mc
