// arg_recon: reconcile the gate's booked fills (from the audit log) against the
// exchange trade file.
//
//   arg_recon audit.log trades.csv
//
// Exit code 0 = clean, 4 = breaks found, 1 = I/O error.

#include <cstdio>

#include "arg/audit.hpp"
#include "arg/recon.hpp"

using namespace arg;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: arg_recon <audit.log> <trades.csv>\n"); return 1; }
    const LoadedLog L = load_log(argv[1]);
    if (!L.error.empty()) { std::fprintf(stderr, "error: %s\n", L.error.c_str()); return 1; }
    if (L.first_bad >= 0) std::printf("warning: audit chain broken at record %lld\n", (long long)L.first_bad);
    const auto trades = read_trades_csv(argv[2]);
    const ReconResult r = reconcile(L.recs, trades);
    print_recon(r, stdout);
    return r.breaks.empty() ? 0 : 4;
}
