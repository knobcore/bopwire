#include "ledger.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <map>
#if defined(_MSC_VER) && defined(_M_X64)
#  include <intrin.h>   // _umul128 / _udiv128 for the deterministic integer cubic
#endif

namespace mc {

namespace {
// Deterministic (a*b)/c with a full 128-bit intermediate and NO floating point,
// so the result is bit-identical on every compiler / arch / -O level. A float
// cubic (the old implementation) rounds differently across x86 vs ARM and under
// -ffast-math; once a burn amount is committed into block state that divergence
// is a chain split. Every call site below guarantees the true quotient fits in
// u64 and (on the MSVC path) that the product high word is < c, so _udiv128
// cannot fault.
inline uint64_t muldiv_u128(uint64_t a, uint64_t b, uint64_t c) {
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>(static_cast<unsigned __int128>(a) * b / c);
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned long long hi;
    unsigned long long lo = _umul128(a, b, &hi);
    unsigned long long rem;
    return _udiv128(hi, lo, c, &rem);
#else
#  error "compute_burn_rate needs __int128 or MSVC x64 intrinsics for a deterministic integer cubic"
#endif
}
} // namespace

uint64_t compute_burn_rate(uint64_t total_supply) {
    if (total_supply < SUPPLY_FLOOR) return 0;
    if (total_supply >= SUPPLY_CAP)
        return std::numeric_limits<uint64_t>::max();
    // Integer fixed-point cubic ramp identical to the old model
    //   pct  = (supply - FLOOR) / (CAP - FLOOR)  in [0,1)
    //   burn = pct^3 * 1000 tokens
    // computed as num^3 * (1000 * TOKEN_DECIMALS) / den^3 via chained 128-bit
    // muldiv (num^3 overflows even 128 bits, so we divide as we go). den = 1e17
    // < 2^63 and num < den, so each product's high word stays < den. This is a
    // pure function of u64 inputs -> bit-identical on every node (consensus-safe),
    // unlike the previous double cubic.
    const uint64_t num = total_supply - SUPPLY_FLOOR;
    const uint64_t den = SUPPLY_CAP  - SUPPLY_FLOOR;
    const uint64_t m1  = muldiv_u128(num, num, den);          // num^2 / den
    const uint64_t m2  = muldiv_u128(m1,  num, den);          // num^3 / den^2
    return muldiv_u128(m2, 1000ULL * TOKEN_DECIMALS, den);    // * 1000 tokens / den
}


Ledger::Ledger(Database& db) : db_(db) {}

// BUG FIX: the old credit/debit/transfer implementations did
// db_.get_balance(addr) which reads from leveldb IGNORING any pending
// WriteBatch updates. When apply_mint had three outputs to three
// distinct addresses everything was fine, but two outputs to the SAME
// address (or two transfers from the same source in one batch) both
// read the old balance and the LAST batched set overwrote the first —
// silently losing tokens. Same problem for total_supply, where each
// per-output increment read the disk value so a 3-output mint ended
// up with supply = old + last_amount instead of old + sum.
//
// Fix: thread a tiny per-call pending map so balances accumulate
// across credit/debit/transfer calls within one batch, and pull the
// total_supply update out of credit entirely. Callers that need to
// mint multiple outputs in one batch use credit_many() and pass them
// in a single shot.

// Overlay-aware primitives (C2). rd_* return the block-scoped staged value if
// present, else seed it from committed state; wr_* update BOTH the overlay and
// the WriteBatch, so later txs in the same block see earlier ones' effects.
uint64_t Ledger::rd_bal(const Address& a) const {
    auto it = bal_overlay_.find(a);
    if (it != bal_overlay_.end()) return it->second;
    uint64_t v = db_.get_balance(a);
    bal_overlay_[a] = v;
    return v;
}
uint64_t Ledger::rd_supply() const {
    if (!supply_overlay_) supply_overlay_ = db_.get_total_supply();
    return *supply_overlay_;
}
void Ledger::wr_bal(leveldb::WriteBatch& batch, const Address& a, uint64_t v) {
    bal_overlay_[a] = v;
    db_.set_balance(batch, a, v);
}
void Ledger::wr_supply(leveldb::WriteBatch& batch, uint64_t v) {
    supply_overlay_ = v;
    db_.set_total_supply(batch, v);
}

void Ledger::credit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    if (amount == 0) return;
    wr_bal(batch, addr, rd_bal(addr) + amount);
    wr_supply(batch, rd_supply() + amount);
}

void Ledger::credit_many(leveldb::WriteBatch& batch,
                          const std::vector<std::pair<Address, uint64_t>>& outs) {
    // Pre-aggregate per-address amounts so multi-output mints to the
    // same recipient compose correctly.
    std::map<Address, uint64_t, AddressLess> per_addr;
    uint64_t total_minted = 0;
    for (const auto& [addr, amount] : outs) {
        if (amount == 0) continue;
        per_addr[addr] += amount;
        total_minted   += amount;
    }
    for (const auto& [addr, amount] : per_addr)
        wr_bal(batch, addr, rd_bal(addr) + amount);
    if (total_minted > 0)
        wr_supply(batch, rd_supply() + total_minted);
}

bool Ledger::debit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    if (amount == 0) return true;
    uint64_t bal = rd_bal(addr);
    if (bal < amount) return false;
    wr_bal(batch, addr, bal - amount);
    return true;
}

bool Ledger::transfer(leveldb::WriteBatch& batch,
                       const Address& from, const Address& to, uint64_t amount) {
    if (amount == 0) return true;
    if (from == to)  return true;
    uint64_t from_bal = rd_bal(from);
    if (from_bal < amount) return false;
    uint64_t to_bal = rd_bal(to);
    wr_bal(batch, from, from_bal - amount);
    wr_bal(batch, to, to_bal + amount);
    return true;
}

uint64_t Ledger::balance(const Address& addr) const { return rd_bal(addr); }
uint64_t Ledger::total_supply() const { return rd_supply(); }

std::string Ledger::format_balance(uint64_t internal_units) {
    uint64_t whole   = internal_units / TOKEN_DECIMALS;
    uint64_t decimal = internal_units % TOKEN_DECIMALS;
    std::ostringstream oss;
    oss << whole << "." << std::setfill('0') << std::setw(8) << decimal;
    return oss.str();
}

bool Ledger::parse_balance(const std::string& s, uint64_t& out) {
    auto dot = s.find('.');
    std::string whole_str, frac_str;
    if (dot == std::string::npos) {
        whole_str = s;
        frac_str  = "0";
    } else {
        whole_str = s.substr(0, dot);
        frac_str  = s.substr(dot + 1);
        if (frac_str.size() > 8) return false;
        while (frac_str.size() < 8) frac_str += '0';
    }
    try {
        uint64_t whole   = std::stoull(whole_str);
        uint64_t decimal = std::stoull(frac_str);
        out = whole * TOKEN_DECIMALS + decimal;
    } catch (...) { return false; }
    return true;
}

} // namespace mc
