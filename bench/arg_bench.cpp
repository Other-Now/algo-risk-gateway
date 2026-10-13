// arg_bench: what does each pre-trade check cost, and how fast does the kill
// switch go quiet?
//
//   arg_bench [--orders 2000000] [--out results/bench.txt]
//
// Workload: 16 instruments, 8 registered algos, ~1,000 open orders in steady
// state, every timed order passes every check (the worst case: no early exit).
// Prices are spread across the LTP band, buys below / sells above the LTP so
// self-trade prevention does real work without rejecting.
//
// Measurements, each reported as p50 / p99 / p99.9 in ns:
//   1. per-check: TSC read at every check boundary (probe overhead subtracted)
//   2. whole gate, one TSC pair per order -- with and without the audit append
//   3. accept -> OUCH bytes in the buffer (gate + encode)
//   4. un-probed throughput: N orders, one clock read at each end
//   5. audit sealing: SHA-256 chain cost per record (off the order path)
//   6. kill switch: time-to-block and time-to-quiet vs number of resting orders

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "arg/audit.hpp"
#include "arg/clock.hpp"
#include "arg/exchange_sim.hpp"
#include "arg/gate.hpp"
#include "arg/rng.hpp"
#include "arg/vendor/histogram.hpp"
#include "arg/vendor/ouch.hpp"

using namespace arg;

