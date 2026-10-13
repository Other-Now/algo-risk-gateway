#pragma once

// Post-trade reconciliation: our view of fills (taken from the audit log -- the
// gate's own record of what it was told) against the exchange's trade file.
//
// Matching key is the exchange execution id. Breaks:
//   MISSING_INTERNAL  exchange reports a fill we never booked (dropped message)
//   MISSING_EXCHANGE  we booked a fill the exchange has no record of
//   FIELD_MISMATCH    same exec id, different order/side/qty/price
//   POSITION          net position per instrument differs between the two sides

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "arg/audit.hpp"
#include "arg/exchange_sim.hpp"

namespace arg {

struct Break {
    std::string kind;
    std::uint64_t exec_id;
    std::string detail;
};

struct ReconResult {
    std::size_t internal_fills = 0, exchange_fills = 0, matched = 0;
    std::vector<Break> breaks;
};

inline std::vector<TradeRow> read_trades_csv(const std::string& path) {
    std::vector<TradeRow> rows;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return rows;
    char line[256];
    if (!std::fgets(line, sizeof line, f)) { std::fclose(f); return rows; }  // header
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long ex, oid, ts; unsigned instr; char side; long long q, px;
        if (std::sscanf(line, "%llu,%llu,%u,%c,%lld,%lld,%llu", &ex, &oid, &instr, &side, &q, &px, &ts) == 7)
            rows.push_back({ex, oid, std::uint16_t(instr), side == 'B' ? Side::Buy : Side::Sell, q, px, ts});
    }
    std::fclose(f);
    return rows;
}

inline ReconResult reconcile(const std::vector<Record>& log, const std::vector<TradeRow>& exch) {
    ReconResult r;
    std::map<std::uint64_t, const Record*> internal;
    std::map<std::uint16_t, Qty> pos_int, pos_ex;
    for (const Record& rec : log) {
        if (static_cast<Rec>(rec.kind) != Rec::FillIn) continue;
        ++r.internal_fills;
        internal[rec.aux] = &rec;
        pos_int[rec.instr] += rec.side == 0 ? rec.qty : -rec.qty;
    }
    char buf[200];
    for (const TradeRow& t : exch) {
        ++r.exchange_fills;
        pos_ex[t.instr] += t.side == Side::Buy ? t.qty : -t.qty;
        auto it = internal.find(t.exec_id);
        if (it == internal.end()) {
            std::snprintf(buf, sizeof buf, "order %llu instr %u %c %lld @ %lld", (unsigned long long)t.order_id,
                          unsigned(t.instr), t.side == Side::Buy ? 'B' : 'S', (long long)t.qty, (long long)t.price);
            r.breaks.push_back({"MISSING_INTERNAL", t.exec_id, buf});
            continue;
        }
        const Record& i = *it->second;
        if (i.id != t.order_id || i.qty != t.qty || i.price != t.price || i.side != std::uint8_t(t.side)) {
            std::snprintf(buf, sizeof buf, "internal %lld@%lld vs exchange %lld@%lld", (long long)i.qty,
                          (long long)i.price, (long long)t.qty, (long long)t.price);
            r.breaks.push_back({"FIELD_MISMATCH", t.exec_id, buf});
        } else {
            ++r.matched;
        }
        internal.erase(it);
    }
    for (auto& [ex, rec] : internal) {
        std::snprintf(buf, sizeof buf, "order %llu qty %lld", (unsigned long long)rec->id, (long long)rec->qty);
        r.breaks.push_back({"MISSING_EXCHANGE", ex, buf});
    }
    std::map<std::uint16_t, int> instrs;
    for (auto& [i, q] : pos_int) instrs[i] = 1;
    for (auto& [i, q] : pos_ex) instrs[i] = 1;
    for (auto& [i, _] : instrs) {
        const Qty a = pos_int[i], b = pos_ex[i];
        if (a != b) {
            std::snprintf(buf, sizeof buf, "instr %u internal %lld exchange %lld (diff %lld)", unsigned(i),
                          (long long)a, (long long)b, (long long)(b - a));
            r.breaks.push_back({"POSITION", 0, buf});
        }
    }
    return r;
}

inline void print_recon(const ReconResult& r, std::FILE* out) {
    std::fprintf(out, "recon: internal fills %zu, exchange fills %zu, matched %zu, breaks %zu\n",
                 r.internal_fills, r.exchange_fills, r.matched, r.breaks.size());
    for (const Break& b : r.breaks)
        std::fprintf(out, "  BREAK %-17s exec %-8llu %s\n", b.kind.c_str(), (unsigned long long)b.exec_id,
                     b.detail.c_str());
}

}  // namespace arg
