// arg_sim: one simulated trading session through the gate.
//
// Five strategies (each built to trip a different rule), noise flow from other
// participants, and a scripted day:
//   40%  manual kill of one instrument (then reset at 55%)
//   ~    the churning algo walks its OTR into the throttle, then the breach kill
//   80%  market-data feed goes silent -> heartbeat kill -> mass cancel + flatten
// plus N exchange fills deliberately dropped before they reach the gate, so
// post-trade reconciliation has real breaks to find.
//
// Writes <out>/audit.log, <out>/trades.csv, <out>/sim_summary.txt.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <chrono>

#include "arg/audit.hpp"
#include "arg/exchange_sim.hpp"
#include "arg/gate.hpp"
#include "arg/recon.hpp"
#include "arg/rng.hpp"
#include "arg/vendor/ouch.hpp"

using namespace arg;

namespace {

constexpr Ts kStart = 9ull * 3600 * 1'000'000'000 + 15ull * 60 * 1'000'000'000;  // 09:15:00
constexpr Ts kStep = 100'000;                                                     // 100 us

enum : std::uint32_t { kMM = 1, kMomo = 2, kRetail = 3, kFat = 4, kChurn = 5 };
const char* algo_label(std::uint32_t a) {
    switch (a) {
        case kMM: return "MM (registered, quotes both sides)";
        case kMomo: return "MOMO (registered, IOC taker)";
        case kRetail: return "RETAIL (unregistered, bursty)";
        case kFat: return "FATFINGER (registered, bad orders)";
        case kChurn: return "CHURN (registered, cancel/replace)";
    }
    return "?";
}

Config session_config() {
    Config c = zeroed();
    struct Spec { const char* sym; Price ref, tick; };
    // MCX-style commodity names; prices in paise.
    const Spec specs[] = {{"GOLD", 7'150'000, 100}, {"SILVER", 8'600'000, 100}, {"CRUDE", 650'000, 100},
                          {"NATGAS", 25'000, 10},   {"COPPER", 82'000, 5},     {"ZINC", 26'000, 5}};
    for (int i = 0; i < 6; ++i) {
        InstrCfg& ic = c.instr[i];
        ic.enabled = 1;
        set_symbol(ic, specs[i].sym);
        ic.tick = specs[i].tick;
        set_bands(ic, specs[i].ref, 6);  // +/-6% daily range
        ic.ltp_band_bp = 200;            // +/-2% from LTP
        ic.max_qty = 50;
        ic.max_value = specs[i].ref * 40;
        ic.max_pos = 200;
    }
    auto algo = [&](std::uint32_t id, bool reg, std::uint32_t xid, std::uint32_t rate, std::uint32_t burst) {
        c.algo[id] = AlgoCfg{1, std::uint8_t(reg ? 1 : 0), xid, rate, burst};
    };
    algo(kMM, true, 100101, 400, 40);
    algo(kMomo, true, 100102, 50, 10);
    algo(kRetail, false, 0, 100, 30);  // configured fast, but unregistered => 10/s cap applies
    algo(kFat, true, 100104, 50, 10);
    algo(kChurn, true, 100105, 2000, 100);
    c.unregistered_ops_cap = 10;
    c.otr = OtrCfg{200, 40, 250};  // throttle near the first penalty slab (50), kill at the 250 slab
    c.loss_limit = 500'000'00;     // Rs 5 lakh
    c.heartbeat_timeout_ns = 50'000'000;  // 50 ms of silence
    c.flatten_on_auto_kill = 1;
    return c;
}

struct Session {
    Config cfg;
    AuditLog log;
    Gate gate;
    ExchangeSim ex;
    Rng rng;
    std::vector<ExEvent> evs;
    Price mid[kMaxInstr] = {};
    std::uint64_t next_id = 1;
    std::uint64_t noise_id = 1ull << 60;
    std::vector<std::uint64_t> noise_live[kMaxInstr];
    std::map<std::uint32_t, std::set<std::uint64_t>> algo_open;  // what each algo thinks is working
    std::map<std::uint64_t, std::uint32_t> owner;
    std::uint64_t reasons[std::size_t(Reason::kCount)] = {};
    std::uint64_t by_algo_accept[kMaxAlgo] = {}, by_algo_sent[kMaxAlgo] = {};
    std::uint64_t fills_seen = 0, fills_dropped = 0, drop_target = 0;
    std::uint64_t wire_bytes = 0;
    bool feed_up = true;

    Session(std::uint64_t seed, std::uint64_t drops)
        : cfg(session_config()), log(cfg, 1 << 21), gate(cfg, &log), ex(cfg), rng(seed), drop_target(drops) {
        for (int i = 0; i < kMaxInstr; ++i) mid[i] = cfg.instr[i].ref_price;
    }

    int n_instr() const { return 6; }

    void send_wire(const NewOrder& o, Ts ts) {
        unsigned char buf[ouch::kEnterOrderLen];
        const std::size_t n = ouch::encode(buf, {o.order_id, ExchangeSim::pack_symbol(cfg.instr[o.instr].symbol),
                                                 std::uint32_t(o.qty), std::uint32_t(o.price), o.side == Side::Buy,
                                                 o.tif == Tif::Ioc ? 0u : 99999u});
        wire_bytes += n;
        ex.submit_ouch(buf, n, ts, evs);
    }

    // Deliver exchange events to the gate (and to the owning algo's bookkeeping).
    void pump(Ts ts) {
        for (std::size_t k = 0; k < evs.size(); ++k) {  // evs may grow while we drain gate actions
            const ExEvent e = evs[k];
            switch (e.kind) {
                case ExEvent::Kind::Fill: {
                    ++fills_seen;
                    // Fault injection: lose a few fills on the way to the gate, spread over the day.
                    if (fills_dropped < drop_target && fills_seen % 331 == 0) { ++fills_dropped; break; }
                    gate.on_fill(e.fill, ts);
                    break;
                }
                case ExEvent::Kind::Done: gate.on_done(e.order_id, ts); forget(e.order_id); break;
                case ExEvent::Kind::Trade:
                    if (feed_up) gate.on_market(e.instr, e.price, ts);
                    break;
            }
            if (e.kind == ExEvent::Kind::Fill && !gate.is_open(e.fill.order_id)) forget(e.fill.order_id);
            drain_actions(ts);
        }
        evs.clear();
    }

    void forget(std::uint64_t id) {
        auto it = owner.find(id);
        if (it == owner.end()) return;
        algo_open[it->second].erase(id);
        owner.erase(it);
    }

    void drain_actions(Ts ts) {
        auto& acts = gate.actions();
        if (acts.empty()) return;
        std::vector<Action> todo;
        todo.swap(acts);
        for (const Action& a : todo) {
            if (a.kind == Action::Kind::Cancel) ex.cancel(a.order_id, evs);
            else send_wire(a.order, ts);
        }
    }

    // Strategy -> gate -> wire.
    Reason place(std::uint32_t algo, std::uint16_t instr, Side s, Tif tif, Price px, Qty q, Ts ts) {
        NewOrder o{next_id++, algo, instr, s, tif, px, q};
        const Reason r = gate.submit(o, ts).reason;
        ++reasons[std::size_t(r)];
        if (algo < kMaxAlgo) ++by_algo_sent[algo];
        if (r == Reason::Ok) {
            ++by_algo_accept[algo];
            owner[o.order_id] = algo;
            algo_open[algo].insert(o.order_id);
            drain_actions(ts);
            send_wire(o, ts);
        }
        pump(ts);
        return r;
    }

    void cancel(std::uint64_t id, Ts ts) {
        gate.cancel_request(id, ts);
        drain_actions(ts);
        ex.cancel(id, evs);
        pump(ts);
    }

    Price snap(int i, Price p) const { const Price t = cfg.instr[i].tick; return (p / t) * t; }
    Price tick(int i) const { return cfg.instr[i].tick; }

    void noise(Ts ts) {
        const int i = int(rng.below(std::uint64_t(n_instr())));
        if (rng.chance(3000)) mid[i] += tick(i) * (rng.chance(5000) ? 1 : -1);
        const Side s = rng.chance(5000) ? Side::Buy : Side::Sell;
        const Qty q = rng.range(1, 30);
        if (rng.chance(3500)) {  // marketable
            const Price px = s == Side::Buy ? mid[i] + 3 * tick(i) : mid[i] - 3 * tick(i);
            ex.submit(false, noise_id++, std::uint16_t(i), s, Tif::Ioc, px, q, ts, evs);
        } else {
            const Price off = rng.range(1, 5) * tick(i);
            const Price px = s == Side::Buy ? mid[i] - off : mid[i] + off;
            const std::uint64_t id = noise_id++;
            ex.submit(false, id, std::uint16_t(i), s, Tif::Day, px, q, ts, evs);
            noise_live[i].push_back(id);
            if (noise_live[i].size() > 80) {
                ex.cancel(noise_live[i].front(), evs);
                noise_live[i].erase(noise_live[i].begin());
            }
        }
        pump(ts);
    }

    void run_mm(Ts ts) {
        auto& mine = algo_open[kMM];
        if (mine.size() > 12 || (!mine.empty() && rng.chance(2500))) {
            cancel(*mine.begin(), ts);
            return;
        }
        const int i = int(rng.below(std::uint64_t(n_instr())));
        const Qty q = rng.range(2, 15);
        place(kMM, std::uint16_t(i), Side::Buy, Tif::Day, mid[i] - tick(i), q, ts);
        place(kMM, std::uint16_t(i), Side::Sell, Tif::Day, mid[i] + tick(i), q, ts);
    }

    void run_momo(Ts ts) {
        const int i = int(rng.below(std::uint64_t(n_instr())));
        const Side s = rng.chance(5000) ? Side::Buy : Side::Sell;
        const Price px = s == Side::Buy ? mid[i] + 2 * tick(i) : mid[i] - 2 * tick(i);
        place(kMomo, std::uint16_t(i), s, Tif::Ioc, px, rng.range(10, 45), ts);
    }

    void run_retail_burst(Ts ts) {
        // 30 orders inside one calendar second: only the first 10 should leave.
        const int i = int(rng.below(std::uint64_t(n_instr())));
        for (int k = 0; k < 30; ++k) {
            const Side s = k % 2 ? Side::Buy : Side::Sell;
            const Price px = s == Side::Buy ? mid[i] - 4 * tick(i) : mid[i] + 4 * tick(i);
            place(kRetail, std::uint16_t(i), s, Tif::Ioc, px, 1, ts + Ts(k) * 1000);
        }
    }

    void run_fatfinger(Ts ts) {
        const int i = int(rng.below(std::uint64_t(n_instr())));
        const InstrCfg& ic = cfg.instr[i];
        const auto inst = std::uint16_t(i);
        switch (rng.below(8)) {
            case 0: place(kFat, inst, Side::Buy, Tif::Ioc, mid[i], ic.max_qty * 10, ts); break;                // fat qty
            case 1: place(kFat, inst, Side::Buy, Tif::Ioc, snap(i, ic.band_hi + 5 * ic.tick), 1, ts); break;  // over circuit
            case 2: place(kFat, inst, Side::Sell, Tif::Ioc, snap(i, mid[i] * 96 / 100), 1, ts); break;       // 4% off LTP
            case 3: place(kFat, inst, Side::Buy, Tif::Ioc, mid[i] + ic.tick / 2 + 1, 1, ts); break;           // off tick
            case 4: place(0, inst, Side::Buy, Tif::Ioc, mid[i], 1, ts); break;                                // untagged
            case 5: place(17, inst, Side::Buy, Tif::Ioc, mid[i], 1, ts); break;                               // unregistered tag
            case 6: place(kFat, inst, Side::Buy, Tif::Ioc, mid[i], ic.max_qty, ts); break;                    // value check
            default: place(kFat, inst, Side::Sell, Tif::Ioc, mid[i] - ic.tick, 5, ts); break;                 // a sane one
        }
    }

    void run_churn(Ts ts) {
        auto& mine = algo_open[kChurn];
        if (!mine.empty()) cancel(*mine.begin(), ts);
        const int i = int(rng.below(std::uint64_t(n_instr())));
        place(kChurn, std::uint16_t(i), Side::Buy, Tif::Day, mid[i] - 15 * tick(i), 1, ts);
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string out = "results";
    double seconds = 10.0;
    std::uint64_t seed = 42, drops = 3;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (!std::strcmp(argv[k], "--out")) out = argv[k + 1];
        else if (!std::strcmp(argv[k], "--seconds")) seconds = std::atof(argv[k + 1]);
        else if (!std::strcmp(argv[k], "--seed")) seed = std::strtoull(argv[k + 1], nullptr, 10);
        else if (!std::strcmp(argv[k], "--drop-fills")) drops = std::strtoull(argv[k + 1], nullptr, 10);
    }

    Session S(seed, drops);
    const std::uint64_t steps = std::uint64_t(seconds * 1e9 / double(kStep));
    const auto wall0 = std::chrono::steady_clock::now();

    for (std::uint64_t n = 0; n < steps; ++n) {
        const Ts ts = kStart + n * kStep;
        const double f = double(n) / double(steps);

        if (n == steps * 40 / 100) S.gate.kill(Scope::Instrument, 2, false, ts), S.drain_actions(ts), S.pump(ts);
        if (n == steps * 55 / 100) S.gate.reset_kill(Scope::Instrument, 2, ts);
        S.feed_up = f < 0.80;  // feed outage for the rest of the day

        for (int k = 0; k < 3; ++k) S.noise(ts);
        if (S.rng.chance(250)) S.run_mm(ts);
        if (S.rng.chance(150)) S.run_momo(ts);
        if (n % 20000 == 7) S.run_retail_burst(ts);
        if (S.rng.chance(40)) S.run_fatfinger(ts);
        if (S.rng.chance(400)) S.run_churn(ts);

        if (n % 10 == 0) {  // every 1 ms: feed heartbeat (LTP snapshot) and timer
            if (S.feed_up)
                for (int i = 0; i < S.n_instr(); ++i) S.gate.on_market(std::uint16_t(i), S.mid[i], ts);
            S.gate.on_timer(ts);
            S.drain_actions(ts);
            S.pump(ts);
        }
    }
    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();

    const Digest head = S.log.write(out + "/audit.log");
    S.ex.write_trades_csv(out + "/trades.csv");

    // Independent compliance query over the audit log: rebuild kill state from
    // KillOut / reset records only, and count orders accepted while in scope.
    std::uint64_t accepted_while_killed = 0;
    {
        bool g = false, a[kMaxAlgo] = {}, ins[kMaxInstr] = {};
        for (const Record& r : S.log.records()) {
            const Rec k = static_cast<Rec>(r.kind);
            if (k == Rec::KillOut) {
                const auto sc = static_cast<Scope>(r.scope);
                if (sc == Scope::Global) g = true; else if (sc == Scope::Algo) a[r.id] = true; else ins[r.id] = true;
            } else if (k == Rec::KillCmd && (r.flags & 2)) {
                const auto sc = static_cast<Scope>(r.scope);
                if (sc == Scope::Global) g = false; else if (sc == Scope::Algo) a[r.id] = false; else ins[r.id] = false;
            } else if (k == Rec::Order && r.reason == 0 && (g || a[r.algo] || ins[r.instr])) {
                ++accepted_while_killed;
            }
        }
    }

    const ReconResult rr = reconcile(S.log.records(), S.ex.firm_trades());

    std::FILE* files[2] = {stdout, std::fopen((out + "/sim_summary.txt").c_str(), "w")};
    for (std::FILE* f : files) {
        if (!f) continue;
        std::fprintf(f, "arg_sim: seed %llu, %.1f s simulated (%llu steps of 100 us), %.2f s wall\n",
                     (unsigned long long)seed, seconds, (unsigned long long)steps, wall_s);
        std::fprintf(f, "audit records %zu, head digest ", S.log.records().size());
        for (int i = 0; i < 8; ++i) std::fprintf(f, "%02x", head[i]);
        std::fprintf(f, "...\nwire: %llu bytes of OUCH EnterOrder\n\n", (unsigned long long)S.wire_bytes);

        std::fprintf(f, "decisions by reason:\n");
        std::uint64_t total = 0;
        for (std::size_t r = 0; r < std::size_t(Reason::kCount); ++r) total += S.reasons[r];
        for (std::size_t r = 0; r < std::size_t(Reason::kCount); ++r)
            if (S.reasons[r])
                std::fprintf(f, "  %-20s %8llu  (%5.1f%%)\n", reason_name(Reason(r)),
                             (unsigned long long)S.reasons[r], 100.0 * double(S.reasons[r]) / double(total));
        std::fprintf(f, "  %-20s %8llu\n\n", "total", (unsigned long long)total);

        std::fprintf(f, "per algo: sent / accepted / OTR (orders:trades)\n");
        for (std::uint32_t a = 1; a <= 5; ++a) {
            const auto o = S.gate.otr_orders(int(a)), t = S.gate.otr_trades(int(a));
            std::fprintf(f, "  %u %-36s %7llu %7llu   %6llu:%-5llu = %.1f%s\n", a, algo_label(a),
                         (unsigned long long)S.by_algo_sent[a], (unsigned long long)S.by_algo_accept[a],
                         (unsigned long long)o, (unsigned long long)t, double(o) / double(t ? t : 1),
                         S.gate.killed_algo(int(a)) ? "  [KILLED]" : "");
        }

        std::fprintf(f, "\nkill events:\n");
        for (const Record& r : S.log.records()) {
            if (static_cast<Rec>(r.kind) != Rec::KillOut) continue;
            std::size_t cancels = 0, flats = 0;
            for (std::size_t j = std::size_t(r.seq) + 1; j < S.log.records().size(); ++j) {
                const Rec k = static_cast<Rec>(S.log.records()[j].kind);
                if (k == Rec::CancelOut) ++cancels; else if (k == Rec::FlattenOut) ++flats; else break;
            }
            std::fprintf(f, "  t=+%7.3f s  %-10s id %-3llu %-15s -> %zu mass-cancels, %zu flatten orders\n",
                         double(r.ts - kStart) / 1e9, scope_name(Scope(r.scope)), (unsigned long long)r.id,
                         kill_why_name(KillWhy(r.reason)), cancels, flats);
        }

        std::fprintf(f, "\nsafety checks:\n");
        std::fprintf(f, "  self-trades seen by the exchange      %llu\n", (unsigned long long)S.ex.self_trades());
        std::fprintf(f, "  orders accepted while killed (audit)  %llu\n", (unsigned long long)accepted_while_killed);
        std::fprintf(f, "  open orders at end of day (gate)      %zu\n", S.gate.open_orders());
        std::fprintf(f, "  fills dropped before the gate         %llu (fault injection)\n\n",
                     (unsigned long long)S.fills_dropped);

        std::fprintf(f, "end-of-day positions (gate view):\n");
        for (int i = 0; i < S.n_instr(); ++i)
            std::fprintf(f, "  %.8s %6lld\n", S.cfg.instr[i].symbol, (long long)S.gate.position(i));
        std::fprintf(f, "\n");
        print_recon(rr, f);
    }
    if (files[1]) std::fclose(files[1]);
    return 0;
}
