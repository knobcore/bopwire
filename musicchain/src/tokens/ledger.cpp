#include "ledger.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <limits>

namespace mc {

uint64_t compute_burn_rate(uint64_t total_supply) {
    if (total_supply < SUPPLY_FLOOR) return 0;
    if (total_supply >= SUPPLY_CAP)
        return std::numeric_limits<uint64_t>::max();
    const double range = static_cast<double>(SUPPLY_CAP - SUPPLY_FLOOR);
    const double pct   = static_cast<double>(total_supply - SUPPLY_FLOOR) / range;
    // Cubic ramp; multiplier 1000 tokens at pct=1.0 (which never gets
    // reached — the cap branch above intercepts first).
    const double burn  = pct * pct * pct
                       * 1000.0 * static_cast<double>(TOKEN_DECIMALS);
    return static_cast<uint64_t>(burn);
}


Ledger::Ledger(Database& db) : db_(db) {}

void Ledger::credit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    uint64_t bal = db_.get_balance(addr);
    bal += amount;
    db_.set_balance(batch, addr, bal);
    uint64_t supply = db_.get_total_supply();
    db_.set_total_supply(batch, supply + amount);
}

bool Ledger::debit(leveldb::WriteBatch& batch, const Address& addr, uint64_t amount) {
    uint64_t bal = db_.get_balance(addr);
    if (bal < amount) return false;
    db_.set_balance(batch, addr, bal - amount);
    return true;
}

bool Ledger::transfer(leveldb::WriteBatch& batch,
                       const Address& from, const Address& to, uint64_t amount) {
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
