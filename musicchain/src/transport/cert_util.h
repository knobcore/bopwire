// Generate self-signed ECDSA P-256 cert + key as PEM files in the system
// temp dir. Shared by mc_rats_quic (peer transport) and the HTTP/3 server
// (api). Caller is responsible for deleting the files after the server has
// ingested them.

#pragma once
#include <string>

namespace mc::transport {

struct CertFiles {
    std::string cert_path;
    std::string key_path;
    bool        ok = false;
};

/// Synthesize a fresh self-signed cert + key, write them to temp PEM files,
/// return both paths. The files are tagged with the current process id so
/// multiple mc-rats / h3 instances on the same box don't clash.
CertFiles make_self_signed_files();

} // namespace mc::transport
