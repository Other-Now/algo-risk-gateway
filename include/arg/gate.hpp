#pragma once

// The pre-trade risk gate.
//
// Design rules, each of which exists for a reason:
//
//  * Single-threaded and deterministic. The gate never reads a clock; every
//    call carries its timestamp. Same inputs => same outputs, which is what
//    lets the replay tool reproduce every decision byte-for-byte.
//
//  * check() is read-only, commit() mutates. An order rejected by the 7th
//    check must not have consumed a rate token in the 8th -- it never reached
//    the exchange, so it must not count against the algo.
//
//  * Cheap and most-likely-to-reject checks first; the token bucket is last
//    because it is the only check whose outcome depends on OTR state.
//
//  * Cancels are never blocked. A risk layer that can stop you getting out of
//    the market is worse than none.
//
//  * Open orders count at their worst case until the exchange says they are
//    gone (fill to zero or a Done report) -- *not* when we send the cancel.
//    A cancel can lose the race against a fill.

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "arg/audit.hpp"
#include "arg/config.hpp"
#include "arg/types.hpp"

namespace arg {

// Per-check timing hooks. NullProbe compiles away; the benchmark supplies a
// probe that reads the TSC at each boundary.
enum class Stage : std::uint8_t {
    Sanity, AlgoId, Kill, PriceBand, QtyValue, Position, SelfTrade, Rate, Commit, Audit, kCount
};
inline const char* stage_name(Stage s) {
    static const char* n[] = {"sanity+dup-id", "algo-id", "kill-switch", "price-bands", "qty/value",
                              "position(worst)", "self-trade", "rate+OTR", "commit", "audit-append"};
    return n[static_cast<int>(s)];
}
struct NullProbe {
    void start() {}
    void mark(Stage) {}
};

struct Decision {
    Reason reason;
    bool ok() const { return reason == Reason::Ok; }
};

// Something the caller must send to the exchange on the gate's behalf.
struct Action {
    enum class Kind : std::uint8_t { Cancel, Flatten } kind;
    std::uint64_t order_id;
    NewOrder order;  // valid for Flatten
};

constexpr std::int64_t kTokenUnit = 1'000'000'000;   // one order = 1e9 nano-tokens
constexpr std::uint64_t kFlattenIdBase = 1ull << 62; // risk-generated order ids

class Gate {
public:
    explicit Gate(const Config& cfg, AuditLog* audit = nullptr) : cfg_(cfg), audit_(audit) {
        open_.reserve(1 << 16);
        for (int i = 0; i < kMaxInstr; ++i) ins_[i].ltp = cfg_.instr[i].ref_price;
        for (int a = 0; a < kMaxAlgo; ++a)
            algo_[a].tokens = std::int64_t(cfg_.algo[a].burst) * kTokenUnit;
    }

    // ---------------------------------------------------------------- orders

    template <class P = NullProbe>
    Decision submit(const NewOrder& o, Ts ts, P&& probe = P{}) {
        probe.start();
        const Reason r = check(o, ts, probe);
        if (r == Reason::Ok) commit(o, ts);
        probe.mark(Stage::Commit);
        if (audit_) {
            Record& rec = audit_->append(Rec::Order, ts);
            fill_rec(rec, o);
            rec.reason = static_cast<std::uint8_t>(r);
        }
        probe.mark(Stage::Audit);
        if (r == Reason::Ok) after_order_message(o.algo_id, ts);
        return {r};
    }

    // An algo cancelling its own order: always allowed, counts towards OTR.
    // The order keeps counting at worst case until on_done / a full fill.
    bool cancel_request(std::uint64_t id, Ts ts) {
        if (audit_) audit_->append(Rec::CancelReq, ts).id = id;
        auto it = open_.find(id);
        if (it == open_.end()) return false;
        it->second.cancel_sent = true;
        const std::uint32_t a = it->second.algo;
        if (a != 0) {
            ++algo_[a].otr_orders;
            after_order_message(a, ts);
        }
        return true;
    }

    // ------------------------------------------------------ exchange events

