#include "messages.h"
#include "../crypto/hash.h"

namespace mc::net {

std::vector<uint8_t> Message::serialize() const {
    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    uint32_t total_len   = payload_len + 1; // +1 for type byte
    std::vector<uint8_t> out;
    out.reserve(5 + payload.size());
    for (int i = 0; i < 4; ++i) out.push_back((total_len >> (8*i)) & 0xFF);
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool Message::parse(const uint8_t* data, size_t len, Message& out, size_t& consumed) {
    if (len < 5) return false;
    uint32_t msg_len = 0;
    for (int i = 0; i < 4; ++i) msg_len |= (static_cast<uint32_t>(data[i]) << (8*i));
    if (msg_len == 0 || msg_len > MAX_MESSAGE_SIZE) return false;
    if (len < size_t(4 + msg_len)) return false;
    out.type = static_cast<MsgType>(data[4]);
    out.payload.assign(data + 5, data + 4 + msg_len);
    consumed = 4 + msg_len;
    return true;
}

Message make_ping() { return {MsgType::PING, {}}; }
Message make_pong() { return {MsgType::PONG, {}}; }

Message make_get_block(const Hash256& hash) {
    return {MsgType::GET_BLOCK, std::vector<uint8_t>(hash.begin(), hash.end())};
}

Message make_tx(const std::vector<uint8_t>& tx_raw) {
    return {MsgType::TX, tx_raw};
}

Message make_block(const Block& block) {
    return {MsgType::BLOCK, block.serialize()};
}

Message make_block_candidate(const Block& candidate) {
    return {MsgType::BLOCK_CANDIDATE, candidate.serialize()};
}

Message make_checksum_req(uint32_t height) {
    std::vector<uint8_t> payload(4);
    for (int i = 0; i < 4; ++i) payload[i] = (height >> (8*i)) & 0xFF;
    return {MsgType::CHECKSUM_REQ, std::move(payload)};
}

Message make_checksum_resp(uint32_t height, const Hash256& full_hash) {
    std::vector<uint8_t> payload(36);
    for (int i = 0; i < 4; ++i) payload[i] = (height >> (8*i)) & 0xFF;
    std::copy(full_hash.begin(), full_hash.end(), payload.begin() + 4);
    return {MsgType::CHECKSUM_RESP, std::move(payload)};
}

Message make_confirmation(const std::string& candidate_hash, const Confirmation& conf) {
    std::vector<uint8_t> payload;
    auto hash_bytes = mc::crypto::from_hex(candidate_hash);
    payload.insert(payload.end(), hash_bytes.begin(), hash_bytes.end());
    payload.insert(payload.end(), conf.validator_id.begin(), conf.validator_id.end());
    payload.insert(payload.end(), conf.pubkey.begin(), conf.pubkey.end());
    payload.insert(payload.end(), conf.signature.begin(), conf.signature.end());
    return {MsgType::CONFIRMATION, payload};
}

// ---- DHT messages ---------------------------------------------------

static void encode_dht_record(std::vector<uint8_t>& out, const DhtRecord& r) {
    out.insert(out.end(), r.ipv6.begin(), r.ipv6.end());          // 16 bytes
    out.push_back(r.p2p_port & 0xFF);                              // 2 bytes LE
    out.push_back((r.p2p_port >> 8) & 0xFF);
    out.push_back(r.api_port & 0xFF);                              // 2 bytes LE
    out.push_back((r.api_port >> 8) & 0xFF);
    out.insert(out.end(), r.node_id.begin(), r.node_id.end());    // 32 bytes
}

bool parse_dht_record(const uint8_t* data, size_t len, size_t offset, DhtRecord& out) {
    if (offset + 52 > len) return false;
    const uint8_t* p = data + offset;
    std::copy(p, p + 16, out.ipv6.begin());
    out.p2p_port = static_cast<uint16_t>(p[16]) | (static_cast<uint16_t>(p[17]) << 8);
    out.api_port = static_cast<uint16_t>(p[18]) | (static_cast<uint16_t>(p[19]) << 8);
    std::copy(p + 20, p + 52, out.node_id.begin());
    return true;
}

Message make_node_announce(const std::array<uint8_t,16>& ipv6,
                            uint16_t p2p_port, uint16_t api_port,
                            const Hash256& node_id) {
    DhtRecord r;
    r.ipv6 = ipv6;
    r.p2p_port = p2p_port;
    r.api_port = api_port;
    r.node_id  = node_id;
    std::vector<uint8_t> payload;
    payload.reserve(52);
    encode_dht_record(payload, r);
    return {MsgType::NODE_ANNOUNCE, std::move(payload)};
}

Message make_get_peers() {
    return {MsgType::GET_PEERS, {}};
}

Message make_peers(const std::vector<DhtRecord>& peers) {
    std::vector<uint8_t> payload;
    payload.reserve(peers.size() * 52);
    for (const auto& r : peers) encode_dht_record(payload, r);
    return {MsgType::PEERS, std::move(payload)};
}

} // namespace mc::net
