// main.cpp — bopwire-web-gateway.
//
// HTTPS/JSON façade (behind Caddy) over the librats network. Serves the Discover
// feed, streams audio pulled from the swarm, and runs honest play sessions so a
// web play mints the artist/seeder/mini reward — never the listener.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "httplib.h"
#include "json.hpp"

#include "rats_link.h"
#include "streamer.h"

using namespace bopwire::gw;
using json = nlohmann::json;

// ─────────────────────────── config ───────────────────────────
static std::string env_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : d;
}
static int env_int(const char* k, int d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::atoi(v) : d;
}

// ─────────────────────────── helpers ──────────────────────────
static std::string random_addr() {
    static std::mutex m;
    static std::mt19937_64 rng(std::random_device{}());
    std::lock_guard<std::mutex> lk(m);
    static const char* hex = "0123456789abcdef";
    std::string s; s.reserve(40);
    for (int i = 0; i < 40; ++i) s.push_back(hex[rng() & 0xF]);
    return s;
}

// Behind Caddy, X-Forwarded-For is "client, proxy...". The Caddyfile resets XFF
// to the real peer, but we still take the LAST comma-separated hop defensively
// (a client can't append after Caddy). Fall back to the socket peer.
static std::string client_ip(const httplib::Request& req) {
    std::string xff = req.get_header_value("X-Forwarded-For");
    if (!xff.empty()) {
        auto pos = xff.find_last_of(',');
        std::string ip = (pos == std::string::npos) ? xff : xff.substr(pos + 1);
        size_t a = ip.find_first_not_of(" \t");
        size_t b = ip.find_last_not_of(" \t");
        if (a != std::string::npos) return ip.substr(a, b - a + 1);
    }
    return req.remote_addr;
}

// Coarse per-IP ceiling on the mint-bearing /api/play/start. This is a DoS/spam
// ceiling, NOT the anti-farm control — web listeners earn nothing, so faking web
// plays has no payoff; the real anti-farm is the per-DEVICE caps on the native
// path. Kept generous so CGNAT / university / VPN egress isn't blocked. Sliding
// 60s window.
static bool ip_allow_start(const std::string& ip) {
    static std::mutex m;
    static std::unordered_map<std::string, std::deque<int64_t>> hits;
    constexpr int     kMaxPerMin = 60;
    constexpr int64_t kWindowMs  = 60000;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(m);
    // Bounded-map guard: evict only genuinely-idle buckets, and do it BEFORE
    // binding a reference into the map — clearing/erasing after `auto& dq` would
    // dangle that reference (heap use-after-free). Active windows stay intact.
    if (hits.size() > 50000) {
        for (auto it = hits.begin(); it != hits.end(); ) {
            while (!it->second.empty() && now - it->second.front() > kWindowMs)
                it->second.pop_front();
            if (it->second.empty()) it = hits.erase(it);
            else ++it;
        }
    }
    auto& dq = hits[ip];
    while (!dq.empty() && now - dq.front() > kWindowMs) dq.pop_front();
    if (static_cast<int>(dq.size()) >= kMaxPerMin) return false;
    dq.push_back(now);
    return true;
}

static json normalize_song(const json& s) {
    return json{
        {"contentHash", s.value("content_hash", "")},
        {"title",       s.value("title", "")},
        {"artist",      s.value("artist", "")},
        {"album",       s.value("album", "")},
        {"genre",       s.value("genre", "")},
        {"year",        s.value("year", 0)},
        {"trackNumber", s.value("track_number", 0)},
        {"durationMs",  s.value("duration_ms", 0)},
        {"playCount",   s.value("play_count", 0)},
        {"swarmSize",   s.value("swarm_size", 0)},
    };
}

// ───────────────── open-stream (PieceStore) cache ─────────────────
// One opened PieceStore per song, reused across the browser's range requests
// (so seeks/re-reads hit cached pieces). Concurrent first-opens are de-duped;
// the least-recently-opened store is evicted past the cap.
class StoreCache {
public:
    StoreCache(RatsLink& link, size_t max_stores) : link_(link), cap_(max_stores) {}