    void on_fill(const Fill& f, Ts ts) {
        if (audit_) {
            Record& r = audit_->append(Rec::FillIn, ts);
            r.id = f.order_id; r.aux = f.exec_id; r.instr = f.instr;
            r.side = static_cast<std::uint8_t>(f.side); r.price = f.price; r.qty = f.qty;
        }
        if (f.instr >= kMaxInstr || f.qty <= 0) { ++unknown_fills_; return; }
        Ins& in = ins_[f.instr];
        const Qty signed_q = f.side == Side::Buy ? f.qty : -f.qty;
        in.pos += signed_q;
        in.cash -= signed_q * f.price;
        ++fills_;

        auto it = open_.find(f.order_id);
        if (it == open_.end()) { ++unknown_fills_; check_loss(ts); return; }
        Open& o = it->second;
        if (o.algo != 0) ++algo_[o.algo].otr_trades;
        const Qty used = std::min(f.qty, o.leaves);
        o.leaves -= used;
        (o.side == Side::Buy ? in.open_buy : in.open_sell) -= used;
        if (o.leaves <= 0) remove_open(it);
        check_loss(ts);
    }

    // Exchange says the order is no longer working (cancel confirmed, IOC remainder expired).
    void on_done(std::uint64_t id, Ts ts) {
        if (audit_) audit_->append(Rec::Done, ts).id = id;
        auto it = open_.find(id);
        if (it != open_.end()) remove_open(it);
    }

    void on_market(std::uint16_t instr, Price ltp, Ts ts) {
        if (audit_) {
            Record& r = audit_->append(Rec::Market, ts);
            r.instr = instr; r.price = ltp;
        }
        last_feed_ts_ = ts;
        feed_seen_ = true;
        if (instr < kMaxInstr && ltp > 0) ins_[instr].ltp = ltp;
        check_loss(ts);
    }

    void on_timer(Ts ts) {
        if (audit_) audit_->append(Rec::Timer, ts);
        if (feed_seen_ && !kill_global_ && cfg_.heartbeat_timeout_ns &&
            ts > last_feed_ts_ && ts - last_feed_ts_ > cfg_.heartbeat_timeout_ns)
            trigger_kill(Scope::Global, 0, KillWhy::HeartbeatLost, cfg_.flatten_on_auto_kill != 0, ts);
    }

    // --------------------------------------------------------- kill switch

    void kill(Scope s, std::uint32_t id, bool flatten, Ts ts) {
        if (audit_) {
            Record& r = audit_->append(Rec::KillCmd, ts);
            r.scope = static_cast<std::uint8_t>(s); r.id = id; r.flags = flatten ? 1 : 0;
        }
        trigger_kill(s, id, KillWhy::Manual, flatten, ts);
    }

    void reset_kill(Scope s, std::uint32_t id, Ts ts) {
        if (audit_) {
            Record& r = audit_->append(Rec::KillCmd, ts);
            r.scope = static_cast<std::uint8_t>(s); r.id = id; r.flags = 2;
        }
        if (s == Scope::Global) kill_global_ = false;
        else if (s == Scope::Algo && id < kMaxAlgo) kill_algo_[id] = false;
        else if (s == Scope::Instrument && id < kMaxInstr) kill_instr_[id] = false;
    }

    // ------------------------------------------------------------- queries

    std::vector<Action>& actions() { return actions_; }
    std::size_t open_orders() const { return open_.size(); }
    bool is_open(std::uint64_t id) const { return open_.find(id) != open_.end(); }
    Qty position(int i) const { return ins_[i].pos; }
    std::int64_t cash(int i) const { return ins_[i].cash; }
    Qty open_buy(int i) const { return ins_[i].open_buy; }
    Qty open_sell(int i) const { return ins_[i].open_sell; }
    Price ltp(int i) const { return ins_[i].ltp; }
    bool killed_global() const { return kill_global_; }
    bool killed_algo(int a) const { return kill_algo_[a]; }
    bool killed_instr(int i) const { return kill_instr_[i]; }
    std::uint64_t otr_orders(int a) const { return algo_[a].otr_orders; }
    std::uint64_t otr_trades(int a) const { return algo_[a].otr_trades; }
    std::uint64_t unknown_fills() const { return unknown_fills_; }
    const Config& config() const { return cfg_; }

    std::int64_t pnl() const {
        std::int64_t p = 0;
        for (int i = 0; i < kMaxInstr; ++i) p += ins_[i].cash + ins_[i].pos * ins_[i].ltp;
        return p;
    }

