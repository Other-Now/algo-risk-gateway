#pragma once

// Core vocabulary of the gate. Everything is an integer: prices in paise (or
// any integer tick unit), quantities in shares/lots, time in nanoseconds.
// Floating point never touches a decision -- that is what makes replay
// byte-exact across runs.

#include <cstdint>

namespace arg {

using Price = std::int64_t;
using Qty = std::int64_t;
using Ts = std::uint64_t;  // nanoseconds; supplied by the caller, never read from a clock

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class Tif : std::uint8_t { Day = 0, Ioc = 1 };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// What a strategy hands the gate.
struct NewOrder {
    std::uint64_t order_id;
    std::uint32_t algo_id;  // 0 = untagged
    std::uint16_t instr;
    Side side;
    Tif tif;
    Price price;
    Qty qty;
};

// Execution report from the exchange.
struct Fill {
    std::uint64_t exec_id;
    std::uint64_t order_id;
    std::uint16_t instr;
    Side side;
    Price price;
    Qty qty;
};

// Reason codes. Order matters only for readability; the numeric value is what
// goes into the audit log, so never renumber an existing code.
enum class Reason : std::uint8_t {
    Ok = 0,
    BadInstrument,      // unknown / disabled instrument
    BadOrder,           // qty <= 0, price <= 0, off-tick, bad enum
    DuplicateOrderId,   // id already open
    AlgoIdMissing,      // no algo tag on the order
    AlgoUnknown,        // tag not in the registered-algo table
    KillGlobal,
    KillAlgo,
    KillInstrument,
    PriceBandDaily,     // outside the day's static price range
    PriceBandLtp,       // too far from last traded price
    MaxQty,
    MaxValue,
    PositionLimit,      // worst case (position + all open same-side qty) breaches
    SelfTrade,          // would cross our own resting order
    UnregisteredOps,    // unregistered algo above the per-second threshold
    RateLimit,          // token bucket empty
    OtrThrottle,        // bucket would allow it at normal cost, but OTR doubled the cost
    kCount
};

inline const char* reason_name(Reason r) {
    switch (r) {
        case Reason::Ok: return "OK";
        case Reason::BadInstrument: return "BAD_INSTRUMENT";
        case Reason::BadOrder: return "BAD_ORDER";
        case Reason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
        case Reason::AlgoIdMissing: return "ALGO_ID_MISSING";
        case Reason::AlgoUnknown: return "ALGO_UNKNOWN";
        case Reason::KillGlobal: return "KILL_GLOBAL";
        case Reason::KillAlgo: return "KILL_ALGO";
        case Reason::KillInstrument: return "KILL_INSTRUMENT";
        case Reason::PriceBandDaily: return "PRICE_BAND_DAILY";
        case Reason::PriceBandLtp: return "PRICE_BAND_LTP";
        case Reason::MaxQty: return "MAX_QTY";
        case Reason::MaxValue: return "MAX_VALUE";
        case Reason::PositionLimit: return "POSITION_LIMIT";
        case Reason::SelfTrade: return "SELF_TRADE";
        case Reason::UnregisteredOps: return "UNREGISTERED_OPS";
        case Reason::RateLimit: return "RATE_LIMIT";
        case Reason::OtrThrottle: return "OTR_THROTTLE";
        default: return "?";
    }
}

enum class Scope : std::uint8_t { Global = 0, Algo = 1, Instrument = 2 };

enum class KillWhy : std::uint8_t { Manual = 0, LossLimit = 1, OtrBreach = 2, HeartbeatLost = 3 };

inline const char* kill_why_name(KillWhy w) {
    switch (w) {
        case KillWhy::Manual: return "MANUAL";
        case KillWhy::LossLimit: return "LOSS_LIMIT";
        case KillWhy::OtrBreach: return "OTR_BREACH";
        case KillWhy::HeartbeatLost: return "HEARTBEAT_LOST";
    }
    return "?";
}

inline const char* scope_name(Scope s) {
    switch (s) {
        case Scope::Global: return "GLOBAL";
        case Scope::Algo: return "ALGO";
        case Scope::Instrument: return "INSTRUMENT";
    }
    return "?";
}

}  // namespace arg