    std::shared_ptr<PieceStore> peek(const std::string& h) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(h);
        return it == map_.end() ? nullptr : it->second;
    }

    // Opened store (cached) or nullptr if it can't open (no swarm). Blocking on
    // first open (stream.open + first piece); instant on subsequent calls.
    std::shared_ptr<PieceStore> get(const std::string& h) {
        if (auto s = peek(h)) return s;
        std::shared_future<std::shared_ptr<PieceStore>> fut;
        bool owner = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (auto it = map_.find(h); it != map_.end()) return it->second;
            if (auto it = inflight_.find(h); it != inflight_.end()) fut = it->second;
            else {
                owner = true;
                RatsLink* lk = &link_;
                fut = std::async(std::launch::async, [lk, h] {
                    auto ps = std::make_shared<PieceStore>(*lk, h);
                    return ps->open() ? ps : std::shared_ptr<PieceStore>();
                }).share();
                inflight_[h] = fut;
            }
        }
        std::shared_ptr<PieceStore> ps;
        try { ps = fut.get(); } catch (...) { ps = nullptr; }
        if (owner) {
            std::lock_guard<std::mutex> lk(m_);
            inflight_.erase(h);
            if (ps) {
                map_[h] = ps; order_.push_back(h);
                while (order_.size() > cap_) { map_.erase(order_.front()); order_.pop_front(); }
            }
        }
        return ps;
    }
private:
    RatsLink& link_;
    std::mutex m_;
    std::unordered_map<std::string, std::shared_ptr<PieceStore>> map_;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<PieceStore>>> inflight_;
    std::deque<std::string> order_;
    size_t cap_;
};

