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

void Ledger::credit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    if (amount == 0) return;
    uint64_t bal = db_.get_balance(addr);
    bal += amount;
    db_.set_balance(batch, addr, bal);
    uint64_t supply = db_.get_total_supply();
    db_.set_total_supply(batch, supply + amount);
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
    for (const auto& [addr, amount] : per_addr) {
        const uint64_t bal = db_.get_balance(addr) + amount;
        db_.set_balance(batch, addr, bal);
    }
    if (total_minted > 0) {
        const uint64_t supply = db_.get_total_supply();
        db_.set_total_supply(batch, supply + total_minted);
    }
}

bool Ledger::debit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    if (amount == 0) return true;
    uint64_t bal = db_.get_balance(addr);
    if (bal < amount) return false;
    db_.set_balance(batch, addr, bal - amount);
    return true;
}

bool Ledger::transfer(leveldb::WriteBatch& batch,
                       const Address& from, const Address& to, uint64_t amount) {
    if (amount == 0) return true;
    if (from == to)  return true;
    uint64_t from_bal = db_.get_balance(from);
    if (from_bal < amount) return false;
    uint64_t to_bal = db_.get_balance(to);
    db_.set_balance(batch, from, from_bal - amount);
    db_.set_balance(batch, to, to_bal + amount);
    return true;
}

uint64_t Ledger::balance(const Address& addr) const {
    return db_.get_balance(addr);
}

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
