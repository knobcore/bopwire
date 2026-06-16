// Legacy TCP P2P mesh removed in Phase 2c. The class is kept as a thin
// shim so existing call sites in consensus/, api/, and tools/ keep
// compiling without rewrite. Everything now flows over mc_rats_quic.

#include "manager.h"

namespace mc::net {

std::string DhtEntry::ipv6_str() const { return {}; }
std::string DhtEntry::api_url()  const { return {}; }

NetworkManager::NetworkManager(Chain& chain, CandidateManager& candidates,
                                const NodeConfig& config,
                                const crypto::KeyPair& keypair)
    : chain_(chain), candidates_(candidates), config_(config), keypair_(keypair) {}

NetworkManager::~NetworkManager() = default;

bool NetworkManager::start() { return true; }
void NetworkManager::stop()  {}

uint32_t NetworkManager::chain_height() const { return chain_.tip().height; }

} // namespace mc::net
