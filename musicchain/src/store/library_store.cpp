#include "library_store.h"

#include "../storage/database.h"

#include "roaring.hh"           // roaring::Roaring (CRoaring C++ wrapper)

#include <leveldb/write_batch.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::store {

namespace {

// leveldb keyspace (separate from the chain's). 2-char prefixes:
//   Lc<hash:32>    -> id:u32        intern: content_hash -> local song id
//   Lp<wallet:20>  -> ord:u32       wallet -> local ordinal (for reverse roaring)
//   Lw<wallet:20>  -> ver:u64 | roaring(portable)   the library
constexpr char kPfxIntern[]   = "Lc";
constexpr char kPfxWalletOrd[] = "Lp";
constexpr char kPfxLibrary[]  = "Lw";

std::string raw_key(const char* pfx, const uint8_t* data, size_t n) {
    std::string k(pfx);
    k.append(reinterpret_cast<const char*>(data), n);
    return k;
}
std::string hkey(const Hash256& h) {
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}
std::string akey(const Address& a) {
    return std::string(reinterpret_cast<const char*>(a.data()), a.size());
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.resize(4);
    std::memcpy(v.data(), &x, 4);          // host LE; both consensus targets are x64 LE
}
uint32_t get_u32(const std::string& v) {
    uint32_t x = 0;
    if (v.size() >= 4) std::memcpy(&x, v.data(), 4);
    return x;
}

} // namespace

struct LibraryStore::Impl {
    Database* db = nullptr;
    mutable std::mutex mu;

    // intern: content_hash <-> local song id (id == index into id_to_hash)
    std::unordered_map<std::string, uint32_t> hash_to_id;
    std::vector<Hash256>                       id_to_hash;

    // wallet <-> local ordinal (reverse roaring keys are ordinals, not addrs)
    std::unordered_map<std::string, uint32_t>  wallet_to_ord;
    std::vector<Address>                       ord_to_wallet;

    // forward: wallet -> set of song ids + version
    std::unordered_map<std::string, roaring::Roaring> libs;
    std::unordered_map<std::string, uint64_t>          versions;

    // reverse (derived, rebuilt on attach): song id -> set of wallet ordinals
    std::unordered_map<uint32_t, roaring::Roaring>     holders;

    // --- intern helpers (assume mu held) ---
    uint32_t intern_hash(const Hash256& h, leveldb::WriteBatch& b) {
        const std::string k = hkey(h);
        auto it = hash_to_id.find(k);
        if (it != hash_to_id.end()) return it->second;
        const uint32_t id = static_cast<uint32_t>(id_to_hash.size());
        hash_to_id.emplace(k, id);
        id_to_hash.push_back(h);
        std::vector<uint8_t> v; put_u32(v, id);
        db->put_batch(b, raw_key(kPfxIntern, h.data(), h.size()), v);
        return id;
    }
    // read-only lookup (no assign)
    bool lookup_id(const Hash256& h, uint32_t& out) const {
        auto it = hash_to_id.find(hkey(h));
        if (it == hash_to_id.end()) return false;
        out = it->second;
        return true;
    }
    uint32_t intern_wallet(const Address& w, leveldb::WriteBatch& b) {
        const std::string k = akey(w);
        auto it = wallet_to_ord.find(k);
        if (it != wallet_to_ord.end()) return it->second;
        const uint32_t ord = static_cast<uint32_t>(ord_to_wallet.size());
        wallet_to_ord.emplace(k, ord);
        ord_to_wallet.push_back(w);
        std::vector<uint8_t> v; put_u32(v, ord);
        db->put_batch(b, raw_key(kPfxWalletOrd, w.data(), w.size()), v);
        return ord;
    }

    // serialize {version, roaring} into the Lw value (assume mu held)
    std::vector<uint8_t> encode_library(uint64_t version,
                                        const roaring::Roaring& r) const {
        std::vector<uint8_t> out(8);
        std::memcpy(out.data(), &version, 8);                 // host LE
        const size_t rsz = r.getSizeInBytes(true /*portable*/);
        out.resize(8 + rsz);
        r.write(reinterpret_cast<char*>(out.data()) + 8, true);
        return out;
    }
    void persist_library(leveldb::WriteBatch& b, const Address& w,
                         uint64_t version, const roaring::Roaring& r) const {
        const auto v = encode_library(version, r);
        db->put_batch(b, raw_key(kPfxLibrary, w.data(), w.size()), v);
    }
};

LibraryStore::LibraryStore() : p_(std::make_unique<Impl>()) {}
LibraryStore::~LibraryStore() = default;