namespace {

constexpr int kInstr = 16;
constexpr int kAlgos = 8;

Config bench_config() {
    Config c = zeroed();
    for (int i = 0; i < kInstr; ++i) {
        InstrCfg& ic = c.instr[i];
        ic.enabled = 1;
        char sym[8] = {'S', 'Y', 'M', char('A' + i), 0};
        set_symbol(ic, sym);
        ic.tick = 5;
        set_bands(ic, 100'000, 10);
        ic.ltp_band_bp = 300;
        ic.max_qty = 1'000;
        ic.max_value = 200'000'000;
        ic.max_pos = 1'000'000'000;
    }
    for (int a = 1; a <= kAlgos; ++a) c.algo[a] = AlgoCfg{1, 1, 700000u + a, 10'000'000, 1'000'000};
    c.unregistered_ops_cap = 10;
    c.otr = OtrCfg{1'000'000'000, 1'000'000, 2'000'000};  // never throttles in the benchmark
    c.loss_limit = 0;
    c.heartbeat_timeout_ns = 0;
    return c;
}

struct TscProbe {
    std::uint64_t last = 0;
    std::uint64_t d[std::size_t(Stage::kCount)] = {};
    void start() { last = ticks(); }
    void mark(Stage s) {
        const std::uint64_t n = ticks();
        d[std::size_t(s)] = n - last;
        last = n;
    }
};

struct Workload {
    std::vector<NewOrder> orders;
    std::vector<std::uint64_t> retire;  // id to retire (Done) after order k, 0 = none
};

Workload make_workload(std::size_t n, std::uint64_t seed) {
    Rng R(seed);
    Workload w;
    w.orders.reserve(n);
    w.retire.reserve(n);
    std::vector<std::uint64_t> live;
    for (std::size_t k = 0; k < n; ++k) {
        const auto instr = std::uint16_t(R.below(kInstr));
        const Side s = R.chance(5000) ? Side::Buy : Side::Sell;
        const Price off = R.range(1, 55) * 5;  // up to 2.75%: inside the 3% LTP band
        const Price px = s == Side::Buy ? 100'000 - off : 100'000 + off;
        const NewOrder o{k + 1, std::uint32_t(1 + R.below(kAlgos)), instr, s, Tif::Day, px, R.range(1, 500)};
        w.orders.push_back(o);
        live.push_back(o.order_id);
        std::uint64_t r = 0;
        if (live.size() > 1000) {  // steady state ~1,000 open orders
            const std::size_t j = R.below(live.size());
            r = live[j];
            live[j] = live.back();
            live.pop_back();
        }
        w.retire.push_back(r);
    }
    return w;
}

struct Pct { double p50, p99, p999; };
Pct pct(const Histogram& h, double scale) {
    return {double(h.percentile(50)) * scale, double(h.percentile(99)) * scale, double(h.percentile(99.9)) * scale};
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 2'000'000;
    std::string out = "results/bench.txt";
    for (int k = 1; k + 1 < argc; k += 2) {
        if (!std::strcmp(argv[k], "--orders")) n = std::strtoull(argv[k + 1], nullptr, 10);
        if (!std::strcmp(argv[k], "--out")) out = argv[k + 1];
    }

    const double ns_per_tick = calibrate_ns_per_tick();
    const std::uint64_t ovh = probe_overhead_ticks();
    // Histograms record tenths of a nanosecond so sub-ns differences survive.
    const double to_dns = ns_per_tick * 10.0;
    auto rec = [&](Histogram& h, std::uint64_t dt_ticks) {
        const std::uint64_t t = dt_ticks > ovh ? dt_ticks - ovh : 0;
        h.record(std::uint64_t(double(t) * to_dns + 0.5));
    };
    constexpr double kScale = 0.1;

    const Config cfg = bench_config();
    const Workload W = make_workload(n, 7);
    char line[512];
    std::string report;
    auto emit = [&](const char* s) { std::fputs(s, stdout); report += s; };

    std::snprintf(line, sizeof line,
                  "arg_bench: %zu orders, %d instruments, %d algos, ~1000 open orders steady state\n"
                  "TSC: %.4f ns/tick, probe overhead %llu ticks (%.1f ns, subtracted)\n\n",
                  n, kInstr, kAlgos, ns_per_tick, (unsigned long long)ovh, double(ovh) * ns_per_tick);
    emit(line);

    // ------------------------------------------------- 1. per-check latency
    Histogram stage_h[std::size_t(Stage::kCount)];
    Histogram sum_h;
    {
        AuditLog log(cfg, 2 * n + 16);  // submits + retires: never reallocates mid-run
        Gate g(cfg, &log);
        TscProbe p;
        Ts t = 1'000'000'000;
        for (std::size_t k = 0; k < n; ++k) {
            t += 1'000;
            const Decision d = g.submit(W.orders[k], t, p);
            if (d.ok()) {
                std::uint64_t total = 0;
                for (std::size_t s = 0; s < std::size_t(Stage::kCount); ++s) {
                    rec(stage_h[s], p.d[s]);
                    total += p.d[s] > ovh ? p.d[s] - ovh : 0;
                }
                sum_h.record(std::uint64_t(double(total) * to_dns + 0.5));
            }
            if (W.retire[k]) g.on_done(W.retire[k], t);
        }
    }
    emit("1. per-check latency (accepted orders; every check runs)        p50       p99     p99.9  ns\n");
    for (std::size_t s = 0; s < std::size_t(Stage::kCount); ++s) {
        const Pct q = pct(stage_h[s], kScale);
        std::snprintf(line, sizeof line, "   %-58s %8.1f  %8.1f  %8.1f\n", stage_name(Stage(s)), q.p50, q.p99, q.p999);
        emit(line);
    }
    {
        const Pct q = pct(sum_h, kScale);
        std::snprintf(line, sizeof line, "   %-58s %8.1f  %8.1f  %8.1f\n", "sum of stages (probed run)", q.p50, q.p99, q.p999);
        emit(line);
    }

    // --------------------------------------- 2/3. whole gate, single probe pair
    auto whole = [&](bool audit, bool encode, Histogram& h) {
        AuditLog log(cfg, 2 * n + 16);  // submits + retires: never reallocates mid-run
        Gate g(cfg, audit ? &log : nullptr);
        unsigned char wire[ouch::kEnterOrderLen];
        std::uint64_t sink = 0;
        std::uint64_t syms[kMaxInstr];
        for (int i = 0; i < kMaxInstr; ++i) syms[i] = ExchangeSim::pack_symbol(cfg.instr[i].symbol);
        Ts t = 1'000'000'000;
        for (std::size_t k = 0; k < n; ++k) {
            t += 1'000;
            const NewOrder& o = W.orders[k];
            const std::uint64_t a = ticks();
            const Decision d = g.submit(o, t);
            if (encode && d.ok())
                sink += ouch::encode(wire, {o.order_id, syms[o.instr], std::uint32_t(o.qty), std::uint32_t(o.price),
                                            o.side == Side::Buy, 99999});
            const std::uint64_t b = ticks();
            sink += wire[20];
            if (d.ok()) rec(h, b - a);
            if (W.retire[k]) g.on_done(W.retire[k], t);
        }
        return sink;
    };
    Histogram h_noaudit, h_audit, h_wire;
    std::uint64_t sink = whole(false, false, h_noaudit);
    sink += whole(true, false, h_audit);
    sink += whole(true, true, h_wire);
    emit("\n2. whole gate, one probe pair per order                          p50       p99     p99.9  ns\n");
    for (auto [label, h] : {std::pair<const char*, Histogram*>{"submit() without audit", &h_noaudit},
                            {"submit() with audit append", &h_audit},
                            {"3. accept -> OUCH bytes in buffer (gate+audit+encode)", &h_wire}}) {
        const Pct q = pct(*h, kScale);
        std::snprintf(line, sizeof line, "   %-58s %8.1f  %8.1f  %8.1f\n", label, q.p50, q.p99, q.p999);
        emit(line);
    }

    // ------------------------------------------------ 4. un-probed throughput
    {
        AuditLog log(cfg, 2 * n + 16);  // submits + retires: never reallocates mid-run
        Gate g(cfg, &log);
        Ts t = 1'000'000'000;
        std::size_t ok = 0;
        const auto w0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < n; ++k) {
            t += 1'000;
            ok += g.submit(W.orders[k], t).ok();
            if (W.retire[k]) g.on_done(W.retire[k], t);
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        std::snprintf(line, sizeof line,
                      "\n4. un-probed: %zu submits (+%zu retires) in %.3f s = %.1f ns per submit incl. retire, "
                      "%.2f M orders/s (%zu accepted)\n",
                      n, n, s, s * 1e9 / double(n), double(n) / s / 1e6, ok);
        emit(line);

        // ------------------------------------------------- 5. sealing cost
        const auto h0 = std::chrono::steady_clock::now();
        Digest h = log.seed();
        for (const Record& r : log.records()) h = AuditLog::link(h, r);
        const double hs = std::chrono::duration<double>(std::chrono::steady_clock::now() - h0).count();
        sink += h[0];
        const std::string tmp = out + ".audit.tmp";
        const auto s0 = std::chrono::steady_clock::now();
        log.write(tmp);
        const double ss = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        std::remove(tmp.c_str());
        const double nr = double(log.records().size());
        std::snprintf(line, sizeof line,
                      "5. audit seal (off the order path), %zu records:\n"
                      "   SHA-256 chain only        %.0f ns/record  (%.2f M records/s)\n"
                      "   chain + write to disk     %.0f ns/record  (%.2f M records/s)\n",
                      log.records().size(), hs * 1e9 / nr, nr / hs / 1e6, ss * 1e9 / nr, nr / ss / 1e6);
        emit(line);
    }

    // ------------------------------------------------ 6. kill time-to-quiet
    emit("\n6. kill switch (global, in-process exchange sim; no network)\n");
    emit("   resting   time-to-block   kill() returns   time-to-quiet   cancels   orders after kill\n");
    for (std::size_t resting : {10u, 100u, 1'000u, 10'000u}) {
        std::vector<double> block_ns, kill_ns, quiet_ns;
        std::size_t leaked = 0, cancels = 0;
        for (int rep = 0; rep < 21; ++rep) {
            Config c = cfg;
            Gate g(c);
            ExchangeSim ex(c);
            std::vector<ExEvent> ev;
            Ts t = 1'000'000'000;
            for (std::size_t k = 0; k < resting; ++k) {
                const NewOrder o{k + 1, std::uint32_t(1 + k % kAlgos), std::uint16_t(k % kInstr), Side::Buy, Tif::Day,
                                 100'000 - 5 * Price(1 + k % 50), 1};
                if (g.submit(o, t).ok()) ex.submit(true, o.order_id, o.instr, o.side, o.tif, o.price, o.qty, t, ev);
            }
            ev.clear();
            const std::uint64_t a = ticks();
            g.kill(Scope::Global, 0, false, t);
            const std::uint64_t b = ticks();
            // first moment a new order is provably blocked
            const bool blocked = !g.submit(NewOrder{999'999'999, 1, 0, Side::Buy, Tif::Day, 99'000, 1}, t).ok();
            const std::uint64_t c1 = ticks();
            auto& acts = g.actions();
            for (const Action& x : acts) ex.cancel(x.order_id, ev);
            cancels = acts.size();
            acts.clear();
            for (const ExEvent& e : ev) if (e.kind == ExEvent::Kind::Done) g.on_done(e.order_id, t);
            const std::uint64_t d = ticks();
            if (!blocked || g.open_orders() != 0) ++leaked;
            kill_ns.push_back(double(b - a) * ns_per_tick);
            block_ns.push_back(double(c1 - a) * ns_per_tick);
            quiet_ns.push_back(double(d - a) * ns_per_tick);
        }
        auto med = [](std::vector<double>& v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        auto fmt = [](double ns, char* buf) {
            if (ns < 1e3) std::snprintf(buf, 32, "%.0f ns", ns);
            else if (ns < 1e6) std::snprintf(buf, 32, "%.1f us", ns / 1e3);
            else std::snprintf(buf, 32, "%.2f ms", ns / 1e6);
        };
        char b1[32], b2[32], b3[32];
        fmt(med(block_ns), b1); fmt(med(kill_ns), b2); fmt(med(quiet_ns), b3);
        std::snprintf(line, sizeof line, "   %7zu   %13s   %14s   %13s   %7zu   %zu\n", resting, b1, b2, b3, cancels, leaked);
        emit(line);
    }
    emit("   (median of 21; time-to-block = kill() + one rejected submit; time-to-quiet = until the exchange\n"
         "    has confirmed every cancel and the gate holds zero open orders)\n");

    std::snprintf(line, sizeof line, "\n(sink %llu)\n", (unsigned long long)(sink & 1));
    emit(line);
    if (std::FILE* f = std::fopen(out.c_str(), "w")) { std::fputs(report.c_str(), f); std::fclose(f); }
    return 0;
}
