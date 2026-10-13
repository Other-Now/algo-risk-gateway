// arg_tests: unit tests + the property test.
//
//   arg_tests                 unit tests + property test, 1,000,000 random operations
//   arg_tests --ops 20000000  property test size
//   arg_tests --seed 7        property test seed
//
// The property test drives the gate and the reference oracle (tests/oracle.hpp)
// with the same random stream of orders, cancels, fills, done reports, market
// ticks, timer ticks and kill commands, biased towards rule boundaries. After
// every operation it asserts:
//   * identical decision AND reason code on every order;
//   * identical mass-cancel lists on every kill;
//   * identical positions, open quantities and kill state (every 256 ops);
//   * from first principles, independent of either implementation: every
//     accepted order is inside the bands, size limits and worst-case position
//     limit at the moment it was accepted, and does not cross our own book.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "arg/audit.hpp"
#include "arg/exchange_sim.hpp"
#include "arg/gate.hpp"
#include "arg/recon.hpp"
#include "arg/rng.hpp"
#include "arg/sha256.hpp"
#include "arg/vendor/ouch.hpp"
#include "oracle.hpp"

using namespace arg;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_fail;                                                                  \
            if (g_fail < 30) std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

static std::string hex(const Digest& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (auto b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}

// ------------------------------------------------------------------ config

static Config test_config() {
    Config c = zeroed();
    for (int i = 0; i < 4; ++i) {
        InstrCfg& ic = c.instr[i];
        ic.enabled = 1;
        char sym[8] = {'T', 'S', 'T', char('0' + i), 0};
        set_symbol(ic, sym);
        ic.tick = 5;
        set_bands(ic, 10'000, 10);  // 9000..11000
        ic.ltp_band_bp = 300;       // +/-3%
        ic.max_qty = 100;
        ic.max_value = 900'000;     // binds for qty > 90 around 10,000
        ic.max_pos = 400;
    }
    c.algo[1] = AlgoCfg{1, 1, 900001, 2000, 20};
    c.algo[2] = AlgoCfg{1, 0, 0, 1000, 50};     // unregistered: 10/s cap binds
    c.algo[3] = AlgoCfg{1, 1, 900003, 200, 5};
    c.algo[4] = AlgoCfg{1, 0, 0, 5, 3};
    c.algo[5] = AlgoCfg{1, 1, 900005, 500, 10};
    c.unregistered_ops_cap = 10;
    c.otr = OtrCfg{30, 8, 20};
    c.loss_limit = 250'000;
    c.heartbeat_timeout_ns = 1'000'000'000;
    c.flatten_on_auto_kill = 0;
    return c;
}

// ------------------------------------------------------------------ unit tests

static void test_sha256() {
    CHECK(hex(Sha256::of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(Sha256::of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(Sha256::of(m, std::strlen(m))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string mil(1'000'000, 'a');
    CHECK(hex(Sha256::of(mil.data(), mil.size())) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_basic_rules() {
    const Config c = test_config();
    Gate g(c);
    Ts t = 1'000'000'000;
    auto o = [&](std::uint64_t id, std::uint32_t a, Side s, Price p, Qty q) {
        return g.submit(NewOrder{id, a, 0, s, Tif::Day, p, q}, t).reason;
    };
    CHECK(o(1, 1, Side::Buy, 10'000, 10) == Reason::Ok);
    CHECK(o(1, 1, Side::Buy, 10'000, 10) == Reason::DuplicateOrderId);
    CHECK(o(2, 0, Side::Buy, 10'000, 10) == Reason::AlgoIdMissing);
    CHECK(o(3, 9, Side::Buy, 10'000, 10) == Reason::AlgoUnknown);
    CHECK(o(4, 1, Side::Buy, 10'002, 10) == Reason::BadOrder);         // off tick
    CHECK(o(5, 1, Side::Buy, 11'005, 10) == Reason::PriceBandDaily);
    CHECK(o(6, 1, Side::Buy, 10'305, 10) == Reason::PriceBandLtp);     // 3.05% > 3%
    CHECK(o(7, 1, Side::Buy, 10'300, 10) == Reason::Ok);               // exactly 3%: allowed
    CHECK(o(8, 1, Side::Buy, 10'000, 101) == Reason::MaxQty);
    CHECK(o(9, 1, Side::Buy, 10'000, 95) == Reason::MaxValue);         // 950,000 > 900,000
    CHECK(o(10, 1, Side::Sell, 10'000, 10) == Reason::SelfTrade);      // our bid at 10,300 would trade
    CHECK(o(11, 1, Side::Sell, 10'305, 10) == Reason::PriceBandLtp);
    // worst-case position: 20 open buys so far, limit 400
    for (std::uint64_t id = 100; id < 104; ++id) CHECK(o(id, 1, Side::Buy, 9'900, 90) == Reason::Ok);  // 380 open
    CHECK(o(200, 1, Side::Buy, 9'900, 21) == Reason::PositionLimit);
    CHECK(o(201, 1, Side::Buy, 9'900, 20) == Reason::Ok);              // exactly 400
    CHECK(g.open_buy(0) == 400);
    // a cancel REQUEST does not free the limit; the exchange's Done does
    g.cancel_request(201, t);
    CHECK(o(202, 1, Side::Buy, 9'900, 1) == Reason::PositionLimit);
    g.on_done(201, t);
    CHECK(o(203, 1, Side::Buy, 9'900, 1) == Reason::Ok);
}

static void test_rate_and_ops() {
    const Config c = test_config();
    Gate g(c);
    Ts t = 5'000'000'000;
    std::uint64_t id = 1;
    // algo 3: burst 5, 200/s => one token per 5 ms
    int ok = 0;
    for (int k = 0; k < 10; ++k) ok += g.submit(NewOrder{id++, 3, 0, Side::Buy, Tif::Ioc, 9'800, 1}, t).ok();
    CHECK(ok == 5);
    t += 4'999'999;
    CHECK(g.submit(NewOrder{id++, 3, 0, Side::Buy, Tif::Ioc, 9'800, 1}, t).reason == Reason::RateLimit);
    t += 1;
    CHECK(g.submit(NewOrder{id++, 3, 0, Side::Buy, Tif::Ioc, 9'800, 1}, t).ok());
    // algo 2: unregistered, 10 per calendar second regardless of its bucket
    Ts s = 7'000'000'000;
    ok = 0;
    for (int k = 0; k < 25; ++k) ok += g.submit(NewOrder{id++, 2, 1, Side::Buy, Tif::Ioc, 9'800, 1}, s + k).ok();
    CHECK(ok == 10);
    CHECK(g.submit(NewOrder{id++, 2, 1, Side::Buy, Tif::Ioc, 9'800, 1}, 8'000'000'000).ok());  // new second
}

static void test_otr() {
    Config c = test_config();
    Gate g(c);
    Ts t = 1'000'000'000;
    std::uint64_t id = 1;
    // algo 1 posts and cancels with no fills: OTR climbs to throttle (8) then breach (20) after min 30 msgs
    bool throttled_seen = false, killed = false;
    for (int k = 0; k < 200 && !killed; ++k) {
        t += 10'000'000;  // 10 ms apart: the bucket is never the constraint
        const auto d = g.submit(NewOrder{id, 1, 0, Side::Buy, Tif::Day, 9'800, 1}, t);
        g.cancel_request(id, t);
        g.on_done(id, t);
        ++id;
        throttled_seen |= g.otr_throttled(1);
        killed = g.killed_algo(1);
        (void)d;
    }
    CHECK(throttled_seen);
    CHECK(killed);
    CHECK(g.otr_orders(1) >= 20 * 1);
    CHECK(g.submit(NewOrder{id++, 1, 0, Side::Buy, Tif::Day, 9'800, 1}, t).reason == Reason::KillAlgo);
    CHECK(g.submit(NewOrder{id++, 3, 0, Side::Buy, Tif::Day, 9'800, 1}, t).ok());  // other algos unaffected
}

static void test_otr_throttle_reason() {
    Config c = test_config();
    c.otr = OtrCfg{6, 2, 1000};          // throttling switches on at the 6th message
    c.algo[1] = AlgoCfg{1, 1, 1, 1, 3};  // 1 order/s, burst 3
    Gate g(c);
    Ts t = 1'000'000'000;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        CHECK(g.submit(NewOrder{id, 1, 0, Side::Buy, Tif::Day, 9'800, 1}, t).ok());
        g.cancel_request(id, t);
        g.on_done(id, t);
    }
    // 6 messages, 0 trades: throttled (cost 2). One second later one token is back: enough at cost 1, not 2.
    CHECK(g.otr_throttled(1));
    CHECK(g.submit(NewOrder{10, 1, 0, Side::Buy, Tif::Day, 9'800, 1}, t + 1'000'000'000).reason == Reason::OtrThrottle);
    CHECK(g.submit(NewOrder{11, 1, 0, Side::Buy, Tif::Day, 9'800, 1}, t + 2'000'000'000).ok());
}

static void test_kill_switch_and_flatten() {
    Config c = test_config();
    c.flatten_on_auto_kill = 1;
    Gate g(c);
    Ts t = 1'000'000'000;
    for (std::uint64_t id = 1; id <= 6; ++id)
        CHECK(g.submit(NewOrder{id, 1, std::uint16_t(id % 2), Side::Buy, Tif::Day, 9'800, 5}, t).ok());
    g.on_fill(Fill{1, 2, 0, Side::Buy, 9'800, 5}, t);  // instr 0 long 5
    g.kill(Scope::Instrument, 1, false, t);
    {
        std::vector<std::uint64_t> c1;
        for (auto& a : g.actions()) if (a.kind == Action::Kind::Cancel) c1.push_back(a.order_id);
        CHECK((c1 == std::vector<std::uint64_t>{1, 3, 5}));
        g.actions().clear();
    }
    CHECK(g.submit(NewOrder{50, 1, 1, Side::Buy, Tif::Day, 9'800, 5}, t).reason == Reason::KillInstrument);
    CHECK(g.submit(NewOrder{51, 1, 0, Side::Buy, Tif::Day, 9'800, 5}, t).ok());
    g.kill(Scope::Global, 0, true, t);
    std::size_t cancels = 0, flats = 0;
    for (auto& a : g.actions()) {
        if (a.kind == Action::Kind::Cancel) ++cancels;
        else {
            ++flats;
            CHECK(a.order.side == Side::Sell);
            CHECK(a.order.qty == 5);
            CHECK(a.order.tif == Tif::Ioc);
            CHECK(a.order.price % 5 == 0);
            CHECK(a.order.price >= c.instr[0].band_lo);
        }
    }
    CHECK(cancels == 3);  // 4, 6, 51 (2 is filled)
    CHECK(flats == 1);
    CHECK(g.submit(NewOrder{60, 3, 2, Side::Buy, Tif::Day, 9'800, 5}, t).reason == Reason::KillGlobal);
    g.reset_kill(Scope::Global, 0, t);
    CHECK(g.submit(NewOrder{61, 3, 2, Side::Buy, Tif::Day, 9'800, 5}, t).ok());
}

static void test_auto_kills() {
    Config c = test_config();
    {
        Gate g(c);
        g.submit(NewOrder{1, 1, 0, Side::Buy, Tif::Day, 10'000, 50}, 1);
        g.on_fill(Fill{1, 1, 0, Side::Buy, 10'000, 50}, 2);
        g.on_market(0, 9'000, 3);  // -50,000: inside the 250k limit
        CHECK(!g.killed_global());
        g.submit(NewOrder{2, 1, 0, Side::Buy, Tif::Day, 9'000, 90}, 4);
        g.on_fill(Fill{2, 2, 0, Side::Buy, 9'000, 90}, 5);
        g.on_market(0, 7'500, 6);  // 140 long: -50,000 - 140 * 1,500 = -260,000 < -250,000
        CHECK(g.killed_global());
    }
    {
        Gate g(c);
        g.on_timer(5'000'000'000);  // no feed seen yet: heartbeat not armed
        CHECK(!g.killed_global());
        g.on_market(0, 10'000, 5'000'000'000);
        g.on_timer(5'900'000'000);
        CHECK(!g.killed_global());
        g.on_timer(6'000'000'001);
        CHECK(g.killed_global());
    }
}

static void test_ouch_exchange_roundtrip() {
    const Config c = test_config();
    ExchangeSim ex(c);
    std::vector<ExEvent> ev;
    ex.submit(false, 900, 0, Side::Sell, Tif::Day, 10'000, 7, 1, ev);
    unsigned char buf[ouch::kEnterOrderLen];
    const std::size_t n = ouch::encode(buf, {42, ExchangeSim::pack_symbol(c.instr[0].symbol), 10, 10'005, true, 0});
    CHECK(n == 49);
    CHECK(ex.submit_ouch(buf, n, 2, ev));
    // IOC buy 10 @ 10,005 against 7 resting @ 10,000 => fill 7 @ 10,000, trade print, Done for the rest
    int fills = 0, dones = 0, trades = 0;
    for (auto& e : ev) {
        if (e.kind == ExEvent::Kind::Fill) { ++fills; CHECK(e.fill.order_id == 42); CHECK(e.fill.qty == 7); CHECK(e.fill.price == 10'000); }
        if (e.kind == ExEvent::Kind::Done) ++dones;
        if (e.kind == ExEvent::Kind::Trade) ++trades;
    }
    CHECK(fills == 1 && dones == 1 && trades == 1);
    CHECK(ex.self_trades() == 0);
}

static void test_recon() {
    const Config c = test_config();
    AuditLog log(c);
    Gate g(c, &log);
    std::vector<TradeRow> ex;
    for (std::uint64_t k = 1; k <= 10; ++k) {
        g.submit(NewOrder{k, 1, 0, Side::Buy, Tif::Day, 9'900, 3}, k * 1'000'000);
        ex.push_back({k, k, 0, Side::Buy, 3, 9'900, k});
        if (k != 4) g.on_fill(Fill{k, k, 0, Side::Buy, 9'900, 3}, k * 1'000'000);  // fill 4 is "lost"
    }
    ex[6].price = 9'905;  // exchange printed a different price for exec 7
    const ReconResult r = reconcile(log.records(), ex);
    CHECK(r.matched == 8);
    std::size_t missing = 0, mismatch = 0, posb = 0;
    for (auto& b : r.breaks) {
        missing += b.kind == "MISSING_INTERNAL";
        mismatch += b.kind == "FIELD_MISMATCH";
        posb += b.kind == "POSITION";
    }
    CHECK(missing == 1 && mismatch == 1 && posb == 1);
}

// ------------------------------------------------------------- property test

struct PropStats {
    std::uint64_t ops = 0, orders = 0, accepted = 0;
    std::uint64_t reasons[std::size_t(Reason::kCount)] = {};
    std::uint64_t kills = 0, mass_cancels = 0, state_checks = 0;
};

static bool run_property(std::uint64_t n_ops, std::uint64_t seed, PropStats& st, AuditLog* log) {
    const Config c = test_config();
    Gate g(c, log);
    oracle::Oracle O(c);
    Rng R(seed);
    Ts t = 1'000'000'000;
    std::uint64_t next_id = 1, exec = 1;
    int fails_here = 0;
    auto fail = [&](const char* what, std::uint64_t op) {
        ++g_fail;
        if (++fails_here < 10) std::printf("PROPERTY FAIL op %llu: %s\n", (unsigned long long)op, what);
    };

    auto compare_actions = [&](std::uint64_t op) {
        std::vector<std::uint64_t> got;
        for (auto& a : g.actions()) if (a.kind == Action::Kind::Cancel) got.push_back(a.order_id);
        g.actions().clear();
        if (got != O.expected_cancels) fail("mass-cancel list differs", op);
        if (!got.empty()) { ++st.kills; st.mass_cancels += got.size(); }
    };

    for (std::uint64_t op = 0; op < n_ops; ++op) {
        // time: mostly sub-ms steps, some simultaneous bursts, rare multi-second gaps (heartbeat)
        const auto tr = R.below(10000);
        if (tr < 9000) t += R.below(200'000);
        else if (tr < 9995) {}
        else t += 500'000'000 + R.below(2'500'000'000ull);

        const auto pick = R.below(1000);
        if (pick < 550) {
            // ------------------------------------------------------- new order
            NewOrder o{};
            o.order_id = next_id++;
            const auto ir = R.below(100);
            if (ir < 2 && !O.open.empty()) o.order_id = O.open[R.below(O.open.size())].id;  // duplicate
            else if (ir < 3) o.order_id = kFlattenIdBase + R.below(5);

            const auto ar = R.below(100);
            o.algo_id = ar < 90 ? std::uint32_t(1 + R.below(5)) : ar < 94 ? 0 : ar < 97 ? 6 : 40;
            const auto nr = R.below(100);
            o.instr = nr < 93 ? std::uint16_t(R.below(4)) : nr < 97 ? 4 : 99;
            const int ii = o.instr < 4 ? o.instr : 0;
            const InstrCfg& ic = c.instr[ii];
            o.side = R.chance(5000) ? Side::Buy : Side::Sell;
            if (R.chance(50)) o.side = Side(2);
            o.tif = R.chance(7000) ? Tif::Day : Tif::Ioc;

            const Price L = O.ltp[ii];
            const auto pr = R.below(100);
            if (pr < 55) o.price = (L / 5 + R.range(-70, 70)) * 5;                  // around / across the LTP band
            else if (pr < 70) o.price = (pr & 1 ? ic.band_hi : ic.band_lo) + R.range(-2, 2) * 5;  // daily band edge
            else if (pr < 80) {                                                     // exact LTP band edge
                const Price d = L * ic.ltp_band_bp / 10000;
                o.price = ((pr & 1) ? L + d : L - d) / 5 * 5 + R.range(-1, 1) * 5;
            } else if (pr < 95 && !O.open.empty()) {                                // next to our own book (STP)
                const auto& x = O.open[R.below(O.open.size())];
                o.price = x.price + R.range(-1, 1) * 5;
                if (R.chance(6000)) o.side = opposite(x.side);
            } else if (pr < 98) o.price = L + R.range(1, 4);                        // off tick
            else o.price = -R.range(0, 5) * 5;                                      // non-positive

            const auto qr = R.below(100);
            if (qr < 60) o.qty = R.range(1, 60);
            else if (qr < 85) o.qty = R.range(85, 102);                             // qty / value edges
            else if (qr < 98) o.qty = R.range(1, 200);
            else o.qty = -R.range(0, 3);

            const Reason want = O.submit(o, t);
            const Reason got = g.submit(o, t).reason;
            ++st.orders;
            ++st.reasons[std::size_t(got)];
            if (got != want) {
                char b[160];
                std::snprintf(b, sizeof b, "order %llu: gate %s, oracle %s", (unsigned long long)o.order_id,
                              reason_name(got), reason_name(want));
                fail(b, op);
            }
            if (got == Reason::Ok) {
                ++st.accepted;
                // first principles, against the oracle's post-accept state
                const InstrCfg& c2 = c.instr[o.instr];
                const bool bands = o.price >= c2.band_lo && o.price <= c2.band_hi;
                const bool size = o.qty <= c2.max_qty && o.price * o.qty <= c2.max_value;
                const Qty worst_long = O.pos[o.instr] + O.open_qty(o.instr, Side::Buy);
                const Qty worst_short = O.pos[o.instr] - O.open_qty(o.instr, Side::Sell);
                // Only the side this order adds to: a sell may (correctly) be accepted while the
                // long side is over the limit, e.g. after a fill for an order the gate never saw.
                const bool posok = o.side == Side::Buy ? worst_long <= c2.max_pos : worst_short >= -c2.max_pos;
                bool crosses = false;
                for (auto& x : O.open)
                    if (x.instr == o.instr && x.id != o.order_id && x.side != o.side &&
                        (o.side == Side::Buy ? o.price >= x.price : o.price <= x.price))
                        crosses = true;
                if (!bands || !size || !posok || crosses || o.algo_id == 0) {
                    char b[200];
                    std::snprintf(b, sizeof b, "accepted order violates a rule: bands %d size %d pos %d (long %lld short %lld) cross %d",
                                  bands, size, posok, (long long)worst_long, (long long)worst_short, crosses);
                    fail(b, op);
                }
            }
            compare_actions(op);
        } else if (pick < 670) {
            // --------------------------------------------------- cancel request
            std::uint64_t id = next_id + 7;
            if (!O.open.empty() && R.chance(9000)) id = O.open[R.below(O.open.size())].id;
            O.cancel_request(id);
            g.cancel_request(id, t);
            compare_actions(op);
        } else if (pick < 820) {
            // ------------------------------------------------------------ fill
            if (O.open.empty()) continue;
            const auto& x = O.open[R.below(O.open.size())];
            if (x.algo == 5 && R.chance(9000)) continue;  // algo 5 rarely fills -> exercises OTR
            Fill f{exec++, x.id, x.instr, x.side, x.price, std::max<Qty>(1, R.range(1, x.leaves))};
            if (R.chance(300)) f.order_id = next_id + 1'000'000;  // fill for an order we never saw
            O.fill(f);
            g.on_fill(f, t);
            compare_actions(op);
        } else if (pick < 900) {
            // ------------------------------------------------------------ done
            if (O.open.empty()) continue;
            std::uint64_t id = O.open[R.below(O.open.size())].id;
            for (auto& x : O.open) if (x.cancel_sent && R.chance(5000)) { id = x.id; break; }
            O.done(id);
            g.on_done(id, t);
            compare_actions(op);
        } else if (pick < 970) {
            // ---------------------------------------------------------- market
            const auto i = std::uint16_t(R.below(4));
            Price p = O.ltp[i] + R.range(-6, 6) * 5;
            p = std::clamp<Price>(p, 9'000, 11'000);
            if (R.chance(100)) p = 0;  // a bad tick: must be ignored
            O.market(i, p, t);
            g.on_market(i, p, t);
            compare_actions(op);
        } else if (pick < 975) {
            O.timer(t);
            g.on_timer(t);
            compare_actions(op);
        } else {
            // ------------------------------------------------- kill / reset
            const auto k = R.below(100);
            Scope s = k < 20 ? Scope::Global : k < 60 ? Scope::Algo : Scope::Instrument;
            std::uint32_t id = s == Scope::Global ? 0 : s == Scope::Algo ? std::uint32_t(1 + R.below(5)) : std::uint32_t(R.below(4));
            if (R.chance(1500)) { O.kill(s, id); g.kill(s, id, false, t); }
            else {
                // reset something that is actually killed, so the run keeps accepting orders
                std::vector<std::pair<Scope, std::uint32_t>> killed;
                if (O.kg) killed.push_back({Scope::Global, 0});
                for (std::uint32_t a = 1; a <= 5; ++a) if (O.ka[a]) killed.push_back({Scope::Algo, a});
                for (std::uint32_t i = 0; i < 4; ++i) if (O.ki[i]) killed.push_back({Scope::Instrument, i});
                if (!killed.empty()) std::tie(s, id) = killed[R.below(killed.size())];
                O.reset(s, id);
                g.reset_kill(s, id, t);
            }
            compare_actions(op);
        }

        if ((op & 255) == 0) {
            ++st.state_checks;
            bool same = g.open_orders() == O.open.size() && g.killed_global() == O.kg;
            for (int i = 0; i < 4; ++i)
                same = same && g.position(i) == O.pos[i] && g.open_buy(i) == O.open_qty(i, Side::Buy) &&
                       g.open_sell(i) == O.open_qty(i, Side::Sell) && g.killed_instr(i) == O.ki[i] &&
                       g.ltp(i) == O.ltp[i];
            for (int a = 1; a <= 5; ++a)
                same = same && g.killed_algo(a) == O.ka[a] && g.otr_orders(a) == O.algo[a].orders;
            if (!same) fail("state diverged", op);
        }
        ++st.ops;
        if (fails_here >= 10) return false;
    }
    return fails_here == 0;
}

static void test_property_small_and_replay() {
    // A short property run with auditing on, then: chain verifies, replay is byte-identical,
    // a flipped byte breaks the chain, and a forged-but-rehashed decision fails replay.
    const Config c = test_config();
    AuditLog log(c);
    PropStats st;
    CHECK(run_property(200'000, 99, st, &log));
    const std::string path = "arg_test_audit.log";
    log.write(path);

    LoadedLog L = load_log(path);
    CHECK(L.error.empty());
    CHECK(L.first_bad == -1);
    CHECK(L.recs.size() == log.records().size());

    {
        AuditLog re(L.cfg, L.recs.size());
        Gate g(L.cfg, &re);
        for (auto& r : L.recs) if (is_input(static_cast<Rec>(r.kind))) apply_input(g, r);
        CHECK(re.records().size() == L.recs.size());
        CHECK(std::memcmp(re.records().data(), L.recs.data(), L.recs.size() * sizeof(Record)) == 0);
    }

    // find an accepted order to tamper with
    std::size_t victim = 0;
    for (std::size_t i = 0; i < L.recs.size(); ++i)
        if (static_cast<Rec>(L.recs[i].kind) == Rec::Order && L.recs[i].reason == 0 && i > 1000) { victim = i; break; }
    CHECK(victim > 0);

    {   // 1. flip a byte on disk: chain must break exactly there
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        const long off = long(8 + 4 + sizeof(Config) + victim * (sizeof(Record) + 32) + offsetof(Record, qty));
        std::fseek(f, off, SEEK_SET);
        unsigned char b = 0;
        std::fread(&b, 1, 1, f);
        b ^= 0x01;
        std::fseek(f, off, SEEK_SET);
        std::fwrite(&b, 1, 1, f);
        std::fclose(f);
        const LoadedLog T = load_log(path);
        CHECK(T.first_bad == std::int64_t(victim));
    }
    {   // 2. forge: mark an accepted order as rejected and recompute the whole chain
        AuditLog forged(L.cfg, L.recs.size());
        for (auto r : L.recs) {
            if (r.seq == victim) r.reason = std::uint8_t(Reason::RateLimit);
            Record& w = forged.append(static_cast<Rec>(r.kind), r.ts);
            w = r;
        }
        forged.write(path);
        const LoadedLog F = load_log(path);
        CHECK(F.first_bad == -1);  // the chain alone cannot see it...
        AuditLog re(F.cfg, F.recs.size());
        Gate g(F.cfg, &re);
        for (auto& r : F.recs) if (is_input(static_cast<Rec>(r.kind))) apply_input(g, r);
        std::size_t first_diff = SIZE_MAX;
        for (std::size_t i = 0; i < F.recs.size() && i < re.records().size(); ++i)
            if (std::memcmp(&F.recs[i], &re.records()[i], sizeof(Record)) != 0) { first_diff = i; break; }
        CHECK(first_diff == victim);  // ...replay can, and points at the exact record
    }
    std::remove(path.c_str());
}

int main(int argc, char** argv) {
    std::uint64_t ops = 1'000'000, seed = 1;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (!std::strcmp(argv[k], "--ops")) ops = std::strtoull(argv[k + 1], nullptr, 10);
        if (!std::strcmp(argv[k], "--seed")) seed = std::strtoull(argv[k + 1], nullptr, 10);
    }

    test_sha256();
    test_basic_rules();
    test_rate_and_ops();
    test_otr();
    test_otr_throttle_reason();
    test_kill_switch_and_flatten();
    test_auto_kills();
    test_ouch_exchange_roundtrip();
    test_recon();
    test_property_small_and_replay();
    std::printf("unit tests: %d checks, %d failures\n", g_checks, g_fail);

    PropStats st;
    const bool ok = run_property(ops, seed, st, nullptr);
    std::printf("\nproperty test: seed %llu, %llu operations, %llu orders (%llu accepted), %llu state comparisons, "
                "%llu kills (%llu mass-cancels)\n",
                (unsigned long long)seed, (unsigned long long)st.ops, (unsigned long long)st.orders,
                (unsigned long long)st.accepted, (unsigned long long)st.state_checks, (unsigned long long)st.kills,
                (unsigned long long)st.mass_cancels);
    std::printf("decision coverage (every reason must be exercised):\n");
    bool all = true;
    for (std::size_t r = 0; r < std::size_t(Reason::kCount); ++r) {
        std::printf("  %-20s %10llu\n", reason_name(Reason(r)), (unsigned long long)st.reasons[r]);
        if (st.reasons[r] == 0) all = false;
    }
    CHECK(all);
    std::printf("property test: %s -- gate and oracle agreed on every decision\n", ok ? "PASS" : "FAIL");
    std::printf("\nTOTAL: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