void LibraryStore::attach(Database& db) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->db = &db;

    // 1. intern table: Lc<hash> -> id
    db.for_each_with_prefix(kPfxIntern, [&](const std::string& key,
                                            const std::string& val) {
        if (key.size() != 2 + 32) return true;
        Hash256 h{}; std::memcpy(h.data(), key.data() + 2, 32);
        const uint32_t id = get_u32(val);
        p_->hash_to_id.emplace(std::string(key.data() + 2, 32), id);
        if (id >= p_->id_to_hash.size()) p_->id_to_hash.resize(id + 1);
        p_->id_to_hash[id] = h;
        return true;
    });
    // 2. wallet ordinals: Lp<wallet> -> ord
    db.for_each_with_prefix(kPfxWalletOrd, [&](const std::string& key,
                                               const std::string& val) {
        if (key.size() != 2 + 20) return true;
        Address w{}; std::memcpy(w.data(), key.data() + 2, 20);
        const uint32_t ord = get_u32(val);
        p_->wallet_to_ord.emplace(std::string(key.data() + 2, 20), ord);
        if (ord >= p_->ord_to_wallet.size()) p_->ord_to_wallet.resize(ord + 1);
        p_->ord_to_wallet[ord] = w;
        return true;
    });
    // 3. libraries: Lw<wallet> -> {version, roaring}; rebuild reverse index.
    db.for_each_with_prefix(kPfxLibrary, [&](const std::string& key,
                                             const std::string& val) {
        if (key.size() != 2 + 20 || val.size() < 8) return true;
        const std::string wkey(key.data() + 2, 20);
        uint64_t version = 0; std::memcpy(&version, val.data(), 8);
        roaring::Roaring r =
            roaring::Roaring::readSafe(val.data() + 8, val.size() - 8);
        // wallet ordinal for the reverse index
        auto ow = p_->wallet_to_ord.find(wkey);
        const bool have_ord = ow != p_->wallet_to_ord.end();
        for (uint32_t id : r) {
            if (have_ord) p_->holders[id].add(ow->second);
        }
        p_->versions[wkey] = version;
        p_->libs.emplace(wkey, std::move(r));
        return true;
    });
}

uint64_t LibraryStore::set_library(const Address& wallet,
                                   const std::vector<Hash256>& hashes) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return 0;
    const std::string wkey = akey(wallet);
    leveldb::WriteBatch batch;
    const uint32_t ord = p_->intern_wallet(wallet, batch);

    // Build the new id set.
    roaring::Roaring next;
    for (const auto& h : hashes) next.add(p_->intern_hash(h, batch));

    // Reverse-index diff: drop ords for removed ids, add for new ones.
    auto it = p_->libs.find(wkey);
    if (it != p_->libs.end()) {
        for (uint32_t id : it->second)
            if (!next.contains(id)) p_->holders[id].remove(ord);
    }
    for (uint32_t id : next) p_->holders[id].add(ord);

    const uint64_t version = p_->versions[wkey] + 1;
    p_->versions[wkey] = version;
    p_->persist_library(batch, wallet, version, next);
    p_->libs[wkey] = std::move(next);
    p_->db->write(batch);
    return version;
}

bool LibraryStore::apply_delta(const Address& wallet,
                               const std::vector<Hash256>& add,
                               const std::vector<Hash256>& remove,
                               uint64_t version) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    const std::string wkey = akey(wallet);
    // Idempotent / monotonic: only newer versions apply (gossip re-delivery,
    // out-of-order arrival, and self-echo are all dropped here).
    auto vit = p_->versions.find(wkey);
    if (vit != p_->versions.end() && version <= vit->second) return false;

    leveldb::WriteBatch batch;
    const uint32_t ord = p_->intern_wallet(wallet, batch);
    roaring::Roaring& r = p_->libs[wkey];   // default-constructs if absent

    for (const auto& h : remove) {
        uint32_t id;
        if (p_->lookup_id(h, id) && r.contains(id)) {
            r.remove(id);
            auto hit = p_->holders.find(id);
            if (hit != p_->holders.end()) hit->second.remove(ord);
        }
    }
    for (const auto& h : add) {
        const uint32_t id = p_->intern_hash(h, batch);
        r.add(id);
        p_->holders[id].add(ord);
    }

    p_->versions[wkey] = version;
    p_->persist_library(batch, wallet, version, r);
    p_->db->write(batch);
    return true;
}

std::vector<Hash256> LibraryStore::library(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Hash256> out;
    auto it = p_->libs.find(akey(wallet));
    if (it == p_->libs.end()) return out;
    out.reserve(it->second.cardinality());
    for (uint32_t id : it->second)
        if (id < p_->id_to_hash.size()) out.push_back(p_->id_to_hash[id]);
    return out;
}

size_t LibraryStore::library_size(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->libs.find(akey(wallet));
    return it == p_->libs.end() ? 0 : static_cast<size_t>(it->second.cardinality());
}

uint64_t LibraryStore::library_version(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->versions.find(akey(wallet));
    return it == p_->versions.end() ? 0 : it->second;
}

std::vector<Address> LibraryStore::holders(const Hash256& ch) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Address> out;
    uint32_t id;
    if (!p_->lookup_id(ch, id)) return out;
    auto it = p_->holders.find(id);
    if (it == p_->holders.end()) return out;
    out.reserve(it->second.cardinality());
    for (uint32_t ord : it->second)
        if (ord < p_->ord_to_wallet.size()) out.push_back(p_->ord_to_wallet[ord]);
    return out;
}

size_t LibraryStore::holder_count(const Hash256& ch) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    uint32_t id;
    if (!p_->lookup_id(ch, id)) return 0;
    auto it = p_->holders.find(id);
    return it == p_->holders.end() ? 0 : static_cast<size_t>(it->second.cardinality());
}

size_t LibraryStore::wallet_count() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->libs.size();
}

size_t LibraryStore::catalog_size() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->id_to_hash.size();
}

} // namespace mc::store
