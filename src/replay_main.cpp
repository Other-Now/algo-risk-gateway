// arg_replay: verify an audit log and reproduce every decision in it.
//
//   arg_replay audit.log
//
// Step 1 walks the SHA-256 chain (catches corruption / naive edits).
// Step 2 rebuilds the gate from the config embedded in the log header, feeds it
// every logged *input* in order, and compares the log it produces against the
// original byte-for-byte -- every decision, reason code, kill, mass-cancel and
// flatten order. This catches edits that kept the chain valid.
//
// Exit codes: 0 reproduced, 2 chain broken, 3 replay diverged, 1 I/O error.

#include <cstdio>
#include <cstring>

#include "arg/audit.hpp"
#include "arg/gate.hpp"

using namespace arg;

static void dump(const char* tag, const Record& r) {
    std::printf("  %s seq %llu kind %u id %llu algo %u instr %u side %u px %lld qty %lld reason %u scope %u\n", tag,
                (unsigned long long)r.seq, r.kind, (unsigned long long)r.id, r.algo, r.instr, r.side,
                (long long)r.price, (long long)r.qty, r.reason, r.scope);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: arg_replay <audit.log>\n"); return 1; }
    const LoadedLog L = load_log(argv[1]);
    if (!L.error.empty()) { std::fprintf(stderr, "error: %s\n", L.error.c_str()); return 1; }

    std::printf("records: %zu\n", L.recs.size());
    if (L.first_bad >= 0) {
        std::printf("CHAIN BROKEN at record %lld -- log has been altered or corrupted\n", (long long)L.first_bad);
        return 2;
    }
    std::printf("chain:   OK (head ");
    for (int i = 0; i < 8; ++i) std::printf("%02x", L.head[i]);
    std::printf("...)\n");

    AuditLog replayed(L.cfg, L.recs.size() + 16);
    Gate g(L.cfg, &replayed);
    std::size_t inputs = 0;
    for (const Record& r : L.recs)
        if (is_input(static_cast<Rec>(r.kind))) { apply_input(g, r); ++inputs; }

    const auto& R = replayed.records();
    const std::size_t n = R.size() < L.recs.size() ? R.size() : L.recs.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (std::memcmp(&R[i], &L.recs[i], sizeof(Record)) != 0) {
            std::printf("REPLAY DIVERGED at record %zu\n", i);
            dump("logged  ", L.recs[i]);
            dump("replayed", R[i]);
            return 3;
        }
    }
    if (R.size() != L.recs.size()) {
        std::printf("REPLAY DIVERGED: logged %zu records, replay produced %zu\n", L.recs.size(), R.size());
        return 3;
    }
    std::size_t orders = 0;
    for (const Record& r : L.recs) orders += static_cast<Rec>(r.kind) == Rec::Order;
    std::printf("replay:  OK -- %zu inputs re-fed, %zu records (%zu order decisions) reproduced byte-for-byte\n",
                inputs, R.size(), orders);
    return 0;
}
