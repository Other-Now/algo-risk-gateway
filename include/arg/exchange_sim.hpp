#pragma once

// A deliberately small simulated exchange: one price-time-priority limit book
// per instrument, Day and IOC orders, cancels. It exists to give the gate
// something real to talk to -- fills, cancel confirmations, trade prints -- and
// to produce the exchange-side trade file that post-trade reconciliation checks
// the gate against.
//
// Firm orders arrive as OUCH 4.2 EnterOrder bytes (the encoder is tick2trade's)
// and are decoded here, so the wire path is exercised end to end. "Noise"
// orders from other participants are injected directly.
//
// It also counts self-trades -- firm order matched against firm order. The gate
// is supposed to make that number zero; the sim checks it independently.

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "arg/config.hpp"
#include "arg/types.hpp"
#include "arg/vendor/ouch.hpp"

namespace arg {

struct ExEvent {
    enum class Kind : std::uint8_t { Fill, Done, Trade } kind;
    Fill fill;               // Fill: a firm order executed
    std::uint64_t order_id;  // Done
    std::uint16_t instr;     // Trade
    Price price;             // Trade
};

struct TradeRow {
    std::uint64_t exec_id, order_id;
    std::uint16_t instr;
    Side side;
    Qty qty;
    Price price;
    Ts ts;
};

class ExchangeSim {
public:
    explicit ExchangeSim(const Config& cfg) {
        for (int i = 0; i < kMaxInstr; ++i)
            if (cfg.instr[i].enabled) sym_to_instr_[pack_symbol(cfg.instr[i].symbol)] = std::uint16_t(i);
    }

    static std::uint64_t pack_symbol(const char* s) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<unsigned char>(s[i]);
        return v;
    }

    // Firm order on the wire. Returns false if the message does not decode.
    bool submit_ouch(const unsigned char* m, std::size_t len, Ts ts, std::vector<ExEvent>& out) {
        if (len != ouch::kEnterOrderLen || m[0] != 'O') return false;
        const std::uint64_t id = ouch::load_be64(m + 7);
        const Side side = m[15] == 'B' ? Side::Buy : Side::Sell;
        const Qty qty = ouch::load_be32(m + 16);
        const std::uint64_t sym = ouch::load_be64(m + 20);
        const Price px = ouch::load_be32(m + 28);
        const Tif tif = ouch::load_be32(m + 32) == 0 ? Tif::Ioc : Tif::Day;
        auto it = sym_to_instr_.find(sym);
        if (it == sym_to_instr_.end()) return false;
        submit(true, id, it->second, side, tif, px, qty, ts, out);
        return true;
    }

    void submit(bool firm, std::uint64_t id, std::uint16_t instr, Side side, Tif tif, Price px, Qty qty,
                Ts ts, std::vector<ExEvent>& out) {
        Book& b = books_[instr];
        Resting in{id, firm, qty};
        if (side == Side::Buy) match(b.asks, in, side, instr, ts, out, [&](Price p) { return p <= px; });
        else match(b.bids, in, side, instr, ts, out, [&](Price p) { return p >= px; });
        if (in.qty <= 0) return;
        if (tif == Tif::Ioc) {
            if (firm) out.push_back({ExEvent::Kind::Done, {}, id, instr, 0});
            return;
        }
        if (side == Side::Buy) b.bids[px].push_back(in); else b.asks[px].push_back(in);
        where_[id] = {instr, side, px};
    }

    void cancel(std::uint64_t id, std::vector<ExEvent>& out) {
        auto w = where_.find(id);
        if (w == where_.end()) return;  // already filled / gone: the cancel lost the race
        Book& b = books_[w->second.instr];
        auto erase_from = [&](auto& side_map) {
            auto lv = side_map.find(w->second.price);
            if (lv == side_map.end()) return;
            for (auto q = lv->second.begin(); q != lv->second.end(); ++q)
                if (q->id == id) {
                    if (q->firm) out.push_back({ExEvent::Kind::Done, {}, id, w->second.instr, 0});
                    lv->second.erase(q);
                    break;
                }
            if (lv->second.empty()) side_map.erase(lv);
        };
        if (w->second.side == Side::Buy) erase_from(b.bids); else erase_from(b.asks);
        where_.erase(w);
    }

