// explorer_index.cpp — read-only in-memory index behind the explorer verbs.
// See explorer_index.h for the design notes. NOTHING in this file writes to
// the database or the chain.
#include "explorer_index.h"

#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../moderation/mod_action.h"   // Envelope parsing for the ml: mod log
#include "../tokens/ledger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>

using json = nlohmann::json;

namespace mc::api {

namespace {

constexpr uint64_t kMsPerDay = 86400000ULL;

// ---- small helpers ---------------------------------------------------

std::string strip0x(const std::string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return s.substr(2);
    return s;
}

bool is_hex_str(const std::string& s) {
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return !s.empty();
}

template <size_t N>
bool parse_hex_arr(const std::string& in, std::array<uint8_t, N>& out) {
    const std::string h = strip0x(in);
    if (h.size() != N * 2 || !is_hex_str(h)) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < N; ++i)
        out[i] = static_cast<uint8_t>((nib(h[2 * i]) << 4) | nib(h[2 * i + 1]));
    return true;
}

std::string lc(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hexs(const Hash256& h)  { return crypto::to_hex(h); }
std::string hexa(const Address& a)  { return crypto::to_checksum_hex(a); }
std::string hexp(const PubKey33& p) { return crypto::to_hex(p.data(), p.size()); }
std::string hexsig(const Sig64& s)  { return crypto::to_hex(s.data(), s.size()); }

bool is_zero_addr(const Address& a) {
    for (uint8_t b : a) if (b) return false;
    return true;
}

// Howard Hinnant's civil-date math — portable (no gmtime_r on MinGW).
void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t  yy  = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::string day_str(uint32_t day) {
    int y; unsigned m, d;
    civil_from_days(static_cast<int64_t>(day), y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

// "YYYY-MM-DD" -> utc day number; false on malformed input.
bool parse_date_day(const std::string& s, uint32_t& out) {
    int y = 0; unsigned m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return false;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    out = static_cast<uint32_t>(days_from_civil(y, m, d));
    return true;
}

// Optional {since:"YYYY-MM-DD"} / {since_ms:<ms>} — returns the utc day
// cutoff (inclusive) or 0 when absent/invalid.
uint32_t since_day_of(const json& in) {
    if (in.contains("since_ms") && in["since_ms"].is_number())
        return static_cast<uint32_t>(in["since_ms"].get<uint64_t>() / kMsPerDay);
    uint32_t d = 0;
    if (in.contains("since") && in["since"].is_string() &&
        parse_date_day(in["since"].get<std::string>(), d))
        return d;
    return 0;
}

json err_body(const std::string& msg) { return json{{"error", msg}}; }

// "hide_artist" -> {"hide","artist"}; "forgery_report" -> {"forgery_report","hash"}.
void split_mod_action(const std::string& action, std::string& kind,
                      std::string& category) {
    if (action == "forgery_report") { kind = action; category = "hash"; return; }
    const auto us = action.find('_');
    if (us == std::string::npos) { kind = action; category = ""; return; }
    kind     = action.substr(0, us);
    category = action.substr(us + 1);
}

const char* tx_type_name(uint8_t t) {
    switch (static_cast<TxType>(t)) {
        case TxType::TRANSFER:           return "transfer";
        case TxType::MINT:               return "mint";
        case TxType::MODERATOR_OP:       return "moderator_op";
        case TxType::MODERATOR_PROPOSAL: return "moderator_proposal";
        case TxType::USERNAME_REGISTER:  return "username_register";
        case TxType::SLASH:              return "slash";
        case TxType::RELAY_REWARD:       return "relay_reward";
        case TxType::NODE_AUTH:          return "node_auth";
        case TxType::CHECKPOINT:         return "checkpoint";
        case TxType::SETTLEMENT_MINT:    return "settlement_mint";
        case TxType::RATING:             return "rating";
        default:                         return "unknown";
    }
}

json proof_to_json(const PlayProof& p) {
    return json{
        {"session_id",           hexs(p.session_id)},
        {"content_hash",         hexs(p.content_hash)},
        {"block_hash",           hexs(p.block_hash)},
        {"artist_address",       hexa(p.artist_address)},
        {"player_address",       hexa(p.player_address)},
        {"serving_node_id",      hexs(p.serving_node_id)},
        {"play_start_timestamp", p.play_start_timestamp},
        {"play_end_timestamp",   p.play_end_timestamp},
        {"total_duration_ms",    p.total_duration_ms},
        {"heartbeat_count",      p.heartbeat_count},
        {"node_signature",       hexsig(p.node_signature)},
        {"seeder_address",       hexa(p.seeder_address)},
        {"mini_node_address",    hexa(p.mini_node_address)},
        {"serving_node_pubkey",  hexp(p.serving_node_pubkey)},
        {"player_pubkey",        hexp(p.player_pubkey)},
        {"player_signature",     hexsig(p.player_signature)},
        {"mini_node_pubkey",     hexp(p.mini_node_pubkey)},
        {"mini_node_signature",  hexsig(p.mini_node_signature)},
        {"version",              p.version},
    };
}

json song_section_to_json(const SongSection& s) {
    json splits = json::array();
    for (const auto& rs : s.royalty_splits)
        splits.push_back({{"address", hexa(rs.address)},
                          {"basis_points", rs.basis_points}});
    return json{
        {"audio_format",           audio_format_to_string(s.audio_format)},
        {"content_hash",           hexs(s.content_hash)},
        {"compressed_fingerprint", s.compressed_fingerprint},
        {"duration_ms",            s.duration_ms},
        {"title",                  s.title},
        {"artist",                 s.artist},
        {"artist_address",         hexa(s.artist_address)},
        {"genre",                  s.genre},
        {"album",                  s.album},
        {"year",                   s.year},
        {"track_number",           s.track_number},
        {"royalty_splits",         std::move(splits)},
    };
}

std::vector<std::string> role_names(uint16_t bits) {
    std::vector<std::string> out;
    if (bits & 1)   out.push_back("sender");
    if (bits & 2)   out.push_back("recipient");
    if (bits & 4)   out.push_back("listener");
    if (bits & 8)   out.push_back("artist");
    if (bits & 16)  out.push_back("seeder");
    if (bits & 32)  out.push_back("relay");
    if (bits & 64)  out.push_back("node");
    return out;
}

} // namespace

// ---------------------------------------------------------------------
// sync
// ---------------------------------------------------------------------

void ExplorerIndex::reset_locked() {
    blocks_.clear(); tx_loc_.clear(); addr_.clear(); songs_.clear();
    artists_.clear(); artists_by_name_.clear(); artist_by_name_.clear();
    genre_counts_.clear();
    mod_actions_.clear(); mod_seen_sigs_.clear();
    proposals_.clear(); proposals_by_target_.clear();
    mod_log_synced_ts_ = 0;
    mod_hides_ = mod_unhides_ = mod_grants_ = mod_revokes_ = mod_label_edits_ = 0;
    total_txs_ = total_plays_ = total_minted_ = total_burned_ = 0;
    tx_type_counts_.clear();
    uniq_listeners_.clear(); uniq_artists_.clear();
    uniq_seeders_.clear();   uniq_relays_.clear();
    daily_.clear();
    indexed_height_ = 0; have_any_ = false; indexed_tip_hash_ = Hash256{};
}

void ExplorerIndex::ensure_synced_locked() {
    const auto tip = chain_.tip();
    if (have_any_) {
        // Reorg / restart-onto-other-branch guard: the block we indexed last
        // must still be the canonical block at that height. If not, rebuild
        // from scratch (bounded by FINALITY_DEPTH; the walk is fast).
        auto h = chain_.get_block_hash(indexed_height_);
        if (!h || *h != indexed_tip_hash_) {
            std::cerr << "[explorer] indexed tip moved (reorg?) — full rebuild\n";
            reset_locked();
        }
    }
    if (!have_any_ || indexed_height_ < tip.height) {
        const uint32_t start = have_any_ ? indexed_height_ + 1 : 0;
        for (uint32_t h = start; h <= tip.height; ++h) {
            auto bh = chain_.get_block_hash(h);
            if (!bh) continue;                   // gap (no block at this height)
            auto raw = db_.get("b:" + db_.hex(*bh));
            if (!raw) continue;
            Block b;
            if (!Block::deserialize(raw->data(), raw->size(), b)) continue;
            index_block_locked(h, *bh, b, raw->size());
            indexed_height_   = h;
            indexed_tip_hash_ = *bh;
            have_any_         = true;
        }
    }
    // The ml: mod log advances independently of blocks (hide/unhide are
    // gossip, not txs), so pull its delta on every sync.
    sync_mod_log_locked();
}

// Chain height whose block was current at wall-clock ts_ms (for stamping
// gossip-log actions with an approximate height). Block timestamps ascend.
uint32_t ExplorerIndex::height_at_ts_locked(uint64_t ts_ms) const {
    uint32_t h = 0;
    for (const auto& [height, row] : blocks_) {
        if (row.timestamp_ms > ts_ms) break;
        h = height;
    }
    return h;
}

void ExplorerIndex::add_mod_action_locked(ModAction&& a) {
    if      (a.kind == "hide")   mod_hides_   += 1;
    else if (a.kind == "unhide") mod_unhides_ += 1;
    else if (a.kind == "grant")  mod_grants_  += 1;
    else if (a.kind == "revoke") mod_revokes_ += 1;
    else if (a.kind == "label_edit") mod_label_edits_ += 1;
    mod_actions_.push_back(std::move(a));
}

// Pull new entries out of the replicated ml: moderation log. Every entry is a
// moderator-SIGNED envelope every node already holds and verified on ingest —
// public data, no privileged verbs involved.
void ExplorerIndex::sync_mod_log_locked() {
    const uint64_t since = mod_log_synced_ts_;   // inclusive; sig-dedup below
    bool added = false;
    db_.iter_mod_log_since(since,
        [&](uint64_t ts_ms, const std::string& sig16, const std::string& payload) {
            if (!mod_seen_sigs_.insert(sig16).second) return true;
            if (ts_ms > mod_log_synced_ts_) mod_log_synced_ts_ = ts_ms;
            moderation::Envelope env;
            try {
                auto j = json::parse(payload);
                if (!moderation::from_json(j, env)) return true;
            } catch (...) { return true; }
            ModAction a;
            split_mod_action(env.action, a.kind, a.category);
            a.value      = env.value;
            a.pubkey_hex = env.mod_pub_hex;
            a.ts_ms      = ts_ms;
            a.height     = height_at_ts_locked(ts_ms);
            a.sig_hex    = env.sig_hex;
            a.source     = "gossip";
            auto pub = crypto::from_hex(env.mod_pub_hex);
            if (pub.size() == 33) {
                PubKey33 p{};
                std::copy(pub.begin(), pub.end(), p.begin());
                a.moderator = crypto::address_from_pubkey(p);
            }
            add_mod_action_locked(std::move(a));
            added = true;
            return true;
        });
    if (added)
        std::stable_sort(mod_actions_.begin(), mod_actions_.end(),
                         [](const ModAction& x, const ModAction& y) {
                             return x.ts_ms < y.ts_ms;
                         });
}

ExplorerIndex::AddrInfo& ExplorerIndex::touch_locked(const Address& a,
                                                     uint32_t height,
                                                     uint32_t index,
                                                     uint16_t roles) {
    AddrInfo& ai = addr_[a];
    if (ai.refs.empty()) ai.first_seen = height;
    ai.last_seen = height;
    if (!ai.refs.empty() && ai.refs.back().height == height &&
        ai.refs.back().index == index) {
        ai.refs.back().roles |= roles;           // same tx — merge role bits
    } else {
        ai.refs.push_back(TxRef{height, index, roles});
    }
    return ai;
}

// Shared play-attribution for a MINT proof or a settlement constituent.
// Counts the play against song/artist/listener/seeder/relay aggregates and
// stamps role bits; it does NOT touch amounts (the caller owns those).
void ExplorerIndex::index_play_locked(const PlayProof& proof, uint32_t height,
                                      uint32_t tx_index, uint32_t sub,
                                      uint64_t day, uint64_t /*ts_ms*/) {
    total_plays_ += 1;
    daily_[static_cast<uint32_t>(day)].first += 1;

    SongAgg& sa = songs_[proof.content_hash];
    sa.plays += 1;
    sa.plays_by_day[static_cast<uint32_t>(day)] += 1;
    sa.play_refs.push_back(PlayRef{height, tx_index, sub});

    ArtistAgg* ar = nullptr;
    if (!is_zero_addr(proof.artist_address)) {
        ar = &artists_[proof.artist_address];
        ar->plays += 1;
        ar->plays_per_song[proof.content_hash] += 1;
        ar->plays_by_day[static_cast<uint32_t>(day)] += 1;
        ar->blocks.insert(height);
        uniq_artists_.insert(proof.artist_address);
        AddrInfo& ai = touch_locked(proof.artist_address, height, tx_index, R_ARTIST);
        ai.plays_artist += 1;
    }
    // Name-keyed artist view: attribute the play to the NAME the song was
    // registered under (one uploader wallet can carry many artist names, so
    // the address agg alone would merge them).
    ArtistAgg* an = nullptr;
    if (!sa.artist_name.empty()) {
        an = &artists_by_name_[lc(sa.artist_name)];
        an->plays += 1;
        an->plays_per_song[proof.content_hash] += 1;
        an->plays_by_day[static_cast<uint32_t>(day)] += 1;
        an->blocks.insert(height);
    }
    if (!is_zero_addr(proof.player_address)) {
        uniq_listeners_.insert(proof.player_address);
        sa.listeners.insert(proof.player_address);
        if (ar) ar->listeners.insert(proof.player_address);
        if (an) an->listeners.insert(proof.player_address);
        AddrInfo& ai = touch_locked(proof.player_address, height, tx_index, R_LISTENER);
        ai.plays_listener += 1;
        ai.listen_days[static_cast<uint32_t>(day)] += 1;
    }
    if (!is_zero_addr(proof.seeder_address)) {
        uniq_seeders_.insert(proof.seeder_address);
        if (ar) ar->seeders[proof.seeder_address] += 1;
        if (an) an->seeders[proof.seeder_address] += 1;
        AddrInfo& ai = touch_locked(proof.seeder_address, height, tx_index, R_SEEDER);
        ai.plays_seeder += 1;
    }
    if (!is_zero_addr(proof.mini_node_address)) {
        uniq_relays_.insert(proof.mini_node_address);
        if (ar) ar->relays[proof.mini_node_address] += 1;
        if (an) an->relays[proof.mini_node_address] += 1;
        AddrInfo& ai = touch_locked(proof.mini_node_address, height, tx_index, R_RELAY);
        ai.plays_relay += 1;
    }
}

void ExplorerIndex::index_block_locked(uint32_t height, const Hash256& hash,
                                       const Block& block, size_t size_bytes) {
    const uint64_t ts  = block.header.timestamp_ms;
    const uint64_t day = ts / kMsPerDay;

    BlockRow row;
    row.hash         = hash;
    row.timestamp_ms = ts;
    row.tx_count     = static_cast<uint32_t>(block.transactions.size());
    row.size_bytes   = static_cast<uint32_t>(size_bytes);
    row.has_song     = block.has_song;

    // Song registration.
    if (block.has_song) {
        const SongSection& s = block.song;
        row.song_title  = s.title;
        row.song_artist = s.artist;
        SongAgg& sa = songs_[s.content_hash];
        sa.reg_height     = height;
        sa.reg_block_hash = hash;
        sa.title          = s.title;
        sa.artist_name    = s.artist;
        sa.genre          = s.genre;
        sa.artist_address = s.artist_address;
        if (!s.genre.empty()) genre_counts_[lc(s.genre)] += 1;
        if (!is_zero_addr(s.artist_address)) {
            uniq_artists_.insert(s.artist_address);
            ArtistAgg& ar = artists_[s.artist_address];
            if (ar.name.empty()) ar.name = s.artist;
            ar.blocks.insert(height);
            if (!s.artist.empty()) {
                ar.names.insert(s.artist);
                artist_by_name_[lc(s.artist)] = s.artist_address;
            }
        }
        if (!s.artist.empty()) {
            ArtistAgg& an = artists_by_name_[lc(s.artist)];
            if (an.name.empty()) an.name = s.artist;   // display-cased
            an.blocks.insert(height);
            if (!is_zero_addr(s.artist_address))
                an.addresses.insert(s.artist_address);
        }
    }

    for (uint32_t i = 0; i < block.transactions.size(); ++i) {
        const auto& raw = block.transactions[i];
        if (raw.empty()) continue;
        const uint8_t type = raw[0];
        const Hash256 txh  = crypto::sha256(raw.data(), raw.size());
        tx_loc_[txh] = TxLoc{height, i};
        total_txs_  += 1;
        tx_type_counts_[type] += 1;

        switch (static_cast<TxType>(type)) {
        case TxType::TRANSFER: {
            TransferTx tx;
            if (!TransferTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.from_address, height, i, R_SENDER);
            touch_locked(tx.to_address,   height, i, R_RECIPIENT);
            break;
        }
        case TxType::MINT: {
            MintTx m;
            if (!MintTx::deserialize(raw.data(), raw.size(), m)) break;
            row.weight += 1;
            index_play_locked(m.proof, height, i, UINT32_MAX, day, ts);

            // Serving-node address (v3 proofs carry the pubkey inline).
            Address node_addr{};
            {
                PubKey33 zero{};
                if (m.proof.serving_node_pubkey != zero)
                    node_addr = crypto::address_from_pubkey(m.proof.serving_node_pubkey);
            }
            const Address artist_escrow =
                crypto::escrow_address_for(m.proof.artist_address);
            for (const auto& o : m.outputs) {
                total_minted_ += o.amount;
                daily_[static_cast<uint32_t>(day)].second += o.amount;
                if (o.recipient == m.proof.player_address) {
                    touch_locked(o.recipient, height, i, R_LISTENER)
                        .earned_listener += o.amount;
                } else if (!is_zero_addr(m.proof.seeder_address) &&
                           o.recipient == m.proof.seeder_address) {
                    touch_locked(o.recipient, height, i, R_SEEDER)
                        .earned_seeder += o.amount;
                } else if (!is_zero_addr(m.proof.mini_node_address) &&
                           o.recipient == m.proof.mini_node_address) {
                    touch_locked(o.recipient, height, i, R_RELAY)
                        .earned_relay += o.amount;
                } else if (o.recipient == m.proof.artist_address ||
                           o.recipient == artist_escrow) {
                    // Artist lane — escrowed pre-10k output is attributed to
                    // the ARTIST address (that is who it is held for).
                    if (!is_zero_addr(m.proof.artist_address)) {
                        touch_locked(m.proof.artist_address, height, i, R_ARTIST)
                            .earned_artist += o.amount;
                        artists_[m.proof.artist_address].earned += o.amount;
                        SongAgg& esa = songs_[m.proof.content_hash];
                        esa.artist_earned += o.amount;
                        if (!esa.artist_name.empty())
                            artists_by_name_[lc(esa.artist_name)].earned += o.amount;
                    } else {
                        touch_locked(o.recipient, height, i, R_RECIPIENT)
                            .earned_other += o.amount;
                    }
                } else if (!is_zero_addr(node_addr) && o.recipient == node_addr) {
                    touch_locked(o.recipient, height, i, R_NODE)
                        .earned_node += o.amount;
                } else {
                    // Royalty-split recipient / their escrow / unclaimed escrow
                    // / legacy serving node — countable but not lane-splittable.
                    touch_locked(o.recipient, height, i, R_RECIPIENT)
                        .earned_other += o.amount;
                }
            }
            if (m.burn_amount > 0 && !is_zero_addr(m.proof.player_address)) {
                touch_locked(m.proof.player_address, height, i, R_LISTENER)
                    .burned += m.burn_amount;
                total_burned_ += m.burn_amount;
            }
            break;
        }
        case TxType::MODERATOR_OP: {
            ModeratorOpTx tx;
            if (!ModeratorOpTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.proposer, height, i, R_SENDER);
            if (!is_zero_addr(tx.subject))
                touch_locked(tx.subject, height, i, R_RECIPIENT);
            ModAction a;
            if      (tx.op_code == 1) { a.kind = "grant";      a.category = "moderator"; }
            else if (tx.op_code == 2) { a.kind = "revoke";     a.category = "moderator"; }
            else if (tx.op_code == 3) { a.kind = "label_edit"; a.category = "label"; }
            else                      { a.kind = "moderator_op"; a.category = "unknown"; }
            a.value     = tx.op_code == 3 ? tx.meta_json : hexa(tx.subject);
            a.moderator = tx.proposer;
            a.level     = tx.level;
            a.ts_ms     = ts;
            a.height    = height;
            a.tx_hash   = crypto::to_hex(txh);
            a.source    = "block";
            add_mod_action_locked(std::move(a));
            break;
        }
        case TxType::MODERATOR_PROPOSAL: {
            ProposalTx tx;
            if (!ProposalTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.proposer, height, i, R_SENDER);
            if (!is_zero_addr(tx.target_addr))
                touch_locked(tx.target_addr, height, i, R_RECIPIENT);
            ModAction a;
            if      (tx.kind == 1) { a.kind = "proposal_hide";    a.category = "content";
                                     a.value = hexs(tx.target_hash); }
            else if (tx.kind == 2) { a.kind = "proposal_release"; a.category = "escrow";
                                     a.value = hexa(tx.target_addr); }
            else if (tx.kind == 3) { a.kind = "vote_yes";         a.category = "proposal";
                                     a.value = hexs(tx.target_hash); }
            else if (tx.kind == 4) { a.kind = "proposal_grant";   a.category = "moderator";
                                     a.value = hexa(tx.target_addr); }
            else if (tx.kind == 5) { a.kind = "proposal_rating_threshold";
                                     a.category = "rating_policy";
                                     a.value = std::to_string(tx.amount); }
            else                   { a.kind = "proposal";         a.category = "unknown"; }
            a.moderator = tx.proposer;
            a.ts_ms     = ts;
            a.height    = height;
            a.tx_hash   = crypto::to_hex(txh);
            a.source    = "block";
            add_mod_action_locked(std::move(a));
            // Vote-provenance bookkeeping.
            if (tx.kind == 3) {
                // VOTE_YES: target_hash is the PROPOSAL tx hash.
                auto pit = proposals_.find(tx.target_hash);
                if (pit != proposals_.end())
                    pit->second.votes.push_back(
                        VoteRef{tx.proposer, height, crypto::to_hex(txh)});
            } else if (tx.kind == 1 || tx.kind == 2 || tx.kind == 4 || tx.kind == 5) {
                ProposalInfo pi;
                pi.kind        = tx.kind;
                pi.target_hash = tx.target_hash;
                pi.target_addr = tx.target_addr;
                pi.amount      = tx.amount;
                pi.proposer    = tx.proposer;
                pi.height      = height;
                pi.ts_ms       = ts;
                pi.tx_hash     = crypto::to_hex(txh);
                proposals_[txh] = std::move(pi);
                if (tx.kind == 1)
                    proposals_by_target_[tx.target_hash].push_back(txh);
            }
            break;
        }
        case TxType::USERNAME_REGISTER: {
            UsernameTx tx;
            if (!UsernameTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.owner, height, i, R_SENDER);
            break;
        }
        case TxType::RATING: {
            RatingTx tx;
            if (!RatingTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.rater, height, i, R_SENDER);
            // A rating-driven auto-hide is the THIRD source of a hide (after
            // the moderator gossip envelope and the HIDE_CONTENT vote), so it
            // gets a row in the SAME mod_actions_ stream, in the same shape,
            // distinguished by source == "rating". The chain records the hide
            // under rh: with the height it fired at; if that is THIS block,
            // this rating tx is the one that tipped it over.
            if (auto rh = db_.get_rating_hide(tx.content_hash);
                rh && rh->height == height) {
                ModAction a;
                a.kind      = "hide";
                a.category  = "hash";
                a.value     = hexs(tx.content_hash);
                a.moderator = Address{};   // nobody signed this — the rule did
                a.ts_ms     = ts;
                a.height    = height;
                a.tx_hash   = crypto::to_hex(txh);
                a.source    = "rating";
                add_mod_action_locked(std::move(a));
                mod_hides_ += 1;
            }
            break;
        }
        case TxType::SLASH: {
            SlashTx tx;
            if (!SlashTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.reporter_address, height, i, R_SENDER);
            touch_locked(tx.target_address,   height, i, R_RECIPIENT);
            break;
        }
        case TxType::RELAY_REWARD: {
            RelayRewardTx tx;
            if (!RelayRewardTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.issuer_address, height, i, R_SENDER);
            AddrInfo& ai = touch_locked(tx.target_address, height, i,
                                        R_RECIPIENT | R_RELAY);
            ai.earned_relay += tx.count;
            total_minted_   += tx.count;
            daily_[static_cast<uint32_t>(day)].second += tx.count;
            uniq_relays_.insert(tx.target_address);
            break;
        }
        case TxType::NODE_AUTH: {
            NodeAuthTx tx;
            if (!NodeAuthTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.issuer_address, height, i, R_SENDER);
            break;
        }
        case TxType::CHECKPOINT: {
            CheckpointTx tx;
            if (!CheckpointTx::deserialize(raw.data(), raw.size(), tx)) break;
            touch_locked(tx.issuer_address, height, i, R_SENDER);
            break;
        }
        case TxType::SETTLEMENT_MINT: {
            SettlementMintTx tx;
            if (!SettlementMintTx::deserialize(raw.data(), raw.size(), tx)) break;
            const uint32_t plays = std::min<uint32_t>(
                tx.constituent_count, MAX_CONSTITUENTS_PER_SETTLEMENT);
            row.weight += plays;
            touch_locked(tx.serving_node_wallet, height, i, R_SENDER | R_NODE);
            // The constituent proofs flood separately as the sb: body. When we
            // hold it, attribute every play; the credited AMOUNTS are
            // recompute-authoritative (a function of state at apply time) and
            // are NOT re-derived here — settlement earnings are reported as
            // plays, not tokens. When the body is absent the settlement still
            // counts `plays` toward totals/weight, just without per-address
            // attribution.
            auto braw = db_.get("sb:" + crypto::to_hex(tx.constituents_merkle_root));
            std::vector<PlayProof> proofs;
            if (braw && deserialize_settle_body(braw->data(), braw->size(), proofs)) {
                for (uint32_t k = 0; k < proofs.size(); ++k)
                    index_play_locked(proofs[k], height, i, k, day, ts);
            } else {
                total_plays_ += plays;
                daily_[static_cast<uint32_t>(day)].first += plays;
            }
            break;
        }
        default:
            break;
        }
    }

    blocks_[height] = std::move(row);
}

// ---------------------------------------------------------------------
// tx JSON expansion — every field, nothing elided
// ---------------------------------------------------------------------

json ExplorerIndex::tx_to_json_locked(const std::vector<uint8_t>& raw) const {
    json j;
    if (raw.empty()) return json{{"type", "empty"}};
    const uint8_t type = raw[0];
    j["type"]       = tx_type_name(type);
    j["type_byte"]  = type;
    j["size_bytes"] = raw.size();
    j["tx_hash"]    = crypto::to_hex(crypto::sha256(raw.data(), raw.size()));

    switch (static_cast<TxType>(type)) {
    case TxType::TRANSFER: {
        TransferTx tx;
        if (!TransferTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["from"]             = hexa(tx.from_address);
        j["to"]               = hexa(tx.to_address);
        j["amount"]           = tx.amount;
        j["amount_formatted"] = Ledger::format_balance(tx.amount);
        j["nonce"]            = tx.nonce;
        j["from_pubkey"]      = hexp(tx.from_pubkey);
        j["signature"]        = hexsig(tx.signature);
        return j;
    }
    case TxType::MINT: {
        MintTx m;
        if (!MintTx::deserialize(raw.data(), raw.size(), m)) break;
        j["proof"] = proof_to_json(m.proof);
        json outs = json::array();
        uint64_t total = 0;
        for (const auto& o : m.outputs) {
            total += o.amount;
            outs.push_back({{"recipient", hexa(o.recipient)},
                            {"amount", o.amount},
                            {"amount_formatted", Ledger::format_balance(o.amount)}});
        }
        j["outputs"]                = std::move(outs);
        j["total_minted"]           = total;
        j["total_minted_formatted"] = Ledger::format_balance(total);
        j["burn_amount"]            = m.burn_amount;
        j["burn_amount_formatted"]  = Ledger::format_balance(m.burn_amount);
        // Song context (from the index) so the UI can label the play.
        if (auto it = songs_.find(m.proof.content_hash); it != songs_.end()) {
            j["song_title"]  = it->second.title;
            j["song_artist"] = it->second.artist_name;
        }
        return j;
    }
    case TxType::MODERATOR_OP: {
        ModeratorOpTx tx;
        if (!ModeratorOpTx::deserialize(raw.data(), raw.size(), tx)) break;
        static const char* ops[] = {"?", "grant", "revoke", "tag_label_edit"};
        j["op_code"]         = tx.op_code;
        j["op_name"]         = tx.op_code <= 3 ? ops[tx.op_code] : "?";
        j["level"]           = tx.level;
        j["subject"]         = hexa(tx.subject);
        j["subject_pubkey"]  = hexp(tx.subject_pubkey);
        j["proposer"]        = hexa(tx.proposer);
        j["proposer_pubkey"] = hexp(tx.proposer_pubkey);
        j["nonce"]           = tx.nonce;
        j["meta_json"]       = tx.meta_json;
        j["signature"]       = hexsig(tx.signature);
        return j;
    }
    case TxType::MODERATOR_PROPOSAL: {
        ProposalTx tx;
        if (!ProposalTx::deserialize(raw.data(), raw.size(), tx)) break;
        static const char* kinds[] = {"?", "hide_content", "release_escrow",
                                      "vote_yes", "grant_moderator",
                                      "set_rating_threshold"};
        j["kind"]            = tx.kind;
        j["kind_name"]       = tx.kind <= 5 ? kinds[tx.kind] : "?";
        if (tx.kind == static_cast<uint8_t>(ProposalKind::SET_RATING_THRESHOLD)) {
            uint32_t mr = 0, bps = 0;
            unpack_rating_policy(tx.amount, mr, bps);
            j["rating_threshold"] = json{{"min_ratings", mr},
                                         {"down_ratio_bps", bps}};
        }
        j["target_hash"]     = hexs(tx.target_hash);
        j["target_addr"]     = hexa(tx.target_addr);
        j["amount"]          = tx.amount;
        j["proposer"]        = hexa(tx.proposer);
        j["proposer_pubkey"] = hexp(tx.proposer_pubkey);
        j["nonce"]           = tx.nonce;
        j["subject_pubkey"]  = hexp(tx.subject_pubkey);
        j["signature"]       = hexsig(tx.signature);
        return j;
    }
    case TxType::RATING: {
        RatingTx tx;
        if (!RatingTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["content_hash"] = hexs(tx.content_hash);
        j["value"]        = tx.value;
        j["value_name"]   = tx.value == 1 ? "up" : tx.value == 2 ? "down" : "?";
        j["rater"]        = hexa(tx.rater);
        j["rater_pubkey"] = hexp(tx.rater_pubkey);
        j["nonce"]        = tx.nonce;
        j["signature"]    = hexsig(tx.signature);
        return j;
    }
    case TxType::USERNAME_REGISTER: {
        UsernameTx tx;
        if (!UsernameTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["name"]         = tx.name;
        j["owner"]        = hexa(tx.owner);
        j["owner_pubkey"] = hexp(tx.owner_pubkey);
        j["nonce"]        = tx.nonce;
        j["signature"]    = hexsig(tx.signature);
        return j;
    }
    case TxType::SLASH: {
        SlashTx tx;
        if (!SlashTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["kind"]             = static_cast<int>(tx.kind);
        j["kind_name"]        = tx.kind == SlashKind::EQUIVOCATION
                                    ? "equivocation" : "fingerprint_forgery";
        j["target_address"]   = hexa(tx.target_address);
        j["target_pubkey"]    = hexp(tx.target_pubkey);
        j["evidence_hex"]     = crypto::to_hex(tx.evidence);
        j["nonce"]            = tx.nonce;
        j["reporter_address"] = hexa(tx.reporter_address);
        j["reporter_pubkey"]  = hexp(tx.reporter_pubkey);
        j["signature"]        = hexsig(tx.signature);
        return j;
    }
    case TxType::RELAY_REWARD: {
        RelayRewardTx tx;
        if (!RelayRewardTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["target_address"]   = hexa(tx.target_address);
        j["amount"]           = tx.count;
        j["amount_formatted"] = Ledger::format_balance(tx.count);
        j["issuer_address"]   = hexa(tx.issuer_address);
        j["issuer_pubkey"]    = hexp(tx.issuer_pubkey);
        j["nonce"]            = tx.nonce;
        j["signature"]        = hexsig(tx.signature);
        return j;
    }
    case TxType::NODE_AUTH: {
        NodeAuthTx tx;
        if (!NodeAuthTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["node_id"]        = hexs(tx.node_id);
        j["node_pubkey"]    = hexp(tx.node_pubkey);
        j["authorize"]      = tx.authorize != 0;
        j["issuer_address"] = hexa(tx.issuer_address);
        j["issuer_pubkey"]  = hexp(tx.issuer_pubkey);
        j["nonce"]          = tx.nonce;
        j["signature"]      = hexsig(tx.signature);
        return j;
    }
    case TxType::CHECKPOINT: {
        CheckpointTx tx;
        if (!CheckpointTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["height"]         = tx.height;
        j["block_hash"]     = hexs(tx.block_hash);
        j["issuer_address"] = hexa(tx.issuer_address);
        j["issuer_pubkey"]  = hexp(tx.issuer_pubkey);
        j["nonce"]          = tx.nonce;
        j["signature"]      = hexsig(tx.signature);
        return j;
    }
    case TxType::SETTLEMENT_MINT: {
        SettlementMintTx tx;
        if (!SettlementMintTx::deserialize(raw.data(), raw.size(), tx)) break;
        j["serving_node_id"]          = hexs(tx.serving_node_id);
        j["serving_node_wallet"]      = hexa(tx.serving_node_wallet);
        j["epoch_id"]                 = tx.epoch_id;
        j["constituents_merkle_root"] = hexs(tx.constituents_merkle_root);
        j["constituent_count"]        = tx.constituent_count;
        j["node_signature"]           = hexsig(tx.node_signature);
        // Companion body (sb:) — the full constituent proof list when we hold it.
        auto braw = db_.get("sb:" + crypto::to_hex(tx.constituents_merkle_root));
        std::vector<PlayProof> proofs;
        if (braw && deserialize_settle_body(braw->data(), braw->size(), proofs)) {
            json arr = json::array();
            for (const auto& p : proofs) arr.push_back(proof_to_json(p));
            j["constituents"] = std::move(arr);
        } else {
            j["constituents"] = nullptr;   // body not held locally
        }
        return j;
    }
    default:
        break;
    }
    // Undecodable / unknown — still show everything we have.
    j["raw_hex"] = crypto::to_hex(raw);
    j["decode_error"] = true;
    return j;
}

json ExplorerIndex::block_row_json_locked(uint32_t height, const BlockRow& r) const {
    json j{
        {"height",       height},
        {"hash",         hexs(r.hash)},
        {"timestamp_ms", r.timestamp_ms},
        {"tx_count",     r.tx_count},
        {"weight",       r.weight},
        {"size_bytes",   r.size_bytes},
        {"has_song",     r.has_song},
    };
    if (r.has_song) {
        j["song_title"]  = r.song_title;
        j["song_artist"] = r.song_artist;
    }
    return j;
}

// ---------------------------------------------------------------------
// verbs
// ---------------------------------------------------------------------

ExplorerIndex::Result ExplorerIndex::blocks_list(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    size_t limit = 25;
    if (in.contains("limit") && in["limit"].is_number())
        limit = in["limit"].get<size_t>();
    if (limit < 1)   limit = 1;
    if (limit > 500) limit = 500;
    size_t offset = 0;
    if (in.contains("offset") && in["offset"].is_number())
        offset = in["offset"].get<size_t>();

    const uint32_t since_day = since_day_of(in);
    const uint64_t since_ms  = static_cast<uint64_t>(since_day) * kMsPerDay;

    // Range mode: {from,to} inclusive heights (still newest-first, still
    // capped at `limit` rows — "blocks 1..581" pages instead of exploding).
    bool have_range = in.contains("from") || in.contains("to");
    uint32_t from_h = 0, to_h = indexed_height_;
    if (have_range) {
        if (in.contains("from") && in["from"].is_number())
            from_h = in["from"].get<uint32_t>();
        if (in.contains("to") && in["to"].is_number())
            to_h = in["to"].get<uint32_t>();
        if (from_h > to_h) std::swap(from_h, to_h);
    }

    // Collect candidate heights newest-first.
    json rows = json::array();
    size_t matched = 0;
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
        const uint32_t h = it->first;
        if (have_range && (h < from_h || h > to_h)) continue;
        if (since_ms && it->second.timestamp_ms < since_ms) continue;
        if (matched >= offset && rows.size() < limit)
            rows.push_back(block_row_json_locked(h, it->second));
        ++matched;
    }
    json out{
        {"total",  matched},
        {"offset", offset},
        {"limit",  limit},
        {"tip_height", indexed_height_},
        {"blocks", std::move(rows)},
    };
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::blocks_get(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    // Resolve {height} | {hash} | {id: height-or-hash}.
    Hash256 bh{};
    bool have_hash = false;
    if (in.contains("hash") && in["hash"].is_string())
        have_hash = parse_hex_arr<32>(in["hash"].get<std::string>(), bh);
    std::optional<uint32_t> height;
    if (in.contains("height")) {
        if (in["height"].is_number()) height = in["height"].get<uint32_t>();
        else if (in["height"].is_string()) {
            try { height = static_cast<uint32_t>(std::stoul(in["height"].get<std::string>())); }
            catch (...) {}
        }
    }
    if (!have_hash && !height && in.contains("id") && in["id"].is_string()) {
        const std::string id = in["id"].get<std::string>();
        if (parse_hex_arr<32>(id, bh)) have_hash = true;
        else { try { height = static_cast<uint32_t>(std::stoul(id)); } catch (...) {} }
    }
    if (!have_hash && height) {
        auto h = chain_.get_block_hash(*height);
        if (!h) return {404, err_body("no block at height " +
                                      std::to_string(*height)).dump()};
        bh = *h; have_hash = true;
    }
    if (!have_hash) return {400, err_body("need height or hash").dump()};

    auto raw = db_.get("b:" + db_.hex(bh));
    if (!raw) return {404, err_body("block not found").dump()};
    Block b;
    if (!Block::deserialize(raw->data(), raw->size(), b))
        return {500, err_body("stored block failed to deserialize").dump()};
    const auto hopt = chain_.get_block_height(bh);

    json txs = json::array();
    uint64_t weight = 0;
    for (const auto& t : b.transactions) {
        if (t.empty()) continue;
        if (static_cast<TxType>(t[0]) == TxType::MINT) weight += 1;
        else if (static_cast<TxType>(t[0]) == TxType::SETTLEMENT_MINT) {
            SettlementMintTx sm;
            if (SettlementMintTx::deserialize(t.data(), t.size(), sm))
                weight += std::min<uint32_t>(sm.constituent_count,
                                             MAX_CONSTITUENTS_PER_SETTLEMENT);
        }
        txs.push_back(tx_to_json_locked(t));
    }

    json out{
        {"height",     hopt ? json(*hopt) : json(nullptr)},
        {"hash",       hexs(bh)},
        {"size_bytes", raw->size()},
        {"weight",     weight},
        {"header", {
            {"version",          b.header.version},
            {"prev_hash",        hexs(b.header.prev_hash)},
            {"merkle_root",      hexs(b.header.merkle_root)},
            {"fingerprint_hash", hexs(b.header.fingerprint_hash)},
            {"content_hash",     hexs(b.header.content_hash)},
            {"timestamp_ms",     b.header.timestamp_ms},
            {"state_root",       hexs(b.header.state_root)},
            {"song_body_hash",   hexs(b.header.song_body_hash)},
        }},
        {"has_song",     b.has_song},
        {"song",         b.has_song ? song_section_to_json(b.song) : json(nullptr)},
        {"tx_count",     b.transactions.size()},
        {"transactions", std::move(txs)},
    };
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::tx_get(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Hash256 txh{};
    if (!in.contains("hash") || !in["hash"].is_string() ||
        !parse_hex_arr<32>(in["hash"].get<std::string>(), txh))
        return {400, err_body("need 64-hex tx hash").dump()};
    auto it = tx_loc_.find(txh);
    if (it == tx_loc_.end()) return {404, err_body("tx not found").dump()};

    auto brow = blocks_.find(it->second.height);
    if (brow == blocks_.end()) return {404, err_body("tx block missing").dump()};
    auto raw = db_.get("b:" + db_.hex(brow->second.hash));
    if (!raw) return {500, err_body("block row missing").dump()};
    Block b;
    if (!Block::deserialize(raw->data(), raw->size(), b) ||
        it->second.index >= b.transactions.size())
        return {500, err_body("block failed to deserialize").dump()};

    json out = tx_to_json_locked(b.transactions[it->second.index]);
    out["block_height"] = it->second.height;
    out["block_hash"]   = hexs(brow->second.hash);
    out["timestamp_ms"] = brow->second.timestamp_ms;
    out["tx_index"]     = it->second.index;
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::address_summary(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Address a{};
    if (!in.contains("address") || !in["address"].is_string() ||
        !parse_hex_arr<20>(in["address"].get<std::string>(), a))
        return {400, err_body("need 40-hex address").dump()};

    const uint64_t bal    = db_.get_balance(a);
    const Address  escrow = crypto::escrow_address_for(a);
    const uint64_t ebal   = db_.get_balance(escrow);
    const uint64_t nonce  = db_.get_nonce(a);

    static const AddrInfo kEmpty{};
    auto it = addr_.find(a);
    const AddrInfo& ai = it == addr_.end() ? kEmpty : it->second;

    const uint64_t earned = ai.earned_artist + ai.earned_seeder + ai.earned_relay +
                            ai.earned_node + ai.earned_listener + ai.earned_other;
    json out{
        {"address",                  hexa(a)},
        {"balance",                  bal},
        {"balance_formatted",        Ledger::format_balance(bal)},
        {"escrow_address",           hexa(escrow)},
        {"escrow_balance",           ebal},
        {"escrow_balance_formatted", Ledger::format_balance(ebal)},
        {"nonce",                    nonce},
        {"tx_count",                 ai.refs.size()},
        {"first_seen_height",        ai.refs.empty() ? json(nullptr) : json(ai.first_seen)},
        {"last_seen_height",         ai.refs.empty() ? json(nullptr) : json(ai.last_seen)},
        {"plays_as_listener",        ai.plays_listener},
        {"plays_as_artist",          ai.plays_artist},
        {"plays_as_seeder",          ai.plays_seeder},
        {"plays_as_relay",           ai.plays_relay},
        {"earned_total",             earned},
        {"earned_total_formatted",   Ledger::format_balance(earned)},
        {"earned_by_role", {
            {"artist",   ai.earned_artist},
            {"seeder",   ai.earned_seeder},
            {"relay",    ai.earned_relay},
            {"node",     ai.earned_node},
            {"listener", ai.earned_listener},
            {"other",    ai.earned_other},
        }},
        {"burned_total",             ai.burned},
        {"burned_total_formatted",   Ledger::format_balance(ai.burned)},
    };
    if (auto un = db_.get_addr_username(a)) out["username"] = *un;
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::address_history(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Address a{};
    if (!in.contains("address") || !in["address"].is_string() ||
        !parse_hex_arr<20>(in["address"].get<std::string>(), a))
        return {400, err_body("need 40-hex address").dump()};

    size_t offset = 0, limit = 25;
    if (in.contains("offset") && in["offset"].is_number()) offset = in["offset"].get<size_t>();
    if (in.contains("limit")  && in["limit"].is_number())  limit  = in["limit"].get<size_t>();
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;

    // Optional role filter — {role:"listener"|"artist"|"seeder"|"relay"|
    // "sender"|"recipient"|"node"} narrows the page to txs where the address
    // played that role (this is what backs `plays by`, `seeder`, `relay`,
    // `transfers <addr>` command-bar queries).
    uint16_t role_mask = 0xFFFF;
    if (in.contains("role") && in["role"].is_string()) {
        const std::string r = lc(in["role"].get<std::string>());
        if      (r == "sender")    role_mask = R_SENDER;
        else if (r == "recipient") role_mask = R_RECIPIENT;
        else if (r == "listener")  role_mask = R_LISTENER;
        else if (r == "artist")    role_mask = R_ARTIST;
        else if (r == "seeder")    role_mask = R_SEEDER;
        else if (r == "relay")     role_mask = R_RELAY;
        else if (r == "node")      role_mask = R_NODE;
        else if (!r.empty())       return {400, err_body("unknown role").dump()};
    }

    json txs = json::array();
    size_t matched = 0;
    auto it = addr_.find(a);
    if (it != addr_.end()) {
        // Cache the last block we cracked open — refs cluster by block.
        uint32_t cached_h = UINT32_MAX;
        Block    cached_b;
        bool     cached_ok = false;
        for (auto rit = it->second.refs.rbegin(); rit != it->second.refs.rend(); ++rit) {
            if (!(rit->roles & role_mask)) continue;
            if (matched >= offset && txs.size() < limit) {
                auto brow = blocks_.find(rit->height);
                if (brow != blocks_.end()) {
                    if (cached_h != rit->height) {
                        cached_ok = false;
                        if (auto raw = db_.get("b:" + db_.hex(brow->second.hash)))
                            cached_ok = Block::deserialize(raw->data(), raw->size(),
                                                           cached_b);
                        cached_h = rit->height;
                    }
                    if (cached_ok && rit->index < cached_b.transactions.size()) {
                        json t = tx_to_json_locked(cached_b.transactions[rit->index]);
                        t["block_height"] = rit->height;
                        t["block_hash"]   = hexs(brow->second.hash);
                        t["timestamp_ms"] = brow->second.timestamp_ms;
                        t["roles"]        = role_names(rit->roles);
                        txs.push_back(std::move(t));
                    }
                }
            }
            ++matched;
        }
    }
    json out{
        {"address", hexa(a)},
        {"total",   matched},
        {"offset",  offset},
        {"limit",   limit},
        {"history", std::move(txs)},
    };
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::search(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    std::string q = in.value("q", std::string());
    // trim
    while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front()))) q.erase(q.begin());
    while (!q.empty() && std::isspace(static_cast<unsigned char>(q.back())))  q.pop_back();
    if (q.empty()) return {400, err_body("need q").dump()};
    size_t limit = 20;
    if (in.contains("limit") && in["limit"].is_number()) limit = in["limit"].get<size_t>();
    if (limit < 1)  limit = 1;
    if (limit > 50) limit = 50;

    json results = json::array();
    const std::string qs = strip0x(q);

    // Block height?
    if (!qs.empty() && qs.size() <= 9 &&
        std::all_of(qs.begin(), qs.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const uint32_t h = static_cast<uint32_t>(std::stoul(qs));
        auto it = blocks_.find(h);
        if (it != blocks_.end()) {
            json r = block_row_json_locked(h, it->second);
            r["type"] = "block";
            results.push_back(std::move(r));
        }
    }

    // 64-hex: block hash / tx hash / song content hash.
    Hash256 h32{};
    if (parse_hex_arr<32>(qs, h32)) {
        if (auto bh = chain_.get_block_height(h32)) {
            auto it = blocks_.find(*bh);
            if (it != blocks_.end()) {
                json r = block_row_json_locked(*bh, it->second);
                r["type"] = "block";
                results.push_back(std::move(r));
            }
        }
        if (auto it = tx_loc_.find(h32); it != tx_loc_.end()) {
            results.push_back({{"type", "tx"},
                               {"hash", hexs(h32)},
                               {"block_height", it->second.height}});
        }
        if (auto it = songs_.find(h32); it != songs_.end()) {
            results.push_back({{"type", "song"},
                               {"content_hash", hexs(h32)},
                               {"title", it->second.title},
                               {"artist", it->second.artist_name},
                               {"plays", it->second.plays}});
        }
    }

    // 40-hex: address.
    Address a20{};
    if (parse_hex_arr<20>(qs, a20)) {
        auto it = addr_.find(a20);
        json r{{"type", "address"},
               {"address", hexa(a20)},
               {"known", it != addr_.end() || db_.get_balance(a20) > 0},
               {"tx_count", it == addr_.end() ? 0 : it->second.refs.size()},
               {"balance", db_.get_balance(a20)}};
        if (artists_.count(a20)) r["is_artist"] = true;
        if (db_.is_moderator(a20)) {
            r["is_moderator"] = true;
            r["mod_level"]    = db_.get_mod_level(a20);
        }
        results.push_back(std::move(r));
        // A moderator address also surfaces as a moderation hit so the UI can
        // route to /api/moderation/moderator/<addr>.
        size_t acted = 0;
        for (const auto& ma : mod_actions_) if (ma.moderator == a20) ++acted;
        if (acted || db_.is_moderator(a20))
            results.push_back({{"type", "moderator"},
                               {"address", hexa(a20)},
                               {"actions", acted},
                               {"mod_level", db_.get_mod_level(a20)},
                               {"active", db_.is_moderator(a20)}});
    }

    // Username (exact, on-chain registry).
    if (auto ua = db_.lookup_username(lc(q))) {
        results.push_back({{"type", "address"},
                           {"address", hexa(*ua)},
                           {"username", lc(q)},
                           {"known", true},
                           {"balance", db_.get_balance(*ua)}});
    }

    // Text: artists, songs, genres.
    const std::string needle = lc(q);
    if (needle.size() >= 2) {
        for (const auto& [name, ar] : artists_by_name_) {
            if (results.size() >= limit) break;
            if (name.find(needle) == std::string::npos) continue;
            auto addr_it = artist_by_name_.find(name);
            const std::string ahex = addr_it != artist_by_name_.end()
                                         ? hexa(addr_it->second) : std::string();
            size_t song_count = 0;
            for (const auto& [ch2, sa2] : songs_)
                if (lc(sa2.artist_name) == name) ++song_count;
            results.push_back({{"type", "artist"},
                               {"name", ar.name.empty() ? name : ar.name},
                               {"address", ahex},
                               {"artist_address", ahex},
                               {"songs", song_count},
                               {"plays", ar.plays}});
        }
        for (const auto& [ch, sa] : songs_) {
            if (results.size() >= limit) break;
            if (lc(sa.title).find(needle) == std::string::npos &&
                lc(sa.artist_name).find(needle) == std::string::npos) continue;
            results.push_back({{"type", "song"},
                               {"content_hash", hexs(ch)},
                               {"title", sa.title},
                               {"artist", sa.artist_name},
                               {"height", sa.reg_height},
                               {"plays", sa.plays}});
        }
        for (const auto& [g, n] : genre_counts_) {
            if (results.size() >= limit) break;
            if (g.find(needle) == std::string::npos) continue;
            results.push_back({{"type", "genre"}, {"name", g}, {"songs", n}});
        }
        // Moderation actions whose target value matches — a hidden title,
        // artist, album or hash resolves here (latest action per target).
        {
            std::set<std::string> seen;
            for (auto rit = mod_actions_.rbegin();
                 rit != mod_actions_.rend() && results.size() < limit; ++rit) {
                if (lc(rit->value).find(needle) == std::string::npos) continue;
                const std::string k = rit->category + "\x1f" + lc(rit->value);
                if (!seen.insert(k).second) continue;   // only the latest
                json h = mod_action_json_locked(*rit);
                h["type"] = "moderation";
                bool hidden_now = false;
                if      (rit->category == "artist") hidden_now = db_.is_hidden_artist(rit->value);
                else if (rit->category == "album")  hidden_now = db_.is_hidden_album(rit->value);
                else if (rit->category == "title")  hidden_now = db_.is_hidden_title(rit->value);
                else if (rit->category == "hash") {
                    Hash256 hh{};
                    hidden_now = parse_hex_arr<32>(rit->value, hh) &&
                                 db_.is_song_deleted(hh);
                }
                h["currently_hidden"] = hidden_now;
                results.push_back(std::move(h));
            }
        }
    }

    if (results.size() > limit) {
        json trimmed = json::array();
        for (size_t i = 0; i < limit; ++i) trimmed.push_back(results[i]);
        results = std::move(trimmed);
    }
    json out{{"q", q}, {"results", std::move(results)}};
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::stats_overview(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    uint32_t days = 30;
    if (in.contains("days") && in["days"].is_number())
        days = in["days"].get<uint32_t>();
    if (days < 1)   days = 1;
    if (days > 365) days = 365;
    uint32_t since_day = since_day_of(in);
    if (!since_day && !daily_.empty()) {
        const uint32_t last = daily_.rbegin()->first;
        since_day = last >= days ? last - days + 1 : 0;
    }

    json series = json::array();
    uint64_t plays_since = 0, minted_since = 0;
    for (const auto& [day, pm] : daily_) {
        if (day < since_day) continue;
        plays_since  += pm.first;
        minted_since += pm.second;
        series.push_back({{"date", day_str(day)},
                          {"plays", pm.first},
                          {"minted", pm.second},
                          {"minted_formatted", Ledger::format_balance(pm.second)}});
    }

    json types = json::object();
    for (const auto& [t, n] : tx_type_counts_) types[tx_type_name(t)] = n;

    json genres = json::array();
    for (const auto& [g, n] : genre_counts_)
        genres.push_back({{"name", g}, {"songs", n}});

    const uint64_t supply = db_.get_total_supply();
    const auto tip = chain_.tip();

    json out{
        {"height",                 tip.height},
        {"tip_hash",               hexs(tip.hash)},
        {"tip_weight",             tip.weight},
        {"total_blocks",           blocks_.size()},
        {"total_txs",              total_txs_},
        {"tx_type_counts",         std::move(types)},
        {"total_plays",            total_plays_},
        {"unique_listeners",       uniq_listeners_.size()},
        {"unique_artists",         uniq_artists_.size()},
        {"unique_seeders",         uniq_seeders_.size()},
        {"unique_relays",          uniq_relays_.size()},
        {"total_minted",           total_minted_},
        {"total_minted_formatted", Ledger::format_balance(total_minted_)},
        {"total_burned",           total_burned_},
        {"total_burned_formatted", Ledger::format_balance(total_burned_)},
        {"total_supply",           supply},
        {"total_supply_formatted", Ledger::format_balance(supply)},
        {"songs_registered",       songs_.size()},
        {"unique_artist_names",    artists_by_name_.size()},
        {"moderation", {
            {"actions_total",     mod_actions_.size()},
            {"hides",             mod_hides_},
            {"unhides",           mod_unhides_},
            {"grants",            mod_grants_},
            {"revokes",           mod_revokes_},
            {"label_edits",       mod_label_edits_},
            {"active_moderators", db_.list_active_moderators().size()},
            {"hidden_artists",    db_.list_hidden_artists().size()},
            {"hidden_albums",     db_.list_hidden_albums().size()},
            {"hidden_titles",     db_.list_hidden_titles().size()},
        }},
        {"genres",                 std::move(genres)},
        {"since",                  day_str(since_day)},
        {"plays_since",            plays_since},
        {"minted_since",           minted_since},
        {"recent_activity",        std::move(series)},
        // live index footprint, for capacity planning
        {"index", {
            {"blocks",    blocks_.size()},
            {"txs",       tx_loc_.size()},
            {"addresses", addr_.size()},
            {"songs",     songs_.size()},
            {"artists",   artists_.size()},
            {"indexed_height", indexed_height_},
        }},
    };
    return {200, out.dump()};
}

// ---- artist stats ----------------------------------------------------
//
// Two scopes, because on this chain one uploader WALLET can register songs
// under many artist NAMES:
//   * scope "address" — wallet view: everything that wallet's artist_address
//     appears on, across all its names.
//   * scope "name"    — catalog view: only songs registered under that name
//     (what "stats for Surfbort" means to a label).
// {artist_address} or a 40-hex {artist} selects the address scope; a textual
// {artist} selects the name scope (exact lowercase match, else a UNIQUE
// substring match; an ambiguous substring returns 404 with the candidates).

json ExplorerIndex::artist_agg_json_locked(const ArtistAgg& ar,
                                           const std::string& scope) const {
    // Per-song rows, plays-sorted.
    std::vector<std::pair<Hash256, uint64_t>> per_song(ar.plays_per_song.begin(),
                                                       ar.plays_per_song.end());
    std::sort(per_song.begin(), per_song.end(),
              [](const auto& x, const auto& y) { return x.second > y.second; });
    json songs = json::array();
    for (const auto& [ch, plays] : per_song) {
        json row{{"content_hash", hexs(ch)}, {"plays", plays}};
        if (auto s = songs_.find(ch); s != songs_.end()) {
            row["title"]             = s->second.title;
            row["artist"]            = s->second.artist_name;
            row["unique_listeners"]  = s->second.listeners.size();
            row["earned"]            = s->second.artist_earned;
            row["registered_height"] = s->second.reg_height;
        }
        songs.push_back(std::move(row));
    }

    json over_time = json::array();
    for (const auto& [day, n] : ar.plays_by_day)
        over_time.push_back({{"date", day_str(day)}, {"plays", n}});

    auto counts_arr = [](const std::unordered_map<Address, uint64_t, H20>& m) {
        std::vector<std::pair<Address, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& x, const auto& y) { return x.second > y.second; });
        json out = json::array();
        for (const auto& [addr, n] : v)
            out.push_back({{"address", hexa(addr)}, {"plays", n}});
        return out;
    };

    json blocks = json::array();
    for (uint32_t h : ar.blocks) blocks.push_back(h);

    json out{
        {"scope",                  scope},
        {"name",                   ar.name},
        {"total_plays",            ar.plays},
        {"unique_listeners",       ar.listeners.size()},
        {"earned_total",           ar.earned},
        {"earned_total_formatted", Ledger::format_balance(ar.earned)},
        {"songs",                  std::move(songs)},
        {"top_songs",              json::array()},
        {"plays_over_time",        std::move(over_time)},
        {"seeders",                counts_arr(ar.seeders)},
        {"relays",                 counts_arr(ar.relays)},
        {"blocks",                 std::move(blocks)},
    };
    return out;
}

ExplorerIndex::Result ExplorerIndex::stats_artist(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    // ---- resolve scope ----
    Address a{};
    bool addr_mode = false;
    std::string name_key;
    if (in.contains("artist_address") && in["artist_address"].is_string() &&
        parse_hex_arr<20>(in["artist_address"].get<std::string>(), a)) {
        addr_mode = true;
    } else if (in.contains("artist") && in["artist"].is_string()) {
        const std::string raw = in["artist"].get<std::string>();
        if (parse_hex_arr<20>(raw, a)) {
            addr_mode = true;
        } else {
            const std::string want = lc(raw);
            if (artists_by_name_.count(want)) {
                name_key = want;
            } else {
                std::vector<std::string> hits;
                for (const auto& [n, agg] : artists_by_name_)
                    if (n.find(want) != std::string::npos) hits.push_back(n);
                if (hits.size() == 1) name_key = hits[0];
                else if (hits.size() > 1) {
                    json cand = json::array();
                    for (const auto& n : hits) cand.push_back(n);
                    json e{{"error", "ambiguous artist name"},
                           {"candidates", std::move(cand)}};
                    return {404, e.dump()};
                }
            }
        }
    }
    if (!addr_mode && name_key.empty())
        return {404, err_body("artist not found (give artist_address or a "
                              "unique artist name)").dump()};

    json out;
    Address primary{};   // wallet whose balance/escrow we show
    if (addr_mode) {
        auto it = artists_.find(a);
        if (it == artists_.end())
            return {404, err_body("no on-chain activity for this artist "
                                  "address").dump()};
        const ArtistAgg& ar = it->second;
        out     = artist_agg_json_locked(ar, "address");
        primary = a;
        json names = json::array();
        for (const auto& n : ar.names) names.push_back(n);
        out["names"] = std::move(names);
        // Zero-play songs this wallet registered.
        for (const auto& [ch, sa] : songs_) {
            if (sa.artist_address != a || ar.plays_per_song.count(ch)) continue;
            out["songs"].push_back({{"content_hash", hexs(ch)},
                                    {"plays", 0},
                                    {"title", sa.title},
                                    {"artist", sa.artist_name},
                                    {"unique_listeners", 0},
                                    {"earned", sa.artist_earned},
                                    {"registered_height", sa.reg_height}});
        }
    } else {
        const ArtistAgg& ar = artists_by_name_.at(name_key);
        out = artist_agg_json_locked(ar, "name");
        json addrs = json::array();
        for (const auto& ad : ar.addresses) addrs.push_back(hexa(ad));
        out["addresses"] = std::move(addrs);
        if (auto pit = artist_by_name_.find(name_key); pit != artist_by_name_.end())
            primary = pit->second;
        for (const auto& [ch, sa] : songs_) {
            if (lc(sa.artist_name) != name_key || ar.plays_per_song.count(ch))
                continue;
            out["songs"].push_back({{"content_hash", hexs(ch)},
                                    {"plays", 0},
                                    {"title", sa.title},
                                    {"artist", sa.artist_name},
                                    {"unique_listeners", 0},
                                    {"earned", sa.artist_earned},
                                    {"registered_height", sa.reg_height}});
        }
    }

    out["artist_address"] = hexa(primary);
    out["balance"]        = db_.get_balance(primary);
    out["escrow_balance"] = db_.get_balance(crypto::escrow_address_for(primary));
    // top_songs = first 10 rows of the (plays-sorted) songs list.
    for (size_t i = 0; i < out["songs"].size() && i < 10; ++i)
        out["top_songs"].push_back(out["songs"][i]);
    return {200, out.dump()};
}


ExplorerIndex::Result ExplorerIndex::stats_top(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    const std::string kind = lc(in.value("kind", std::string("songs")));
    size_t n = 10;
    if (in.contains("n") && in["n"].is_number()) n = in["n"].get<size_t>();
    if (n < 1)   n = 1;
    if (n > 100) n = 100;
    const uint32_t since_day = since_day_of(in);

    auto plays_since = [&](const std::map<uint32_t, uint64_t>& by_day,
                           uint64_t all) -> uint64_t {
        if (!since_day) return all;
        uint64_t s = 0;
        for (auto it = by_day.lower_bound(since_day); it != by_day.end(); ++it)
            s += it->second;
        return s;
    };

    json rows = json::array();
    if (kind == "songs") {
        std::vector<std::pair<const Hash256*, uint64_t>> v;
        for (const auto& [ch, sa] : songs_) {
            const uint64_t p = plays_since(sa.plays_by_day, sa.plays);
            if (p) v.push_back({&ch, p});
        }
        std::sort(v.begin(), v.end(),
                  [](const auto& x, const auto& y) { return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < n; ++i) {
            const SongAgg& sa = songs_.at(*v[i].first);
            rows.push_back({{"content_hash", hexs(*v[i].first)},
                            {"title", sa.title},
                            {"artist", sa.artist_name},
                            {"artist_address", hexa(sa.artist_address)},
                            {"plays", v[i].second},
                            {"unique_listeners", sa.listeners.size()}});
        }
    } else if (kind == "artists") {
        // NAME-keyed: one uploader wallet can register many artist names, so
        // ranking by address would collapse the whole catalog into one row.
        std::vector<std::pair<const std::string*, uint64_t>> v;
        for (const auto& [nm, ar] : artists_by_name_) {
            const uint64_t p = plays_since(ar.plays_by_day, ar.plays);
            if (p) v.push_back({&nm, p});
        }
        std::sort(v.begin(), v.end(),
                  [](const auto& x, const auto& y) { return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < n; ++i) {
            const ArtistAgg& ar = artists_by_name_.at(*v[i].first);
            auto addr_it = artist_by_name_.find(*v[i].first);
            rows.push_back({{"address", addr_it != artist_by_name_.end()
                                            ? hexa(addr_it->second) : std::string()},
                            {"name", ar.name},
                            {"plays", v[i].second},
                            {"unique_listeners", ar.listeners.size()},
                            {"earned", ar.earned},
                            {"earned_formatted", Ledger::format_balance(ar.earned)}});
        }
    } else if (kind == "listeners") {
        std::vector<std::pair<const Address*, uint64_t>> v;
        for (const auto& [a, ai] : addr_) {
            uint64_t p = ai.plays_listener;
            if (since_day) {
                p = 0;
                for (auto it = ai.listen_days.lower_bound(since_day);
                     it != ai.listen_days.end(); ++it)
                    p += it->second;
            }
            if (p) v.push_back({&a, p});
        }
        std::sort(v.begin(), v.end(),
                  [](const auto& x, const auto& y) { return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < n; ++i) {
            const AddrInfo& ai = addr_.at(*v[i].first);
            rows.push_back({{"address", hexa(*v[i].first)},
                            {"plays", v[i].second},
                            {"burned", ai.burned},
                            {"earned_listener", ai.earned_listener}});
        }
    } else {
        return {400, err_body("kind must be songs|artists|listeners").dump()};
    }

    json out{{"kind", kind}, {"n", n}, {"rows", std::move(rows)}};
    if (since_day) out["since"] = day_str(since_day);
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::song_detail(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Hash256 ch{};
    if (!in.contains("content_hash") || !in["content_hash"].is_string() ||
        !parse_hex_arr<32>(in["content_hash"].get<std::string>(), ch))
        return {400, err_body("need 64-hex content_hash").dump()};

    // Authoritative full SongSection from the canonical block store —
    // includes compressed_fingerprint, artist_address, royalty_splits.
    auto sec = db_.get_song_section(ch);
    auto it  = songs_.find(ch);
    if (!sec && it == songs_.end())
        return {404, err_body("song not on chain").dump()};

    json out{{"content_hash", hexs(ch)}};
    out["song"] = sec ? song_section_to_json(*sec) : json(nullptr);

    if (it != songs_.end()) {
        const SongAgg& sa = it->second;
        out["registration_height"]     = sa.reg_height;
        out["registration_block_hash"] = hexs(sa.reg_block_hash);
        out["play_count"]              = sa.plays;
        out["unique_listeners"]        = sa.listeners.size();
        out["earned_total"]            = sa.artist_earned;
        out["earned_total_formatted"]  = Ledger::format_balance(sa.artist_earned);
        json ot = json::array();
        for (const auto& [day, npl] : sa.plays_by_day)
            ot.push_back({{"date", day_str(day)}, {"plays", npl}});
        out["plays_over_time"] = std::move(ot);
    } else {
        out["registration_height"] = db_.get_content_height(ch)
                                         ? json(*db_.get_content_height(ch))
                                         : json(nullptr);
        out["play_count"]       = 0;
        out["unique_listeners"] = 0;
        out["earned_total"]     = 0;
        out["plays_over_time"]  = json::array();
    }

    // Cross-check against the chain's committed SongState.
    const SongState st = db_.get_song_state(ch);
    out["state"] = json{
        {"play_count",           st.play_count},
        {"discoverer_address",   hexa(st.discoverer_address)},
        {"first_play_block",     st.first_play_block},
        {"first_play_timestamp", st.first_play_timestamp},
    };
    const auto rc = db_.get_rating_counts(ch);
    const auto rp = db_.get_rating_policy();
    out["ratings"] = json{
        {"up",    rc.up},
        {"down",  rc.down},
        {"total", rc.up + rc.down},
        {"score", static_cast<int64_t>(rc.up) - static_cast<int64_t>(rc.down)},
        // The recorded auto-hide event, if any (null otherwise), plus whether a
        // moderator has since reviewed and cleared it.
        {"auto_hide", [&]() -> json {
            auto rh = db_.get_rating_hide(ch);
            if (!rh) return json(nullptr);
            return json{{"height", rh->height},
                        {"up", rh->up}, {"down", rh->down},
                        {"trigger_tx", crypto::to_hex(rh->trigger_tx)},
                        {"threshold_in_force",
                            {{"min_ratings", rh->min_ratings},
                             {"down_ratio_bps", rh->down_ratio_bps},
                             {"set_height", rh->policy_set_height}}}};
        }()},
        {"moderator_unhidden", db_.is_rating_hide_exempt(ch)},
        {"threshold_in_force", {{"min_ratings",    rp.min_ratings},
                                {"down_ratio_bps", rp.down_ratio_bps},
                                {"source",         rp.set_height ? "chain" : "default"},
                                {"set_height",     rp.set_height},
                                {"set_by",         rp.set_height
                                                       ? crypto::to_hex(rp.set_by.data(), 20)
                                                       : std::string()}}},
    };
    const bool hidden = db_.is_song_deleted(ch);
    out["hidden"] = hidden;
    if (hidden)
        out["hidden_provenance"] =
            provenance_json_locked("hash", crypto::to_hex(ch));
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::song_plays(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Hash256 ch{};
    if (!in.contains("content_hash") || !in["content_hash"].is_string() ||
        !parse_hex_arr<32>(in["content_hash"].get<std::string>(), ch))
        return {400, err_body("need 64-hex content_hash").dump()};
    auto it = songs_.find(ch);
    if (it == songs_.end()) return {404, err_body("song not on chain").dump()};
    const SongAgg& sa = it->second;

    size_t offset = 0, limit = 25;
    if (in.contains("offset") && in["offset"].is_number()) offset = in["offset"].get<size_t>();
    if (in.contains("limit")  && in["limit"].is_number())  limit  = in["limit"].get<size_t>();
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;

    json rows = json::array();
    // Newest first; cache the last cracked block + settlement body.
    uint32_t cached_h = UINT32_MAX;
    Block    cached_b;
    bool     cached_ok = false;
    Hash256  cached_root{};
    std::vector<PlayProof> cached_body;
    size_t i = 0;
    for (auto rit = sa.play_refs.rbegin(); rit != sa.play_refs.rend(); ++rit, ++i) {
        if (i < offset || rows.size() >= limit) continue;
        auto brow = blocks_.find(rit->height);
        if (brow == blocks_.end()) continue;
        if (cached_h != rit->height) {
            cached_ok = false;
            if (auto raw = db_.get("b:" + db_.hex(brow->second.hash)))
                cached_ok = Block::deserialize(raw->data(), raw->size(), cached_b);
            cached_h = rit->height;
        }
        if (!cached_ok || rit->index >= cached_b.transactions.size()) continue;
        const auto& rawtx = cached_b.transactions[rit->index];
        json row{{"block_height", rit->height},
                 {"block_hash",   hexs(brow->second.hash)},
                 {"timestamp_ms", brow->second.timestamp_ms},
                 {"tx_hash", crypto::to_hex(crypto::sha256(rawtx.data(), rawtx.size()))}};
        const PlayProof* p = nullptr;
        MintTx m;
        if (rit->sub == UINT32_MAX) {
            row["settlement"] = false;
            if (!rawtx.empty() && static_cast<TxType>(rawtx[0]) == TxType::MINT &&
                MintTx::deserialize(rawtx.data(), rawtx.size(), m))
                p = &m.proof;
        } else {
            row["settlement"] = true;
            SettlementMintTx sm;
            if (!rawtx.empty() &&
                static_cast<TxType>(rawtx[0]) == TxType::SETTLEMENT_MINT &&
                SettlementMintTx::deserialize(rawtx.data(), rawtx.size(), sm)) {
                if (cached_root != sm.constituents_merkle_root) {
                    cached_body.clear();
                    if (auto braw = db_.get(
                            "sb:" + crypto::to_hex(sm.constituents_merkle_root)))
                        deserialize_settle_body(braw->data(), braw->size(),
                                                cached_body);
                    cached_root = sm.constituents_merkle_root;
                }
                if (rit->sub < cached_body.size()) p = &cached_body[rit->sub];
            }
        }
        if (p) {
            row["listener"]    = hexa(p->player_address);
            row["seeder"]      = hexa(p->seeder_address);
            row["relay"]       = hexa(p->mini_node_address);
            row["duration_ms"] = p->total_duration_ms;
            row["play_start_timestamp"] = p->play_start_timestamp;
            row["play_end_timestamp"]   = p->play_end_timestamp;
        }
        rows.push_back(std::move(row));
    }
    json out{
        {"content_hash", hexs(ch)},
        {"title",        sa.title},
        {"artist",       sa.artist_name},
        {"total",        sa.play_refs.size()},
        {"offset",       offset},
        {"limit",        limit},
        {"plays",        std::move(rows)},
    };
    return {200, out.dump()};
}

// ---------------------------------------------------------------------
// moderation (public sources only: block txs + the replicated ml: log)
// ---------------------------------------------------------------------

json ExplorerIndex::proposal_json_locked(const ProposalInfo& p) const {
    json votes = json::array();
    for (const auto& v : p.votes)
        votes.push_back({{"voter", hexa(v.voter)},
                         {"height", v.height},
                         {"tx_hash", v.tx_hash}});
    // Executed? Authoritative public flag (propstatus:), not replayed quorum
    // math. The chain executes atomically in the block that reaches quorum,
    // so when executed the deciding block is the LAST vote's block (or the
    // proposal's own block if the proposer alone was quorum).
    Hash256 ph{};
    const bool have_ph = parse_hex_arr<32>(p.tx_hash, ph);
    const bool executed =
        have_ph && db_.get_proposal_status(ph) == Database::PROP_EXECUTED;
    const size_t active_now = db_.list_active_moderators().size();
    json j{
        {"proposal_tx_hash", p.tx_hash},
        {"kind",             p.kind},
        {"kind_name",        p.kind == 1 ? "hide_content"
                           : p.kind == 2 ? "release_escrow"
                           : p.kind == 4 ? "grant_moderator" : "?"},
        {"proposer",         hexa(p.proposer)},
        {"proposed_height",  p.height},
        {"timestamp_ms",     p.ts_ms},
        {"yes_votes",        have_ph ? db_.count_proposal_votes(ph)
                                     : p.votes.size() + 1},
        {"voters",           std::move(votes)},
        {"executed",         executed},
        // Historical moderator-set size at execution is not recorded on
        // chain; expose the CURRENT set + threshold, clearly labeled "now".
        {"active_moderators_now", active_now},
        {"threshold_now",         active_now / 2 + 1},
    };
    if (p.kind == 1) j["target_content_hash"] = hexs(p.target_hash);
    if (p.kind == 2 || p.kind == 4) j["target_address"] = hexa(p.target_addr);
    if (executed)
        j["executed_height"] = p.votes.empty() ? p.height
                                               : p.votes.back().height;
    return j;
}

// Provenance for one currently-hidden item. IMPORTANT honesty rule: the
// on-chain envelope carries NO reason field — a DMCA-approved hide is
// byte-identical to a manual one — so `reason` is always "unrecorded".
json ExplorerIndex::provenance_json_locked(const std::string& category,
                                           const std::string& value) const {
    const std::string vlc = lc(value);
    // Latest matching gossip HIDE envelope.
    const ModAction* hide = nullptr;
    for (auto rit = mod_actions_.rbegin(); rit != mod_actions_.rend(); ++rit) {
        if (rit->source != "gossip" || rit->kind != "hide") continue;
        if (rit->category != category || lc(rit->value) != vlc) continue;
        hide = &*rit;
        break;
    }
    json j{{"reason", "unrecorded"}};
    if (hide) {
        const auto founder = db_.get_founder();
        const bool is_founder = founder && *founder == hide->moderator;
        j["by"]           = is_founder ? "founder" : "moderator";
        j["moderator"]    = hexa(hide->moderator);
        j["mod_level_now"] = db_.get_mod_level(hide->moderator);
        j["ts_ms"]        = hide->ts_ms;
        j["height"]       = hide->height;
        j["sig"]          = hide->sig_hex;
        return j;
    }
    if (category == "hash") {
        Hash256 ch{};
        if (parse_hex_arr<32>(value, ch)) {
            // Executed HIDE_CONTENT proposal targeting this content hash?
            if (auto tit = proposals_by_target_.find(ch);
                tit != proposals_by_target_.end()) {
                for (auto pit = tit->second.rbegin();
                     pit != tit->second.rend(); ++pit) {
                    auto pr = proposals_.find(*pit);
                    if (pr == proposals_.end()) continue;
                    if (db_.get_proposal_status(*pit) != Database::PROP_EXECUTED)
                        continue;
                    j["by"]       = "vote";
                    j["proposal"] = proposal_json_locked(pr->second);
                    return j;
                }
            }
            // Rating-driven auto-hide — the third hide source. The record is
            // written by consensus at the moment the rule fired and freezes
            // BOTH the triggering counts and the threshold that was in force,
            // so a later SET_RATING_THRESHOLD can never make this hide
            // unexplainable. No signer: `by` is "ratings", not a person.
            if (auto rh = db_.get_rating_hide(ch)) {
                char pct[16];
                std::snprintf(pct, sizeof pct, "%.2f", rh->down_ratio_bps / 100.0);
                j["by"]     = "ratings";
                j["height"] = rh->height;
                j["ratings"] = json{{"up", rh->up}, {"down", rh->down},
                                    {"total", rh->up + rh->down}};
                j["trigger_tx"] = crypto::to_hex(rh->trigger_tx);
                j["threshold_in_force"] = json{
                    {"min_ratings",    rh->min_ratings},
                    {"down_ratio_bps", rh->down_ratio_bps},
                    {"down_ratio_pct", pct},
                    {"set_height",     rh->policy_set_height},
                    {"set_by",         rh->policy_set_height
                                           ? crypto::to_hex(rh->policy_set_by.data(), 20)
                                           : std::string()},
                };
                return j;
            }
            // K-independent forgery reports (node attestations, not mods).
            const int reports = db_.forgery_report_count(ch);
            if (reports > 0) {
                j["by"] = "forgery_quorum";
                j["forgery_reports"] = reports;
                return j;
            }
        }
    }
    j["by"] = "unknown";
    return j;
}

json ExplorerIndex::mod_action_json_locked(const ModAction& a) const {
    json j{
        {"kind",      a.kind},
        {"category",  a.category},
        {"value",     a.value},
        {"moderator", hexa(a.moderator)},
        {"ts_ms",     a.ts_ms},
        {"height",    a.height},
        {"source",    a.source},
    };
    // Signer classification (founder / moderator / former-moderator). Level
    // is the signer's CURRENT level — historical level at signing is not
    // recorded on chain.
    const auto founder = db_.get_founder();
    j["signer_is_founder"]  = founder && *founder == a.moderator;
    j["signer_mod_level_now"] = db_.get_mod_level(a.moderator);
    if (!a.tx_hash.empty()) j["tx_hash"] = a.tx_hash;
    if (!a.sig_hex.empty()) j["sig"]     = a.sig_hex;
    if (a.level)            j["level"]   = a.level;
    j["id"] = !a.tx_hash.empty() ? a.tx_hash : a.sig_hex;
    // Vote provenance ride-along for proposal rows.
    if (a.source == "block" &&
        (a.kind == "proposal_hide" || a.kind == "proposal_release" ||
         a.kind == "proposal_grant" || a.kind == "proposal_rating_threshold")) {
        Hash256 ph{};
        if (parse_hex_arr<32>(a.tx_hash, ph)) {
            if (auto pit = proposals_.find(ph); pit != proposals_.end())
                j["proposal"] = proposal_json_locked(pit->second);
        }
    }
    return j;
}

ExplorerIndex::Result ExplorerIndex::moderation_list(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    size_t offset = 0, limit = 25;
    if (in.contains("offset") && in["offset"].is_number()) offset = in["offset"].get<size_t>();
    if (in.contains("limit")  && in["limit"].is_number())  limit  = in["limit"].get<size_t>();
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;

    json actions = json::array();
    size_t i = 0;
    for (auto rit = mod_actions_.rbegin(); rit != mod_actions_.rend(); ++rit, ++i) {
        if (i < offset || actions.size() >= limit) continue;
        actions.push_back(mod_action_json_locked(*rit));
    }
    json out{
        {"total",   mod_actions_.size()},
        {"offset",  offset},
        {"limit",   limit},
        {"actions", std::move(actions)},
        {"counts", {
            {"hides",       mod_hides_},
            {"unhides",     mod_unhides_},
            {"grants",      mod_grants_},
            {"revokes",     mod_revokes_},
            {"label_edits", mod_label_edits_},
        }},
        {"active_moderators", db_.list_active_moderators().size()},
    };
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::moderation_hidden(const json&) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    auto rows_for = [&](const std::vector<std::string>& values,
                        const char* category) {
        json rows = json::array();
        for (const auto& v : values) {
            json r{{"value", v}, {"category", category}};
            r["provenance"] = provenance_json_locked(category, v);
            rows.push_back(std::move(r));
        }
        return rows;
    };

    // Hidden content hashes: the d: table has no list call, but every song
    // the chain knows is in songs_ — check each.
    json hashes = json::array();
    for (const auto& [ch, sa] : songs_) {
        if (!db_.is_song_deleted(ch)) continue;
        json r{{"value", hexs(ch)},
               {"category", "hash"},
               {"title",  sa.title},
               {"artist", sa.artist_name}};
        r["provenance"] = provenance_json_locked("hash", hexs(ch));
        hashes.push_back(std::move(r));
    }

    json out{
        {"artists", rows_for(db_.list_hidden_artists(), "artist")},
        {"albums",  rows_for(db_.list_hidden_albums(),  "album")},
        {"titles",  rows_for(db_.list_hidden_titles(),  "title")},
        {"hashes",  std::move(hashes)},
    };
    out["counts"] = json{
        {"artists", out["artists"].size()},
        {"albums",  out["albums"].size()},
        {"titles",  out["titles"].size()},
        {"hashes",  out["hashes"].size()},
    };
    return {200, out.dump()};
}

ExplorerIndex::Result ExplorerIndex::moderation_moderator(const json& in) {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_synced_locked();

    Address a{};
    if (!in.contains("address") || !in["address"].is_string() ||
        !parse_hex_arr<20>(in["address"].get<std::string>(), a))
        return {400, err_body("need 40-hex address").dump()};

    size_t offset = 0, limit = 50;
    if (in.contains("offset") && in["offset"].is_number()) offset = in["offset"].get<size_t>();
    if (in.contains("limit")  && in["limit"].is_number())  limit  = in["limit"].get<size_t>();
    if (limit < 1)   limit = 1;
    if (limit > 200) limit = 200;

    json actions = json::array();
    size_t matched = 0;
    for (auto rit = mod_actions_.rbegin(); rit != mod_actions_.rend(); ++rit) {
        if (!(rit->moderator == a)) continue;
        if (matched >= offset && actions.size() < limit)
            actions.push_back(mod_action_json_locked(*rit));
        ++matched;
    }
    const auto founder = db_.get_founder();
    json out{
        {"address",      hexa(a)},
        {"is_moderator", db_.is_moderator(a)},
        {"is_founder",   founder && *founder == a},
        {"mod_level",    db_.get_mod_level(a)},
        {"total",        matched},
        {"offset",       offset},
        {"limit",        limit},
        {"actions",      std::move(actions)},
    };
    if (auto ab = db_.get_mod_active_block(a)) out["active_since_height"] = *ab;
    return {200, out.dump()};
}

} // namespace mc::api
