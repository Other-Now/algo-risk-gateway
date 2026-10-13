#pragma once

// Risk limits. A plain fixed-size POD on purpose: the raw bytes are written
// into the audit-log header and hashed into the chain seed, so the replay tool
// rebuilds the gate with *exactly* the limits that produced the log -- no
// separate config file to drift out of sync.
//
// Always build one through make_config() / zeroed(): the struct is memset to
// zero first so that any padding bytes are deterministic.

#include <cstdint>
#include <cstring>

#include "arg/types.hpp"

namespace arg {

constexpr int kMaxInstr = 32;
constexpr int kMaxAlgo = 32;

struct InstrCfg {
    std::uint8_t enabled;
    char symbol[8];           // space-padded, goes on the wire
    Price tick;               // price must be a multiple of this
    Price band_lo, band_hi;   // daily price range (e.g. prev close +/- circuit %)
    Price ref_price;          // previous close; LTP until the first trade prints
    std::uint32_t ltp_band_bp;  // max deviation from LTP, basis points
    Qty max_qty;              // per order
    std::int64_t max_value;   // per order, price * qty
    Qty max_pos;              // |position| limit, open orders counted at worst case
};

struct AlgoCfg {
    std::uint8_t enabled;
    std::uint8_t registered;         // has an exchange-issued algo id
    std::uint32_t exchange_algo_id;  // what goes on the wire for a registered algo
    std::uint32_t rate_per_sec;      // token bucket refill
    std::uint32_t burst;             // token bucket depth
};

struct OtrCfg {
    std::uint32_t min_orders;  // OTR is not meaningful before this many order messages
    std::uint32_t throttle;    // OTR >= this: each new order costs 2 tokens
    std::uint32_t breach;      // OTR >= this: algo is killed
};

struct Config {
    std::uint32_t version;
    InstrCfg instr[kMaxInstr];
    AlgoCfg algo[kMaxAlgo];
    std::uint32_t unregistered_ops_cap;  // SEBI: 10 orders/sec/exchange without registration
    OtrCfg otr;
    std::int64_t loss_limit;             // global; kill when mark-to-market PnL < -loss_limit
    std::uint64_t heartbeat_timeout_ns;  // kill when the feed is silent this long
    std::uint8_t flatten_on_auto_kill;   // flatten positions on loss/heartbeat kills
};

inline Config zeroed() {
    Config c;
    std::memset(&c, 0, sizeof c);
    c.version = 1;
    return c;
}

inline void set_symbol(InstrCfg& ic, const char* s) {
    std::memset(ic.symbol, ' ', sizeof ic.symbol);
    for (int i = 0; i < 8 && s[i]; ++i) ic.symbol[i] = s[i];
}

// A daily price range of +/- `circuit_pct` around `ref`, snapped to tick.
inline void set_bands(InstrCfg& ic, Price ref, int circuit_pct) {
    ic.ref_price = ref;
    const Price d = ref * circuit_pct / 100;
    ic.band_lo = ((ref - d) + ic.tick - 1) / ic.tick * ic.tick;
    ic.band_hi = (ref + d) / ic.tick * ic.tick;
}

}  // namespace arg
