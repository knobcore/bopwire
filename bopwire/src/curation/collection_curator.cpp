#include "collection_curator.h"

#include "../crypto/hash.h"
#include "../tokens/ledger.h"   // FULL_REWARD_THRESHOLD (the "under 10k plays" hook)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <unordered_map>

namespace mc::curation {

namespace {

using json = nlohmann::json;

// Normalize a facet EXACTLY like the chain's ia:/ig: index keys
// (Database::lower — plain ASCII lowercase, no trim, no locale).
std::string norm_facet(const std::string& s) {
    std::string out = s;
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Deterministic display capitalization for facet titles ("indie rock" →
// "Indie Rock"). Pure ASCII byte math — no locale, identical everywhere.
std::string title_case(const std::string& s) {
    std::string out = s;
    bool boundary = true;
    for (auto& c : out) {
        if (boundary && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        boundary = (c == ' ' || c == '-' || c == '/');
    }
    return out;
}

// Total order over hashes (byte order) — the universal sort tie-break that
// makes every collection byte-identical across nodes.
bool hash_less(const Hash256& a, const Hash256& b) {
    return std::memcmp(a.data(), b.data(), 32) < 0;
}

// One scanned song — on-chain fields ONLY. swarm_size / availability is a
// display overlay downstream and must never influence membership or order.
struct Entry {
    Hash256     ch;
    uint64_t    play_count       = 0;
    uint32_t    first_play_block = 0;
    uint16_t    year             = 0;
    std::string genre_norm;
    // carried for the embed join (display only, not part of the digest)
    std::string title, artist, album, genre;
    uint32_t    duration_ms  = 0;
    uint16_t    track_number = 0;
};

void append_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xff));
}
void append_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xff));
}
void append_str(std::vector<uint8_t>& v, const std::string& s) {
    append_u32(v, static_cast<uint32_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

// Canonical byte form: header, then collections sorted by id (byte order),
// strings length-prefixed, song hashes raw 32-byte in row order.
std::string compute_digest(const CollectionSet& set) {
    std::vector<const Collection*> ordered;
    ordered.reserve(set.collections.size());
    for (const auto& c : set.collections) ordered.push_back(&c);
    std::sort(ordered.begin(), ordered.end(),
              [](const Collection* a, const Collection* b) { return a->id < b->id; });

    std::vector<uint8_t> bytes;
    append_u32(bytes, set.snapshot_height);
    append_u64(bytes, set.epoch);
    bytes.insert(bytes.end(), set.snapshot_block_hash.begin(),
                 set.snapshot_block_hash.end());
    append_u32(bytes, static_cast<uint32_t>(ordered.size()));
    for (const auto* c : ordered) {
        append_str(bytes, c->id);
        append_str(bytes, c->kind);
        append_str(bytes, c->title);
        append_str(bytes, c->subtitle);
        append_str(bytes, c->facet);
        append_u32(bytes, static_cast<uint32_t>(c->songs.size()));
        for (const auto& h : c->songs)
            bytes.insert(bytes.end(), h.begin(), h.end());
    }
    const Hash256 d = crypto::sha256(bytes.data(), bytes.size());
    return crypto::to_hex(d);
}

json collection_to_json(const Collection& c) {
    json hashes = json::array();
    for (const auto& h : c.songs) hashes.push_back(crypto::to_hex(h));
    return json{
        {"id",          c.id},
        {"kind",        c.kind},
        {"title",       c.title},
        {"subtitle",    c.subtitle},
        {"facet",       c.facet},
        {"song_hashes", std::move(hashes)},
    };
}

} // namespace

CollectionCurator::CollectionCurator(Chain& chain, Database& db)
    : chain_(chain), db_(db) {}

CollectionCurator::~CollectionCurator() { stop(); }

void CollectionCurator::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { loop(); });
}

void CollectionCurator::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

bool CollectionCurator::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

json CollectionCurator::list_json(bool embed) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ready_) return nullptr;
    return embed ? *body_embed_ : *body_plain_;
}

