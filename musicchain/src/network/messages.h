#pragma once
#include "../core/block.h"
#include <cstdint>
#include <vector>

namespace mc::net {

enum class MsgType : uint8_t {
    BLOCK_CANDIDATE   = 0x01,
    CONFIRMATION      = 0x02,
    BLOCK             = 0x03,
    GET_BLOCK         = 0x04,
    GET_BLOCKS        = 0x05,
    GET_HEADERS       = 0x06,
    HEADERS           = 0x07,
    TX                = 0x08,
    GET_TX            = 0x09,
    INV               = 0x0A,
    VALIDATOR_ANNOUNCE= 0x0B,
    GET_VALIDATORS    = 0x0C,
    VALIDATORS        = 0x0D,
    PING              = 0x0E,
    PONG              = 0x0F,
    VERSION           = 0x10,
    CHECKSUM_REQ      = 0x11,  // payload: 4-byte LE block height
    CHECKSUM_RESP     = 0x12,  // payload: 4-byte LE height + 32-byte full_hash
    // DHT peer discovery (IPv6)
    NODE_ANNOUNCE     = 0x13,  // 16-byte IPv6 + 2-byte p2p_port LE + 2-byte api_port LE + 32-byte node_id
    GET_PEERS         = 0x14,  // empty — request peer list
    PEERS             = 0x15,  // N * 52-byte DhtRecord (same layout as NODE_ANNOUNCE payload)
};

struct Message {
    MsgType              type;
    std::vector<uint8_t> payload;

    // Serialize to wire format: 4-byte length (LE) + 1-byte type + payload
    std::vector<uint8_t> serialize() const;

    // Parse from raw bytes; returns false on truncated or malformed data
    static bool parse(const uint8_t* data, size_t len, Message& out, size_t& consumed);
};

// Convenience constructors
Message make_ping();
Message make_pong();
Message make_get_block(const Hash256& hash);
Message make_tx(const std::vector<uint8_t>& tx_raw);
Message make_block(const Block& block);
Message make_block_candidate(const Block& candidate);
Message make_confirmation(const std::string& candidate_hash, const Confirmation& conf);
Message make_checksum_req(uint32_t height);
Message make_checksum_resp(uint32_t height, const Hash256& full_hash);

// DHT messages
// ipv6: 16-byte raw address; p2p_port/api_port: host byte order; node_id: 32-byte hash
Message make_node_announce(const std::array<uint8_t,16>& ipv6,
                            uint16_t p2p_port, uint16_t api_port,
                            const Hash256& node_id);
Message make_get_peers();
// peers: vector of (ipv6[16], p2p_port, api_port, node_id[32])
struct DhtRecord {
    std::array<uint8_t,16> ipv6 = {};
    uint16_t p2p_port = 0;
    uint16_t api_port = 0;
    Hash256  node_id  = {};
};
Message make_peers(const std::vector<DhtRecord>& peers);
bool parse_dht_record(const uint8_t* data, size_t len, size_t offset, DhtRecord& out);

static constexpr size_t MAX_MESSAGE_SIZE = 60 * 1024 * 1024; // 60 MiB

} // namespace mc::net
