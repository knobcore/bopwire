#include "accumulator.h"
#include "database.h"

namespace mc {

// leaf(k,v) = expand(SHA256(u32le(k.size)||k||u32le(v.size)||v)) into 1024 u16
// lanes. expand(seed) = 64 SHA256(seed||u32le(i)) blocks (64*32 = 2048 bytes =
// 1024 little-endian u16). Integer-only, no float/locale -> bit-identical on
// every platform.
std::array<uint16_t, 1024> StateAccumulator::leaf(const std::string& k,
                                                  const std::vector<uint8_t>& v) {
    std::vector<uint8_t> pre;
    auto put_u32 = [&](uint32_t n) { for (int i = 0; i < 4; ++i) pre.push_back((n >> (8*i)) & 0xff); };
    put_u32(static_cast<uint32_t>(k.size()));
    pre.insert(pre.end(), k.begin(), k.end());
    put_u32(static_cast<uint32_t>(v.size()));
    pre.insert(pre.end(), v.begin(), v.end());
    Hash256 seed = crypto::sha256(pre.data(), pre.size());

    std::array<uint16_t, 1024> out{};
    for (uint32_t i = 0; i < 64; ++i) {
        std::vector<uint8_t> blk(seed.begin(), seed.end());
        for (int j = 0; j < 4; ++j) blk.push_back((i >> (8*j)) & 0xff);
        Hash256 h = crypto::sha256(blk.data(), blk.size());   // 32 bytes = 16 u16
        for (int j = 0; j < 16; ++j)
            out[i*16 + j] = static_cast<uint16_t>(h[2*j]) |
                            (static_cast<uint16_t>(h[2*j + 1]) << 8);
    }
    return out;
}

void StateAccumulator::on_put(const std::string& k, const std::vector<uint8_t>& v) {
    std::optional<std::vector<uint8_t>> old;
    auto it = overlay.find(k);
    if (it != overlay.end())      old = it->second;
    else if (db)                  old = db->get(k);
    const auto add = leaf(k, v);
    for (int i = 0; i < 1024; ++i) vec[i] = static_cast<uint16_t>(vec[i] + add[i]);
    if (old) {
        const auto sub = leaf(k, *old);
        for (int i = 0; i < 1024; ++i) vec[i] = static_cast<uint16_t>(vec[i] - sub[i]);
    }
    overlay[k] = v;
}

void StateAccumulator::on_del(const std::string& k) {
    std::optional<std::vector<uint8_t>> old;
    auto it = overlay.find(k);
    if (it != overlay.end())      old = it->second;
    else if (db)                  old = db->get(k);
    if (old) {
        const auto sub = leaf(k, *old);
        for (int i = 0; i < 1024; ++i) vec[i] = static_cast<uint16_t>(vec[i] - sub[i]);
    }
    overlay[k] = std::nullopt;
}

Hash256 StateAccumulator::root() const {
    return crypto::sha256(serialize_vec(vec).data(), 2048);
}

std::vector<uint8_t> StateAccumulator::serialize_vec(const std::array<uint16_t, 1024>& v) {
    std::vector<uint8_t> b(2048);
    for (int i = 0; i < 1024; ++i) { b[2*i] = v[i] & 0xff; b[2*i + 1] = (v[i] >> 8) & 0xff; }
    return b;
}

std::array<uint16_t, 1024> StateAccumulator::deserialize_vec(const std::vector<uint8_t>& b) {
    std::array<uint16_t, 1024> v{};
    if (b.size() >= 2048)
        for (int i = 0; i < 1024; ++i)
            v[i] = static_cast<uint16_t>(b[2*i]) | (static_cast<uint16_t>(b[2*i + 1]) << 8);
    return v;
}

} // namespace mc