json CollectionCurator::get_json(const std::string& id, bool embed) const {
    std::shared_ptr<const json> body;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ready_) return nullptr;
        body = embed ? body_embed_ : body_plain_;
    }
    for (const auto& c : (*body)["collections"]) {
        if (c.value("id", std::string()) != id) continue;
        json out{
            {"epoch",               (*body)["epoch"]},
            {"snapshot_height",     (*body)["snapshot_height"]},
            {"snapshot_block_hash", (*body)["snapshot_block_hash"]},
            {"content_digest",      (*body)["content_digest"]},
            {"collection",          c},
        };
        if (embed) {
            // Slice the songs join down to just this row's hashes.
            json songs = json::object();
            const auto& all = (*body)["songs"];
            for (const auto& h : c["song_hashes"]) {
                const std::string hex = h.get<std::string>();
                if (auto it = all.find(hex); it != all.end()) songs[hex] = *it;
            }
            out["songs"] = std::move(songs);
        }
        return out;
    }
    return nullptr;
}

void CollectionCurator::loop() {
    while (running_) {
        // Check first (so the first epoch generates at startup, not one
        // poll interval later), then sleep in 1-s increments.
        const auto tip = chain_.tip();
        const uint64_t epoch = tip.height / kEpochBlocks;
        bool need;
        {
            std::lock_guard<std::mutex> lk(mu_);
            need = !ready_ || epoch != last_epoch_;
        }
        if (need) {
            const uint32_t snapshot_height =
                static_cast<uint32_t>(epoch) * kEpochBlocks;
            try {
                regenerate(epoch, snapshot_height);
            } catch (const std::exception& e) {
                std::cerr << "[curation] regenerate failed: " << e.what() << "\n";
            }
        }
        for (uint32_t elapsed = 0;
             running_ && elapsed < kPollIntervalMs;
             elapsed += 1000) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void CollectionCurator::regenerate(uint64_t epoch, uint32_t snapshot_height) {
    // ---- one full scan (pure read, off-lock, off the RPC hot path) ----
    std::vector<Entry> entries;
    for (const auto& ch : db_.get_all_song_hashes()) {
        if (db_.is_song_deleted(ch)) continue;
        auto meta = db_.get_song_meta(ch);
        if (!meta) continue;
        // Match songs.list moderation filtering so a hidden song can't
        // resurface through Discover.
        if (db_.is_hidden_artist(meta->artist)) continue;
        if (db_.is_hidden_album (meta->album))  continue;
        if (db_.is_hidden_title (meta->title))  continue;
        const auto state = db_.get_song_state(ch);
        Entry e;
        e.ch               = ch;
        e.play_count       = state.play_count;
        e.first_play_block = state.first_play_block;
        e.year             = meta->year;
        e.genre_norm       = norm_facet(meta->genre);
        e.title            = meta->title;
        e.artist           = meta->artist;
        e.album            = meta->album;
        e.genre            = meta->genre;
        e.duration_ms      = meta->duration_ms;
        e.track_number     = meta->track_number;
        entries.push_back(std::move(e));
    }

    const auto by_plays = [](const Entry* a, const Entry* b) {
        if (a->play_count != b->play_count) return a->play_count > b->play_count;
        return hash_less(a->ch, b->ch);
    };
    const auto take = [](std::vector<const Entry*>& v) {
        if (v.size() > kMaxSongsPerCollection) v.resize(kMaxSongsPerCollection);
        std::vector<Hash256> out;
        out.reserve(v.size());
        for (const auto* e : v) out.push_back(e->ch);
        return out;
    };

    CollectionSet set;
    set.epoch           = epoch;
    set.snapshot_height = snapshot_height;
    if (auto h = chain_.get_block_hash(snapshot_height)) set.snapshot_block_hash = *h;

    // Rising — the product hook: songs still under the full-reward
    // threshold, i.e. every play mints the artist the full amount.
    {
        std::vector<const Entry*> v;
        for (const auto& e : entries)
            if (e.play_count > 0 && e.play_count < FULL_REWARD_THRESHOLD)
                v.push_back(&e);
        std::sort(v.begin(), v.end(), by_plays);
        set.collections.push_back({"rising:", "rising", "Rising",
                                   "Under 10k plays — every listen pays the artist in full",
                                   "", take(v)});
    }

    // Top 50 — all songs by play count.
    {
        std::vector<const Entry*> v;
        for (const auto& e : entries) v.push_back(&e);
        std::sort(v.begin(), v.end(), by_plays);
        set.collections.push_back({"top:", "top", "Top 50",
                                   "The most played songs on the chain",
                                   "", take(v)});
    }

    // New releases — most recent first plays. Never-played songs
    // (first_play_block == 0) sort last by construction.
    {
        std::vector<const Entry*> v;
        for (const auto& e : entries) v.push_back(&e);
        std::sort(v.begin(), v.end(), [](const Entry* a, const Entry* b) {
            if (a->first_play_block != b->first_play_block)
                return a->first_play_block > b->first_play_block;
            return hash_less(a->ch, b->ch);
        });
        set.collections.push_back({"new:", "new", "New Releases",
                                   "Fresh on the chain",
                                   "", take(v)});
    }

    // Best of {Genre} — one row per distinct genre, biggest genres first.
    // std::map keeps facet iteration in byte order (never a hash map here).
    {
        std::map<std::string, std::vector<const Entry*>> by_genre;
        for (const auto& e : entries)
            if (!e.genre_norm.empty()) by_genre[e.genre_norm].push_back(&e);
        std::vector<std::pair<std::string, std::vector<const Entry*>>> rows(
            by_genre.begin(), by_genre.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.second.size() != b.second.size())
                return a.second.size() > b.second.size();
            return a.first < b.first;
        });
        if (rows.size() > kMaxGenreRows) rows.resize(kMaxGenreRows);
        for (auto& [g, v] : rows) {
            std::sort(v.begin(), v.end(), by_plays);
            set.collections.push_back({"genre:" + g, "genre",
                                       "Best of " + title_case(g),
                                       std::to_string(v.size()) + " songs",
                                       g, take(v)});
        }
    }

    // {Year} hits — one row per distinct non-zero year, bounded by a
    // deterministic rule (biggest years first, then newest).
    {
        std::map<uint16_t, std::vector<const Entry*>> by_year;
        for (const auto& e : entries)
            if (e.year != 0) by_year[e.year].push_back(&e);
        std::vector<std::pair<uint16_t, std::vector<const Entry*>>> rows(
            by_year.begin(), by_year.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.second.size() != b.second.size())
                return a.second.size() > b.second.size();
            return a.first > b.first;
        });
        if (rows.size() > kMaxYearRows) rows.resize(kMaxYearRows);
        // Emit newest-year-first for a stable, sensible display order.
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& [y, v] : rows) {
            std::sort(v.begin(), v.end(), by_plays);
            const std::string ys = std::to_string(y);
            set.collections.push_back({"year:" + ys, "year",
                                       ys + " hits",
                                       "The best of " + ys,
                                       ys, take(v)});
        }
    }

    set.content_digest = compute_digest(set);

    // ---- pre-build both reply bodies -----------------------------------
    json collections = json::array();
    for (const auto& c : set.collections) collections.push_back(collection_to_json(c));
    json plain{
        {"epoch",               set.epoch},
        {"snapshot_height",     set.snapshot_height},
        {"snapshot_block_hash", crypto::to_hex(set.snapshot_block_hash)},
        {"content_digest",      set.content_digest},
        {"collections",         std::move(collections)},
    };

    // Embed join: every referenced song once (deduped across rows), keyed by
    // hash, so `top:` and `rising:` overlapping doesn't double-ship metadata.
    json songs = json::object();
    {
        std::unordered_map<std::string, const Entry*> by_hex;
        for (const auto& e : entries) by_hex[crypto::to_hex(e.ch)] = &e;
        for (const auto& c : set.collections) {
            for (const auto& h : c.songs) {
                const std::string hex = crypto::to_hex(h);
                if (songs.contains(hex)) continue;
                if (auto it = by_hex.find(hex); it != by_hex.end()) {
                    const Entry* e = it->second;
                    songs[hex] = json{
                        {"content_hash", hex},
                        {"title",        e->title},
                        {"artist",       e->artist},
                        {"album",        e->album},
                        {"genre",        e->genre},
                        {"year",         e->year},
                        {"track_number", e->track_number},
                        {"duration_ms",  e->duration_ms},
                        {"play_count",   e->play_count},
                    };
                }
            }
        }
    }
    json embed = plain;
    embed["songs"] = std::move(songs);

    const size_t n_collections = set.collections.size();
    const std::string digest_prefix = set.content_digest.substr(0, 16);
    {
        std::lock_guard<std::mutex> lk(mu_);
        set_        = std::move(set);
        body_plain_ = std::make_shared<const json>(std::move(plain));
        body_embed_ = std::make_shared<const json>(std::move(embed));
        ready_      = true;
        last_epoch_ = epoch;
    }
    std::cout << "[curation] epoch " << epoch
              << " (snapshot h=" << snapshot_height << "): "
              << n_collections << " collections from "
              << entries.size() << " songs, digest="
              << digest_prefix << "…\n";
}

} // namespace mc::curation
