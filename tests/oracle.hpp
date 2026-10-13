#pragma once

// Reference model of the gate's rules, written to be obviously correct rather
// than fast. It shares the *spec* with gate.hpp (rule order, reason codes) but
// none of the bookkeeping:
//
//   gate                                  oracle
//   ----                                  ------
//   unordered_map of open orders          plain vector, scanned every time
//   running open_buy / open_sell totals   summed from the vector on demand
//   std::map price->count for STP         min/max found by scanning
//   incremental token bucket              closed-form arrival curve (below)
//   per-second counter                    count of accepted timestamps in that second
//
// Token bucket closed form: a bucket of depth B refilled at r, starting full,
// holds before the k-th accepted order
//     A_k = min( B,  B + r*t_k - C_{k-1} + min_{m<k} (C_{m-1} - r*t_m) )
// where C is cumulative cost of accepted orders. (Unroll L_k = min(B, L_{k-1} +
// r*dt) - c_k.) The oracle tracks only that running minimum -- no bucket.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "arg/config.hpp"
#include "arg/gate.hpp"
#include "arg/types.hpp"

namespace oracle {

using namespace arg;

struct OOpen {
    std::uint64_t id;
    std::uint32_t algo;
    std::uint16_t instr;
    Side side;
    Price price;
    Qty leaves;
    bool cancel_sent;
};

struct OAlgo {
    std::int64_t cum_cost = 0;           // C_{k-1}
    bool has_hist = false;
    std::int64_t min_term = 0;           // min_m (C_{m-1} - r*t_m)
    std::vector<std::uint64_t> secs;     // calendar second of each accepted order (unregistered only)
    std::uint64_t orders = 0, trades = 0;
};

class Oracle {
public:
    explicit Oracle(const Config& c) : cfg(c) {
        for (int i = 0; i < kMaxInstr; ++i) ltp[i] = cfg.instr[i].ref_price;
    }

    Config cfg;
    std::vector<OOpen> open;
    Qty pos[kMaxInstr] = {};
    std::int64_t cash[kMaxInstr] = {};
    Price ltp[kMaxInstr] = {};
    OAlgo algo[kMaxAlgo];
    bool kg = false, ka[kMaxAlgo] = {}, ki[kMaxInstr] = {};
    bool feed_seen = false;
    Ts last_feed = 0;
    std::vector<std::uint64_t> expected_cancels;  // ids the last call should have mass-cancelled

    const OOpen* find(std::uint64_t id) const {
        for (const auto& o : open) if (o.id == id) return &o;
        return nullptr;
    }
    OOpen* find(std::uint64_t id) {
        for (auto& o : open) if (o.id == id) return &o;
        return nullptr;
    }

    bool otr_ge(std::uint32_t a, std::uint32_t ratio) const {
        if (ratio == 0) return false;
        const OAlgo& s = algo[a];
        if (s.orders < cfg.otr.min_orders) return false;
        return s.orders >= std::uint64_t(ratio) * std::max<std::uint64_t>(1, s.trades);
    }

    std::int64_t tokens_available(std::uint32_t a, Ts t) const {
        const AlgoCfg& ac = cfg.algo[a];
        const std::int64_t B = std::int64_t(ac.burst) * kTokenUnit;
        const OAlgo& s = algo[a];
        if (!s.has_hist) return B;
        const std::int64_t r = ac.rate_per_sec;
        const std::int64_t v = B + r * std::int64_t(t) - s.cum_cost + s.min_term;
        return std::min(B, v);
    }

    Reason decide(const NewOrder& o, Ts t, std::int64_t* cost_out) const {
        if (o.instr >= kMaxInstr || !cfg.instr[o.instr].enabled) return Reason::BadInstrument;
        const InstrCfg& ic = cfg.instr[o.instr];
        if (o.qty <= 0 || o.price <= 0 || o.price % ic.tick != 0 || std::uint8_t(o.side) > 1 ||
            std::uint8_t(o.tif) > 1)
            return Reason::BadOrder;
        if (o.order_id >= kFlattenIdBase || find(o.order_id)) return Reason::DuplicateOrderId;
        if (o.algo_id == 0) return Reason::AlgoIdMissing;
        if (o.algo_id >= kMaxAlgo || !cfg.algo[o.algo_id].enabled) return Reason::AlgoUnknown;
        if (kg) return Reason::KillGlobal;
        if (ka[o.algo_id]) return Reason::KillAlgo;
        if (ki[o.instr]) return Reason::KillInstrument;
        if (o.price < ic.band_lo || o.price > ic.band_hi) return Reason::PriceBandDaily;
        const Price L = ltp[o.instr];
        // |p - L| / L <= bp / 10000, cross-multiplied
        if (std::max(o.price - L, L - o.price) * 10000 > L * std::int64_t(ic.ltp_band_bp)) return Reason::PriceBandLtp;
        if (o.qty > ic.max_qty) return Reason::MaxQty;
        if (o.price * o.qty > ic.max_value) return Reason::MaxValue;

        Qty buys = 0, sells = 0;
        bool have_ask = false, have_bid = false;
        Price min_ask = 0, max_bid = 0;
        for (const auto& x : open) {
            if (x.instr != o.instr) continue;
            if (x.side == Side::Buy) {
                buys += x.leaves;
                if (!have_bid || x.price > max_bid) max_bid = x.price;
                have_bid = true;
            } else {
                sells += x.leaves;
                if (!have_ask || x.price < min_ask) min_ask = x.price;
                have_ask = true;
            }
        }
        // Worst case: every open buy fills (long side), or every open sell fills (short side).
        if (o.side == Side::Buy && pos[o.instr] + buys + o.qty > ic.max_pos) return Reason::PositionLimit;
        if (o.side == Side::Sell && pos[o.instr] - sells - o.qty < -ic.max_pos) return Reason::PositionLimit;
        if (o.side == Side::Buy && have_ask && o.price >= min_ask) return Reason::SelfTrade;
        if (o.side == Side::Sell && have_bid && o.price <= max_bid) return Reason::SelfTrade;

        const AlgoCfg& ac = cfg.algo[o.algo_id];
        if (!ac.registered) {
            const std::uint64_t sec = t / 1'000'000'000ull;
            std::uint64_t n = 0;
            for (auto s : algo[o.algo_id].secs) n += (s == sec);
            if (n >= cfg.unregistered_ops_cap) return Reason::UnregisteredOps;
        }
        const std::int64_t cost = otr_ge(o.algo_id, cfg.otr.throttle) ? 2 * kTokenUnit : kTokenUnit;
        const std::int64_t avail = tokens_available(o.algo_id, t);
        if (avail < cost) return (cost > kTokenUnit && avail >= kTokenUnit) ? Reason::OtrThrottle : Reason::RateLimit;
        *cost_out = cost;
        return Reason::Ok;
    }

