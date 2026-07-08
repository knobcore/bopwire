#pragma once
//
// First-launch network self-check for a full node:
//   1. Try to open the P2P + API ports via UPnP (UpnpMapper / miniupnpc).
//   2. Test inbound reachability — the node can accept inbound connections
//      if EITHER the STUN-observed public port matches the listen port
//      (no NAT remap) OR UPnP successfully mapped the port.
//   3. If reachable, run an external HTTPS up/down speed test and report a
//      usable capacity = min(up,down) - 10 Mbit reserve, which the caller
//      writes into the config's load_monitor.max_bandwidth_bps.
//
// The caller REFUSES to complete setup (exits) when the node is not
// reachable — a node that can't accept inbound is useless to the mesh.
//
#include <cstdint>
#include <functional>
#include <string>

namespace mc::net {

struct NetCheckResult {
    bool     reachable         = false;  // can accept inbound (STUN match OR UPnP mapped)
    bool     upnp_mapped       = false;  // UPnP IGD found + at least one mapping succeeded
    bool     stun_port_match   = false;  // STUN public port == listen port
    uint64_t max_bandwidth_bps = 0;      // measured min(up,down) - reserve; 0 if not measured
    bool     measured          = false;  // speed test succeeded
};

// Runs the check. `public_addr()` returns the STUN-observed "ip:port" (may be
// empty until observed — the check polls it for a while). Blocks up to ~50s.
NetCheckResult run_net_check(uint16_t rats_port, uint16_t api_port,
                             std::function<std::string()> public_addr);

} // namespace mc::net