    bool otr_throttled(std::uint32_t a) const { return otr_at_least(a, cfg_.otr.throttle); }

private:
    struct Open {
        std::uint32_t algo;
        std::uint16_t instr;
        Side side;
        Tif tif;
        Price price;
        Qty leaves;
        bool cancel_sent;
    };
    struct Ins {
        Price ltp = 0;
        Qty pos = 0;
        std::int64_t cash = 0;
        Qty open_buy = 0, open_sell = 0;
        std::map<Price, std::uint32_t> own_bids, own_asks;  // price -> #open orders (for STP)
    };
    struct AlgoState {
        std::int64_t tokens = 0;
        Ts last_refill = 0;
        std::uint64_t ops_second = ~0ull;
        std::uint32_t ops_count = 0;
        std::uint64_t otr_orders = 0, otr_trades = 0;
    };

    template <class P>
    Reason check(const NewOrder& o, Ts ts, P& probe) {
        // 1. sanity
        if (o.instr >= kMaxInstr || !cfg_.instr[o.instr].enabled) return Reason::BadInstrument;
        const InstrCfg& ic = cfg_.instr[o.instr];
        if (o.qty <= 0 || o.price <= 0 || o.price % ic.tick != 0 ||
            static_cast<std::uint8_t>(o.side) > 1 || static_cast<std::uint8_t>(o.tif) > 1)
            return Reason::BadOrder;
        if (o.order_id >= kFlattenIdBase || open_.find(o.order_id) != open_.end())
            return Reason::DuplicateOrderId;
        probe.mark(Stage::Sanity);

        // 2. algo id: present, and in the table of algos the exchange issued ids for
        if (o.algo_id == 0) return Reason::AlgoIdMissing;
        if (o.algo_id >= kMaxAlgo || !cfg_.algo[o.algo_id].enabled) return Reason::AlgoUnknown;
        probe.mark(Stage::AlgoId);

        // 3. kill switch
        if (kill_global_) return Reason::KillGlobal;
        if (kill_algo_[o.algo_id]) return Reason::KillAlgo;
        if (kill_instr_[o.instr]) return Reason::KillInstrument;
        probe.mark(Stage::Kill);

        // 4. price bands
        if (o.price < ic.band_lo || o.price > ic.band_hi) return Reason::PriceBandDaily;
        const Price ltp = ins_[o.instr].ltp;
        const Price dev = o.price > ltp ? o.price - ltp : ltp - o.price;
        if (dev * 10000 > ltp * std::int64_t(ic.ltp_band_bp)) return Reason::PriceBandLtp;
        probe.mark(Stage::PriceBand);

        // 5. size. qty is bounded before the multiply, so price*qty cannot overflow.
        if (o.qty > ic.max_qty) return Reason::MaxQty;
        if (o.price * o.qty > ic.max_value) return Reason::MaxValue;
        probe.mark(Stage::QtyValue);

        // 6. position, counting every open order on this side as if it fills
        const Ins& in = ins_[o.instr];
        if (o.side == Side::Buy) {
            if (in.pos + in.open_buy + o.qty > ic.max_pos) return Reason::PositionLimit;
        } else {
            if (in.pos - in.open_sell - o.qty < -ic.max_pos) return Reason::PositionLimit;
        }
        probe.mark(Stage::Position);

        // 7. self-trade: would this order cross one of our own resting orders?
        if (o.side == Side::Buy) {
            if (!in.own_asks.empty() && o.price >= in.own_asks.begin()->first) return Reason::SelfTrade;
        } else {
            if (!in.own_bids.empty() && o.price <= in.own_bids.rbegin()->first) return Reason::SelfTrade;
        }
        probe.mark(Stage::SelfTrade);

        // 8. rate: SEBI per-second threshold for unregistered algos, then token bucket
        const AlgoCfg& ac = cfg_.algo[o.algo_id];
        const AlgoState& as = algo_[o.algo_id];
        const std::uint64_t sec = ts / 1'000'000'000ull;
        if (!ac.registered) {
            const std::uint32_t used = (sec == as.ops_second) ? as.ops_count : 0;
            if (used >= cfg_.unregistered_ops_cap) return Reason::UnregisteredOps;
        }
        scratch_tokens_ = refill(as, ac, ts);
        scratch_cost_ = otr_throttled(o.algo_id) ? 2 * kTokenUnit : kTokenUnit;
        if (scratch_tokens_ < scratch_cost_)
            return (scratch_cost_ > kTokenUnit && scratch_tokens_ >= kTokenUnit) ? Reason::OtrThrottle
                                                                                : Reason::RateLimit;
        probe.mark(Stage::Rate);
        return Reason::Ok;
    }

