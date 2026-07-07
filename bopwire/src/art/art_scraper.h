#pragma once
//
// ArtScraper — background job that fills real album-cover JPEGs into DB2 (kept in
// the main Database under the art:/arts: prefixes, mirroring RelayCreditTracker's
// rc: keyspace). Once running it periodically scans the on-chain song set,
// reduces it to distinct normalized album keys, and for each album still missing
// art calls the vendored sacad static library (in-process, rustls, no subprocess)
// to fetch a ~600px cover — storing the JPEG at art:<key>. Misses/errors are
// negative-cached at arts:<key> with a retry TTL so they aren't re-scraped every
// pass. Throttled (concurrency 1, small inter-fetch delay) so the upstream
// sources (Deezer/iTunes/Discogs/Last.fm/CoverArtArchive) aren't hammered.
//
// Threading mirrors CollectionCurator / RelayCreditTracker: atomic running_, one
// worker, 1-s-increment sleeps so stop() unblocks fast. Pure reader of the chain;
// only writes its own art:/arts: keyspace.
//
// Compiled + linked into bopwire-node ONLY when MC_WITH_ART=ON (the sacad FFI is
// a node-only static lib — the Flutter players' libbopwire stays Rust-free).
//
#include "../storage/database.h"

#include <atomic>
#include <thread>

namespace mc::art {

class ArtScraper {
public:
    static constexpr uint32_t kScanIntervalMs    = 5 * 60 * 1000;       // rescan albums / 5 min
    static constexpr uint32_t kFetchDelayMs      = 1500;                // throttle between fetches
    static constexpr uint32_t kTargetSizePx      = 600;                 // sacad resizes to this
    static constexpr uint32_t kMaxFetchesPerScan = 40;                  // bound work per pass
    static constexpr uint64_t kMissRetryMs       = 7ull * 24 * 3600 * 1000;  // real miss: retry / 7d
    static constexpr uint64_t kErrRetryMs        = 60ull * 60 * 1000;   // transient error: retry / 1h

    explicit ArtScraper(Database& db);
    ~ArtScraper();

    void start();
    void stop();

private:
    void loop();
    void scan_once();

    Database&         db_;
    std::thread       worker_;
    std::atomic<bool> running_{false};
};

}  // namespace mc::art