    Reason submit(const NewOrder& o, Ts t) {
        expected_cancels.clear();
        std::int64_t cost = 0;
        const Reason r = decide(o, t, &cost);
        if (r != Reason::Ok) return r;
        OAlgo& s = algo[o.algo_id];
        const std::int64_t rate = cfg.algo[o.algo_id].rate_per_sec;
        const std::int64_t term = s.cum_cost - rate * std::int64_t(t);
        s.min_term = s.has_hist ? std::min(s.min_term, term) : term;
        s.has_hist = true;
        s.cum_cost += cost;
        if (!cfg.algo[o.algo_id].registered) s.secs.push_back(t / 1'000'000'000ull);
        ++s.orders;
        open.push_back({o.order_id, o.algo_id, o.instr, o.side, o.price, o.qty, false});
        breach_check(o.algo_id);
        return r;
    }

    void cancel_request(std::uint64_t id) {
        expected_cancels.clear();
        OOpen* x = find(id);
        if (!x) return;
        x->cancel_sent = true;
        if (x->algo == 0) return;
        ++algo[x->algo].orders;
        breach_check(x->algo);
    }

    void fill(const Fill& f) {
        expected_cancels.clear();
        const Qty sq = f.side == Side::Buy ? f.qty : -f.qty;
        pos[f.instr] += sq;
        cash[f.instr] -= sq * f.price;
        if (OOpen* x = find(f.order_id)) {
            if (x->algo) ++algo[x->algo].trades;
            x->leaves -= std::min(x->leaves, f.qty);
            if (x->leaves == 0) erase(f.order_id);
        }
        loss_check();
    }

    void done(std::uint64_t id) { expected_cancels.clear(); erase(id); }

    void market(std::uint16_t i, Price p, Ts t) {
        expected_cancels.clear();
        last_feed = t;
        feed_seen = true;
        if (p > 0) ltp[i] = p;
        loss_check();
    }

    void timer(Ts t) {
        expected_cancels.clear();
        if (feed_seen && !kg && cfg.heartbeat_timeout_ns && t > last_feed && t - last_feed > cfg.heartbeat_timeout_ns)
            fire(Scope::Global, 0);
    }

    void kill(Scope s, std::uint32_t id) { expected_cancels.clear(); fire(s, id); }

    void reset(Scope s, std::uint32_t id) {
        expected_cancels.clear();
        if (s == Scope::Global) kg = false; else if (s == Scope::Algo) ka[id] = false; else ki[id] = false;
    }

    std::int64_t pnl() const {
        std::int64_t p = 0;
        for (int i = 0; i < kMaxInstr; ++i) p += cash[i] + pos[i] * ltp[i];
        return p;
    }

    Qty open_qty(int instr, Side s) const {
        Qty q = 0;
        for (const auto& x : open) if (x.instr == instr && x.side == s) q += x.leaves;
        return q;
    }

private:
    void erase(std::uint64_t id) {
        open.erase(std::remove_if(open.begin(), open.end(), [&](const OOpen& x) { return x.id == id; }), open.end());
    }

    void breach_check(std::uint32_t a) {
        if (!ka[a] && otr_ge(a, cfg.otr.breach)) fire(Scope::Algo, a);
    }

    void loss_check() {
        if (!kg && cfg.loss_limit > 0 && pnl() < -cfg.loss_limit) fire(Scope::Global, 0);
    }

    void fire(Scope s, std::uint32_t id) {
        if (s == Scope::Global) kg = true; else if (s == Scope::Algo) ka[id] = true; else ki[id] = true;
        for (auto& x : open) {
            if (x.cancel_sent) continue;
            if (s == Scope::Global || (s == Scope::Algo && x.algo == id) || (s == Scope::Instrument && x.instr == id)) {
                x.cancel_sent = true;
                expected_cancels.push_back(x.id);
            }
        }
        std::sort(expected_cancels.begin(), expected_cancels.end());
    }
};

}  // namespace oracle