    void commit(const NewOrder& o, Ts ts) {
        AlgoState& as = algo_[o.algo_id];
        as.tokens = scratch_tokens_ - scratch_cost_;
        as.last_refill = std::max(as.last_refill, ts);
        const std::uint64_t sec = ts / 1'000'000'000ull;
        if (sec != as.ops_second) { as.ops_second = sec; as.ops_count = 0; }
        ++as.ops_count;
        ++as.otr_orders;
        add_open(o);
    }

    void add_open(const NewOrder& o) {
        Ins& in = ins_[o.instr];
        if (o.side == Side::Buy) { in.open_buy += o.qty; ++in.own_bids[o.price]; }
        else { in.open_sell += o.qty; ++in.own_asks[o.price]; }
        open_.emplace(o.order_id, Open{o.algo_id, o.instr, o.side, o.tif, o.price, o.qty, false});
    }

    void remove_open(std::unordered_map<std::uint64_t, Open>::iterator it) {
        const Open& o = it->second;
        Ins& in = ins_[o.instr];
        auto& book = o.side == Side::Buy ? in.own_bids : in.own_asks;
        (o.side == Side::Buy ? in.open_buy : in.open_sell) -= o.leaves;
        auto lv = book.find(o.price);
        if (lv != book.end() && --lv->second == 0) book.erase(lv);
        open_.erase(it);
    }

    // Integer token bucket: tokens += dt_ns * rate (in nano-tokens), capped at burst.
    // Exact -- no rounding -- so the property test can verify it with a closed form.
    static std::int64_t refill(const AlgoState& as, const AlgoCfg& ac, Ts ts) {
        const std::int64_t cap = std::int64_t(ac.burst) * kTokenUnit;
        if (ts <= as.last_refill || ac.rate_per_sec == 0) return std::min(as.tokens, cap);
        const std::uint64_t dt = ts - as.last_refill;
        if (dt >= std::uint64_t(cap) / ac.rate_per_sec + 1) return cap;
        return std::min(cap, as.tokens + std::int64_t(dt) * std::int64_t(ac.rate_per_sec));
    }

    bool otr_at_least(std::uint32_t a, std::uint32_t ratio) const {
        const AlgoState& as = algo_[a];
        if (ratio == 0 || as.otr_orders < cfg_.otr.min_orders) return false;
        const std::uint64_t trades = as.otr_trades ? as.otr_trades : 1;
        return as.otr_orders >= std::uint64_t(ratio) * trades;
    }

    void after_order_message(std::uint32_t a, Ts ts) {
        if (!kill_algo_[a] && otr_at_least(a, cfg_.otr.breach))
            trigger_kill(Scope::Algo, a, KillWhy::OtrBreach, false, ts);
    }

    void check_loss(Ts ts) {
        if (!kill_global_ && cfg_.loss_limit > 0 && pnl() < -cfg_.loss_limit)
            trigger_kill(Scope::Global, 0, KillWhy::LossLimit, cfg_.flatten_on_auto_kill != 0, ts);
    }