    const std::vector<TradeRow>& firm_trades() const { return trades_; }
    std::uint64_t self_trades() const { return self_trades_; }

    std::size_t resting(std::uint16_t instr, Side s) const {
        auto it = books_.find(instr);
        if (it == books_.end()) return 0;
        std::size_t n = 0;
        if (s == Side::Buy) for (auto& [p, q] : it->second.bids) n += q.size();
        else for (auto& [p, q] : it->second.asks) n += q.size();
        return n;
    }

    // Best opposite-side price, 0 if empty.
    Price best(std::uint16_t instr, Side s) const {
        auto it = books_.find(instr);
        if (it == books_.end()) return 0;
        if (s == Side::Buy) return it->second.bids.empty() ? 0 : it->second.bids.begin()->first;
        return it->second.asks.empty() ? 0 : it->second.asks.begin()->first;
    }

    bool write_trades_csv(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "exec_id,order_id,instr,side,qty,price,ts\n");
        for (const auto& t : trades_)
            std::fprintf(f, "%llu,%llu,%u,%c,%lld,%lld,%llu\n", (unsigned long long)t.exec_id,
                         (unsigned long long)t.order_id, unsigned(t.instr), t.side == Side::Buy ? 'B' : 'S',
                         (long long)t.qty, (long long)t.price, (unsigned long long)t.ts);
        std::fclose(f);
        return true;
    }

private:
    struct Resting { std::uint64_t id; bool firm; Qty qty; };
    struct Book {
        std::map<Price, std::deque<Resting>, std::greater<Price>> bids;
        std::map<Price, std::deque<Resting>> asks;
    };
    struct Where { std::uint16_t instr; Side side; Price price; };

    template <class SideMap, class Crosses>
    void match(SideMap& opp, Resting& in, Side in_side, std::uint16_t instr, Ts ts, std::vector<ExEvent>& out,
               Crosses crosses) {
        while (in.qty > 0 && !opp.empty() && crosses(opp.begin()->first)) {
            auto lv = opp.begin();
            Resting& r = lv->second.front();
            const Qty q = in.qty < r.qty ? in.qty : r.qty;
            const Price px = lv->first;
            if (in.firm && r.firm) ++self_trades_;
            if (in.firm) emit_fill(in.id, instr, in_side, px, q, ts, out);
            if (r.firm) emit_fill(r.id, instr, opposite(in_side), px, q, ts, out);
            out.push_back({ExEvent::Kind::Trade, {}, 0, instr, px});
            in.qty -= q;
            r.qty -= q;
            if (r.qty == 0) {
                where_.erase(r.id);
                lv->second.pop_front();
                if (lv->second.empty()) opp.erase(lv);
            }
        }
    }

    void emit_fill(std::uint64_t id, std::uint16_t instr, Side s, Price px, Qty q, Ts ts,
                   std::vector<ExEvent>& out) {
        const std::uint64_t exec = ++exec_seq_;
        out.push_back({ExEvent::Kind::Fill, Fill{exec, id, instr, s, px, q}, id, instr, px});
        trades_.push_back({exec, id, instr, s, q, px, ts});
    }

    std::unordered_map<std::uint16_t, Book> books_;
    std::unordered_map<std::uint64_t, Where> where_;
    std::unordered_map<std::uint64_t, std::uint16_t> sym_to_instr_;
    std::vector<TradeRow> trades_;
    std::uint64_t exec_seq_ = 0;
    std::uint64_t self_trades_ = 0;
};

}  // namespace arg
