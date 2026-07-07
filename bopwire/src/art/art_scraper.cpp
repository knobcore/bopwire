#include "art_scraper.h"

#include "album_key.h"
#include "sacad_ffi.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

namespace mc::art {

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// arts:<key> tombstone = 1 status byte ('M' miss | 'E' transient error) + 8-byte
// LE timestamp of the attempt. Presence of art:<key> is the "have" signal, so
// tombstones only ever record failures.
std::vector<uint8_t> status_row(char st, uint64_t ts) {
    std::vector<uint8_t> v;
    v.reserve(9);
    v.push_back(static_cast<uint8_t>(st));
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((ts >> (8 * i)) & 0xff));
    return v;
}

// True if a prior miss/error tombstone is still within its retry TTL.
bool negative_cached(const std::vector<uint8_t>& row, uint64_t now) {
    if (row.size() < 9) return false;
    const char st = static_cast<char>(row[0]);
    uint64_t ts = 0;
    for (int i = 0; i < 8; ++i) ts |= static_cast<uint64_t>(row[1 + i]) << (8 * i);
    const uint64_t ttl = (st == 'M') ? ArtScraper::kMissRetryMs : ArtScraper::kErrRetryMs;
    return now >= ts && (now - ts) < ttl;
}

}  // namespace

ArtScraper::ArtScraper(Database& db) : db_(db) {}

ArtScraper::~ArtScraper() { stop(); }

void ArtScraper::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { loop(); });
}

void ArtScraper::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void ArtScraper::loop() {
    while (running_) {
        // Do a pass first (so covers start filling at startup, not one interval
        // later), then sleep in 1-s increments so stop() unblocks fast.
        try {
            scan_once();
        } catch (const std::exception& e) {
            std::cerr << "[art] scan failed: " << e.what() << "\n";
        }
        for (uint32_t elapsed = 0; running_ && elapsed < kScanIntervalMs; elapsed += 1000)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void ArtScraper::scan_once() {
    // 1) Reduce the on-chain song set to distinct normalized album keys, keeping
    //    a representative raw (artist, album) to hand to sacad.
    struct AlbumRef {
        std::string artist, album;
    };
    std::unordered_map<std::string, AlbumRef> albums;  // key hex -> raw
    for (const auto& ch : db_.get_all_song_hashes()) {
        if (!running_) return;
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        if (meta->artist.empty()) continue;  // need an artist to search meaningfully
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album(meta->album)) continue;
        std::string key = album_key_hex(meta->artist, meta->album);
        albums.emplace(std::move(key), AlbumRef{meta->artist, meta->album});
    }

    const uint64_t now = now_ms();
    uint32_t attempts = 0;
    for (auto& [key, ref] : albums) {
        if (!running_) return;
        if (attempts >= kMaxFetchesPerScan) break;

        const std::string blob_key = "art:" + key;
        if (db_.get(blob_key)) continue;  // already have this cover
        const std::string status_key = "arts:" + key;
        if (auto st = db_.get(status_key); st && negative_cached(*st, now)) continue;

        // 2) In-process sacad fetch (throttled, concurrency 1).
        uint8_t* buf = nullptr;
        size_t len = 0;
        const int rc =
            sacad_fetch_cover(ref.artist.c_str(), ref.album.c_str(), kTargetSizePx, &buf, &len);
        if (rc == 0 && buf != nullptr && len > 0) {
            std::vector<uint8_t> jpeg(buf, buf + len);
            sacad_free(buf, len);
            db_.put(blob_key, jpeg);
            db_.del(status_key);  // clear any stale miss tombstone
            std::cout << "[art] cover " << ref.artist << " \xe2\x80\x94 " << ref.album << " ("
                      << len << " bytes)\n";
        } else {
            if (buf != nullptr) sacad_free(buf, len);
            // rc==1 -> genuine not-found (long TTL); anything else -> transient (short TTL).
            db_.put(status_key, status_row(rc == 1 ? 'M' : 'E', now));
        }
        ++attempts;

        // Throttle between network fetches, but stay responsive to stop().
        for (uint32_t e = 0; running_ && e < kFetchDelayMs; e += 250)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (attempts > 0)
        std::cout << "[art] scan complete: " << attempts << " fetch attempt(s), "
                  << albums.size() << " distinct album(s)\n";
}

}  // namespace mc::art