    void trigger_kill(Scope s, std::uint32_t id, KillWhy why, bool flatten, Ts ts) {
        if (s == Scope::Global) kill_global_ = true;
        else if (s == Scope::Algo) { if (id >= kMaxAlgo) return; kill_algo_[id] = true; }
        else { if (id >= kMaxInstr) return; kill_instr_[id] = true; }
        if (audit_) {
            Record& r = audit_->append(Rec::KillOut, ts);
            r.scope = static_cast<std::uint8_t>(s); r.id = id; r.reason = static_cast<std::uint8_t>(why);
            r.flags = flatten ? 1 : 0;
        }

        // Mass-cancel everything resting in scope. Sorted, so the log does not
        // depend on hash-map iteration order.
        cancel_ids_.clear();
        for (auto& [oid, o] : open_) {
            if (o.cancel_sent) continue;
            const bool in_scope = s == Scope::Global ||
                                  (s == Scope::Algo && o.algo == id) ||
                                  (s == Scope::Instrument && o.instr == id);
            if (in_scope) cancel_ids_.push_back(oid);
        }
        std::sort(cancel_ids_.begin(), cancel_ids_.end());
        for (std::uint64_t oid : cancel_ids_) {
            open_.find(oid)->second.cancel_sent = true;
            if (audit_) audit_->append(Rec::CancelOut, ts).id = oid;
            actions_.push_back({Action::Kind::Cancel, oid, {}});
        }

        // Flatten: marketable IOCs at the edge of the LTP band, in max_qty clips.
        // Only meaningful for global / instrument scope -- positions are firm-level.
        if (!flatten || s == Scope::Algo) return;
        for (int i = 0; i < kMaxInstr; ++i) {
            if (!cfg_.instr[i].enabled || (s == Scope::Instrument && std::uint32_t(i) != id)) continue;
            const InstrCfg& ic = cfg_.instr[i];
            Qty left = ins_[i].pos;
            if (left == 0) continue;
            const Side side = left > 0 ? Side::Sell : Side::Buy;
            const Price ltp = ins_[i].ltp;
            const Price d = ltp * std::int64_t(ic.ltp_band_bp) / 10000;
            Price px = side == Side::Sell ? ((ltp - d + ic.tick - 1) / ic.tick) * ic.tick
                                          : ((ltp + d) / ic.tick) * ic.tick;
            px = std::clamp(px, ic.band_lo, ic.band_hi);
            left = left > 0 ? left : -left;
            for (int clip = 0; clip < 16 && left > 0; ++clip) {
                Qty q = std::min(left, ic.max_qty);
                if (px * q > ic.max_value) q = std::max<Qty>(1, ic.max_value / px);
                left -= q;
                NewOrder fo{kFlattenIdBase + flatten_seq_++, 0, std::uint16_t(i), side, Tif::Ioc, px, q};
                add_open(fo);
                if (audit_) {
                    Record& r = audit_->append(Rec::FlattenOut, ts);
                    fill_rec(r, fo);
                }
                actions_.push_back({Action::Kind::Flatten, fo.order_id, fo});
            }
        }
    }

    static void fill_rec(Record& r, const NewOrder& o) {
        r.id = o.order_id; r.algo = o.algo_id; r.instr = o.instr;
        r.side = static_cast<std::uint8_t>(o.side); r.tif = static_cast<std::uint8_t>(o.tif);
        r.price = o.price; r.qty = o.qty;
    }

    Config cfg_;
    AuditLog* audit_;
    Ins ins_[kMaxInstr];
    AlgoState algo_[kMaxAlgo];
    std::unordered_map<std::uint64_t, Open> open_;
    bool kill_global_ = false;
    bool kill_algo_[kMaxAlgo] = {};
    bool kill_instr_[kMaxInstr] = {};
    Ts last_feed_ts_ = 0;
    bool feed_seen_ = false;
    std::uint64_t flatten_seq_ = 0;
    std::uint64_t fills_ = 0, unknown_fills_ = 0;
    std::int64_t scratch_tokens_ = 0, scratch_cost_ = 0;
    std::vector<std::uint64_t> cancel_ids_;
    std::vector<Action> actions_;
};

// Feed one logged input record back into a gate. Used by the replay tool and tests.
inline void apply_input(Gate& g, const Record& r) {
    switch (static_cast<Rec>(r.kind)) {
        case Rec::Order:
            g.submit(NewOrder{r.id, r.algo, r.instr, static_cast<Side>(r.side), static_cast<Tif>(r.tif),
                              r.price, r.qty},
                     r.ts);
            break;
        case Rec::CancelReq: g.cancel_request(r.id, r.ts); break;
        case Rec::FillIn:
            g.on_fill(Fill{r.aux, r.id, r.instr, static_cast<Side>(r.side), r.price, r.qty}, r.ts);
            break;
        case Rec::Done: g.on_done(r.id, r.ts); break;
        case Rec::Market: g.on_market(r.instr, r.price, r.ts); break;
        case Rec::Timer: g.on_timer(r.ts); break;
        case Rec::KillCmd:
            if (r.flags & 2) g.reset_kill(static_cast<Scope>(r.scope), std::uint32_t(r.id), r.ts);
            else g.kill(static_cast<Scope>(r.scope), std::uint32_t(r.id), (r.flags & 1) != 0, r.ts);
            break;
        default: break;  // outputs are regenerated, not fed
    }
    g.actions().clear();
}

}  // namespace arg