// ─────────────────────────── main ─────────────────────────────
int main() {
    const std::string vps_host = env_or("BOPWIRE_VPS_HOST", "127.0.0.1");
    const int         vps_port = env_int("BOPWIRE_VPS_PORT", 8080);
    const std::string gw_id    = env_or("BOPWIRE_GATEWAY_ID", "");       // 40-hex or auto
    const std::string host     = env_or("BOPWIRE_LISTEN_HOST", "127.0.0.1");
    const int         port     = env_int("BOPWIRE_LISTEN_PORT", 8090);
    const size_t      max_streams = (size_t) env_int("BOPWIRE_MAX_STREAMS", 24);

    // Allowed CORS origins (comma-separated).
    std::vector<std::string> origins;
    {
        std::string raw = env_or("BOPWIRE_ALLOWED_ORIGINS",
                                 "https://bopwire.com,https://www.bopwire.com");
        size_t p = 0, c;
        while ((c = raw.find(',', p)) != std::string::npos) {
            origins.push_back(raw.substr(p, c - p)); p = c + 1;
        }
        origins.push_back(raw.substr(p));
    }

    RatsLink link(vps_host, vps_port, gw_id);
    if (!link.start()) { std::fprintf(stderr, "[gw] could not join the network\n"); return 1; }
    StoreCache stores(link, max_streams);

    const auto t0 = std::chrono::steady_clock::now();

    // catalog cache (coalesce repeated songs.list)
    std::mutex cat_mu;
    json cat_cache = json::array();
    std::chrono::steady_clock::time_point cat_at{};

    auto load_catalog = [&]() -> json {
        std::lock_guard<std::mutex> lk(cat_mu);
        auto now = std::chrono::steady_clock::now();
        if (!cat_cache.empty() &&
            now - cat_at < std::chrono::seconds(8)) return cat_cache;
        const std::string node = link.pick_full_node();
        if (node.empty()) throw std::runtime_error("no_node");
        json r = link.rpc_via_relay(node, "songs.list", json::object(), 15000);
        json arr = json::array();
        for (const auto& s : r.value("body", json::array()))
            if (s.value("swarm_size", 0) > 0) arr.push_back(normalize_song(s));
        cat_cache = arr; cat_at = now;
        return arr;
    };

    // collections cache (deterministic per-epoch Discover feed). The node
    // only regenerates at epoch boundaries, so a 30 s TTL is already generous;
    // the join against the live catalog is re-done on refresh so availability
    // (who's seeding right now) tracks reality while MEMBERSHIP stays the
    // node's deterministic answer — dim, don't drop.
    std::mutex col_mu;
    json col_cache;   // fully joined /api/collections response
    std::chrono::steady_clock::time_point col_at{};

    auto load_collections = [&]() -> json {
        std::lock_guard<std::mutex> lk(col_mu);
        auto now = std::chrono::steady_clock::now();
        if (!col_cache.is_null() &&
            now - col_at < std::chrono::seconds(30)) return col_cache;
        const std::string node = link.pick_full_node();
        if (node.empty()) throw std::runtime_error("no_node");
        json r = link.rpc_via_relay(node, "collections.list",
                                    json{{"embed", true}}, 15000);
        if (r.value("status", std::string()) != "ok")
            throw std::runtime_error(r.value("status", std::string("error")));
        const json body = r.value("body", json::object());

        // Live catalog keyed by hash — the availability/swarmSize overlay.
        std::unordered_map<std::string, json> live;
        try {
            for (const auto& s : load_catalog())
                live[s.value("contentHash", std::string())] = s;
        } catch (...) { /* collections still render, just all dimmed */ }

        const json emb = body.value("songs", json::object());
        json rows = json::array();
        for (const auto& c : body.value("collections", json::array())) {
            json songs = json::array();
            for (const auto& hj : c.value("song_hashes", json::array())) {
                if (!hj.is_string()) continue;
                const std::string h = hj.get<std::string>();
                if (auto it = live.find(h); it != live.end()) {
                    json s = it->second;
                    s["available"] = true;
                    songs.push_back(std::move(s));
                } else if (emb.contains(h)) {
                    // On the deterministic list but not seeded right now.
                    json s = normalize_song(emb.at(h));
                    s["available"] = false;
                    songs.push_back(std::move(s));
                }
            }
            rows.push_back(json{
                {"id",       c.value("id", "")},
                {"kind",     c.value("kind", "")},
                {"title",    c.value("title", "")},
                {"subtitle", c.value("subtitle", "")},
                {"facet",    c.value("facet", "")},
                {"songs",    std::move(songs)},
            });
        }
        col_cache = json{
            {"epoch",             body.value("epoch", 0ull)},
            {"snapshotHeight",    body.value("snapshot_height", 0u)},
            {"snapshotBlockHash", body.value("snapshot_block_hash", "")},
            {"contentDigest",     body.value("content_digest", "")},
            {"collections",       std::move(rows)},
        };
        col_at = now;
        return col_cache;
    };

    // active play sessions (reward lifecycle)
    struct Play { std::string hash, node; };
    std::mutex play_mu;
    std::unordered_map<std::string, Play> plays;

    // album-art cache: key "artist\x1falbum" -> decoded JPEG. A hit is stable
    // (art per album doesn't change) so it lives for the process; a miss is kept
    // briefly so a freshly-scraped cover appears without a gateway restart.
    // Bounded — cleared wholesale past the cap.
    struct ArtEnt { std::string bytes; bool found = false;
                    std::chrono::steady_clock::time_point at{}; };
    std::mutex art_mu;
    std::unordered_map<std::string, ArtEnt> art_cache;

    // ───────────────────────── HTTP ─────────────────────────
    httplib::Server svr;

    auto cors = [&](const httplib::Request& req, httplib::Response& res) {
        std::string origin = req.get_header_value("Origin");
        std::string allow = origins.empty() ? "*" : origins.front();
        for (const auto& o : origins) if (o == origin || o == "*") { allow = origin.empty() ? o : origin; break; }
        res.set_header("Access-Control-Allow-Origin", allow);
        res.set_header("Vary", "Origin");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Range");
        // let the WASM player read the stream size (for the seek bar) cross-origin
        res.set_header("Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges");
    };
    svr.set_post_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        cors(req, res);
    });
    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    auto err = [](httplib::Response& res, int code, const std::string& msg) {
        res.status = code;
        res.set_content(json{{"error", msg}}.dump(), "application/json");
    };

    // Standard base64 decoder (art.get returns the JPEG base64-encoded over the
    // JSON RPC). Skips whitespace/newlines; stops at '='.
    auto b64dec = [](const std::string& s) -> std::string {
        auto val = [](unsigned char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        out.reserve(s.size() * 3 / 4);
        int buf = 0, bits = 0;
        for (unsigned char c : s) {
            if (c == '=') break;
            const int v = val(c);
            if (v < 0) continue;
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((buf >> bits) & 0xff)); }
        }
        return out;
    };

    svr.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        auto up = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - t0).count();
        json out{{"ok", true},
                 {"node", link.pick_full_node().empty() ? "connecting" : "connected"},
                 {"uptime_s", up}};
        res.set_content(out.dump(), "application/json");
    });

    // /api/songs — the catalog, served from the 8 s cache. With any of
    // ?q/?artist/?genre/?album/?offset/?limit/?sort the reply becomes a
    // filtered, sorted SLICE ({total,offset,limit,songs}) so the browser
    // never downloads the whole catalog; bare /api/songs keeps the legacy
    // full-array shape for old clients.
    svr.Get("/api/songs", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const json cat = load_catalog();
            const bool paged = req.has_param("q")      || req.has_param("artist") ||
                               req.has_param("genre")  || req.has_param("album")  ||
                               req.has_param("offset") || req.has_param("limit")  ||
                               req.has_param("sort");
            if (!paged) { res.set_content(cat.dump(), "application/json"); return; }

            auto lc = [](std::string s) {
                for (auto& c : s) c = (char) std::tolower((unsigned char) c);
                return s;
            };
            const std::string q      = lc(req.get_param_value("q"));
            const std::string artist = lc(req.get_param_value("artist"));
            const std::string genre  = lc(req.get_param_value("genre"));
            const std::string album  = lc(req.get_param_value("album"));

            std::vector<json> hits;
            for (const auto& s : cat) {
                if (!artist.empty() && lc(s.value("artist", "")) != artist) continue;
                if (!genre.empty()  && lc(s.value("genre",  "")) != genre)  continue;
                if (!album.empty()  && lc(s.value("album",  "")) != album)  continue;
                if (!q.empty() &&
                    lc(s.value("title",  "")).find(q) == std::string::npos &&
                    lc(s.value("artist", "")).find(q) == std::string::npos &&
                    lc(s.value("album",  "")).find(q) == std::string::npos &&
                    lc(s.value("genre",  "")).find(q) == std::string::npos) continue;
                hits.push_back(s);
            }
            const std::string sort = req.get_param_value("sort");
            if (sort == "plays") {
                std::stable_sort(hits.begin(), hits.end(), [](const json& a, const json& b) {
                    return a.value("playCount", 0ull) > b.value("playCount", 0ull); });
            } else if (sort == "title") {
                std::stable_sort(hits.begin(), hits.end(), [](const json& a, const json& b) {
                    return a.value("title", "") < b.value("title", ""); });
            } else if (sort == "album") {
                std::stable_sort(hits.begin(), hits.end(), [](const json& a, const json& b) {
                    const auto aa = a.value("album", ""), ba = b.value("album", "");
                    if (aa != ba) return aa < ba;
                    return a.value("trackNumber", 0) < b.value("trackNumber", 0); });
            }
            size_t offset = 0, limit = 100;
            try { if (req.has_param("offset")) offset = std::stoul(req.get_param_value("offset")); } catch (...) {}
            try { if (req.has_param("limit"))  limit  = std::stoul(req.get_param_value("limit"));  } catch (...) {}
            if (limit < 1) limit = 1; if (limit > 500) limit = 500;
            if (offset > hits.size()) offset = hits.size();
            json page = json::array();
            for (size_t i = offset; i < hits.size() && page.size() < limit; ++i)
                page.push_back(std::move(hits[i]));
            res.set_content(json{
                {"total",  hits.size()},
                {"offset", offset},
                {"limit",  limit},
                {"songs",  std::move(page)},
            }.dump(), "application/json");
        }
        catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    // /api/art?artist=<enc>&album=<enc> — real album cover (JPEG) the node
    // scraped into DB2 via sacad. Immutable-cached in the browser; a miss is 404
    // so the player's <img onError> falls back to generated cover art.
    svr.Get("/api/art", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string artist = req.get_param_value("artist");
        const std::string album  = req.get_param_value("album");
        if (artist.empty()) { err(res, 400, "artist required"); return; }
        const std::string key = artist + std::string(1, '\x1f') + album;

        {   // cache lookup
            std::lock_guard<std::mutex> lk(art_mu);
            auto it = art_cache.find(key);
            if (it != art_cache.end()) {
                if (it->second.found) {
                    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
                    res.set_content(it->second.bytes, "image/jpeg");
                    return;
                }
                if (std::chrono::steady_clock::now() - it->second.at <
                    std::chrono::seconds(90)) { err(res, 404, "no_art"); return; }
            }
        }

        std::string jpeg;
        bool found = false;
        try {
            const std::string node = link.pick_full_node();
            if (node.empty()) throw std::runtime_error("no_node");
            json r = link.rpc_via_relay(node, "art.get",
                                        json{{"artist", artist}, {"album", album}}, 12000);
            const json body = r.value("body", json::object());
            if (body.is_object() && body.contains("data_b64")) {
                jpeg  = b64dec(body.value("data_b64", std::string()));
                found = !jpeg.empty();
            }
        } catch (const std::exception&) {
            // transient node/relay error — don't negative-cache; just fall back.
            err(res, 404, "no_art");
            return;
        }

        {   // bounded cache update
            std::lock_guard<std::mutex> lk(art_mu);
            if (art_cache.size() > 4096) art_cache.clear();
            art_cache[key] = ArtEnt{jpeg, found, std::chrono::steady_clock::now()};
        }
        if (found) {
            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
            res.set_content(jpeg, "image/jpeg");
        } else {
            err(res, 404, "no_art");
        }
    });

    // /api/facets — distinct artists / genres / years of the live catalog
    // with counts. A few KB; the Browse drill renders from this and then
    // pages /api/songs?artist=… per drill instead of holding the catalog.
    svr.Get("/api/facets", [&](const httplib::Request&, httplib::Response& res) {
        try {
            const json cat = load_catalog();
            auto lc = [](std::string s) {
                for (auto& c : s) c = (char) std::tolower((unsigned char) c);
                return s;
            };
            std::map<std::string, std::pair<std::string, size_t>> artists, genres;
            // artist_lc -> (album_lc -> display album), for the artist-tile cover
            // cycle: the client fetches /api/art for these and crossfades the hits.
            std::map<std::string, std::map<std::string, std::string>> artistAlbums;
            std::map<int, size_t> years;
            for (const auto& s : cat) {
                const std::string ar = s.value("artist", ""), ge = s.value("genre", ""),
                                  al = s.value("album", "");
                if (!ar.empty()) {
                    auto& e = artists[lc(ar)]; if (e.first.empty()) e.first = ar; ++e.second;
                    if (!al.empty()) artistAlbums[lc(ar)].emplace(lc(al), al);
                }
                if (!ge.empty()) { auto& e = genres[lc(ge)];  if (e.first.empty()) e.first = ge; ++e.second; }
                const int y = s.value("year", 0);
                if (y > 0) ++years[y];
            }
            auto facet_arr = [](const auto& m) {
                json out = json::array();
                for (const auto& [k, v] : m)
                    out.push_back({{"name", v.first}, {"count", v.second}});
                return out;
            };
            json artists_out = json::array();
            for (const auto& [k, v] : artists) {
                json albums = json::array();
                if (auto it = artistAlbums.find(k); it != artistAlbums.end()) {
                    size_t n = 0;
                    for (const auto& [alc, disp] : it->second) {   // capped per artist
                        albums.push_back(disp);
                        if (++n >= 8) break;
                    }
                }
                artists_out.push_back({{"name", v.first}, {"count", v.second},
                                       {"albums", std::move(albums)}});
            }
            json yr = json::array();
            for (const auto& [y, n] : years) yr.push_back({{"year", y}, {"count", n}});
            res.set_content(json{
                {"total",   cat.size()},
                {"artists", std::move(artists_out)},
                {"genres",  facet_arr(genres)},
                {"years",   std::move(yr)},
            }.dump(), "application/json");
        }
        catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    // /api/collections — the node's deterministic per-epoch Discover feed,
    // joined against the live catalog (offline members kept but flagged
    // available:false so the client dims them instead of dropping them).
    svr.Get("/api/collections", [&](const httplib::Request&, httplib::Response& res) {
        try { res.set_content(load_collections().dump(), "application/json"); }
        catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    svr.Get(R"(/api/collections/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const json all = load_collections();
            const std::string id = req.matches[1];
            for (const auto& c : all.value("collections", json::array())) {
                if (c.value("id", std::string()) == id) {
                    res.set_content(c.dump(), "application/json");
                    return;
                }
            }
            err(res, 404, "unknown collection");
        }
        catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    svr.Get("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
        json body = json::object();
        if (req.has_param("q"))      body["q"]      = req.get_param_value("q");
        if (req.has_param("artist")) body["artist"] = req.get_param_value("artist");
        if (req.has_param("genre"))  body["genre"]  = req.get_param_value("genre");
        try {
            const std::string node = link.pick_full_node();
            if (node.empty()) { err(res, 503, "no_node"); return; }
            json r = link.rpc_via_relay(node, "songs.search", body, 15000);
            json arr = json::array();
            for (const auto& s : r.value("body", json::array()))
                if (s.value("swarm_size", 0) > 0) arr.push_back(normalize_song(s));
            res.set_content(arr.dump(), "application/json");
        } catch (const std::exception& e) { err(res, 503, e.what()); }
    });

    // GET /api/stream/<64-hex> — audio, streamed progressively from the swarm.
    // httplib's content provider handles Range (206) by driving `offset`; we
    // pull ~256 KB per call from the on-demand PieceStore so first-byte latency
    // is one piece fetch, not the whole file.
    svr.Get(R"(/api/stream/([0-9a-fA-F]{64}))",
            [&](const httplib::Request& req, httplib::Response& res) {
        const std::string h = req.matches[1];
        auto store = stores.get(h);
        if (!store || store->total_size() <= 0) {
            err(res, 404, "no seeders for this song right now"); return;
        }
        res.set_header("Accept-Ranges", "bytes");
        auto sp = store;   // keep the store alive for the streamed response
        res.set_content_provider(
            (size_t) store->total_size(), store->content_type(),
            [sp](size_t offset, size_t length, httplib::DataSink& sink) -> bool {
                const size_t want = std::min<size_t>(length, 256 * 1024);
                std::string chunk = sp->get_range((int64_t) offset, (int64_t) want);
                if (chunk.empty()) return false;          // EOF or fetch failure
                return sink.write(chunk.data(), chunk.size());
            });
    });

    // ── reward lifecycle (browser reports REAL playback) ──
    svr.Post("/api/play/start", [&](const httplib::Request& req, httplib::Response& res) {
        if (!ip_allow_start(client_ip(req))) { err(res, 429, "rate_limited"); return; }
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string h = in.value("contentHash", "");
        if (h.size() != 64) { err(res, 400, "bad contentHash"); return; }
        const std::string node = link.pick_full_node();
        if (node.empty()) { err(res, 503, "no_node"); return; }
        try {
            json r = link.rpc_via_relay(node, "session.start", json{
                {"content_hash", h},
                // Web listeners earn NOTHING: the all-zero address makes the node
                // skip the discoverer mint lane entirely (mint.cpp:71), so no
                // unspendable token is minted to a throwaway address either.
                {"player_address", std::string(40, '0')},
                // Browser identity for analytics/anti-farm ONLY, never a payout:
                // the page's wallet address if it sent one, else an ephemeral id.
                {"tracking_address", in.value("playerAddress", random_addr())},
                {"attestation", json::object()},
            }, 12000);
            const std::string sid = r.value("body", json::object()).value("session_id", "");
            if (sid.empty()) { err(res, 502, "no session_id"); return; }
            { std::lock_guard<std::mutex> lk(play_mu); plays[sid] = Play{h, node}; }
            res.set_content(json{{"playId", sid}}.dump(), "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    svr.Post("/api/play/heartbeat", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string sid = in.value("playId", "");
        const int64_t pos = in.value("positionMs", 0);
        Play pl; { std::lock_guard<std::mutex> lk(play_mu); auto it = plays.find(sid);
                   if (it == plays.end()) { err(res, 404, "no such play"); return; } pl = it->second; }
        try {
            link.rpc_via_relay(pl.node, "session.heartbeat",
                               json{{"session_id", sid}, {"position_ms", pos}}, 8000);
            res.set_content("{\"ok\":true}", "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    svr.Post("/api/play/complete", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        const std::string sid = in.value("playId", "");
        Play pl; { std::lock_guard<std::mutex> lk(play_mu); auto it = plays.find(sid);
                   if (it == plays.end()) { err(res, 404, "no such play"); return; }
                   pl = it->second; plays.erase(it); }
        json body{{"session_id", sid}};
        if (auto st = stores.peek(pl.hash)) {         // reward lanes from the stream
            if (!st->seeder().empty()) body["seeder_address"]    = st->seeder();
            if (!st->mini().empty())   body["mini_node_address"] = st->mini();
        }
        try {
            json r = link.rpc_via_relay(pl.node, "session.complete", body, 12000);
            res.set_content(r.value("body", json::object()).dump(), "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    // ── DMCA takedown form → node inbox (moderator review) ──
    // The browser posts { representing, phone, email, targets:[{artist,
    // contentHashes:[...]}] }; we relay it to the full node's dmca.submit
    // verb, which drops a structured JSON into the same <data_dir>/dmca/
    // inbox the PDF takedowns use. No listener reward, no chain write —
    // human moderator review at the TUI is the gate.
    svr.Post("/api/dmca", [&](const httplib::Request& req, httplib::Response& res) {
        json in; try { in = json::parse(req.body); } catch (...) { err(res, 400, "bad json"); return; }
        if (in.value("email", std::string()).empty() ||
            !in.contains("targets") || !in["targets"].is_array() || in["targets"].empty()) {
            err(res, 400, "email and at least one target required"); return;
        }
        const std::string node = link.pick_full_node();
        if (node.empty()) { err(res, 503, "no_node"); return; }
        try {
            json r = link.rpc_via_relay(node, "dmca.submit", in, 12000);
            if (r.value("status", std::string()) != "ok") {
                err(res, 502, r.value("error", std::string("submit failed"))); return;
            }
            res.set_content(r.value("body", json::object()).dump(), "application/json");
        } catch (const std::exception& e) { err(res, 502, e.what()); }
    });

    std::printf("[gw] listening on http://%s:%d  (%zu mini(s) in mesh)\n",
                host.c_str(), port, link.mini_count());
    svr.listen(host.c_str(), port);
    link.stop();
    return 0;
}
