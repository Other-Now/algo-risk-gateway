# algo-risk-gateway

A pre-trade risk and compliance gate for algorithmic orders, in C++20. It sits between the strategies and the exchange gateway. Every order passes ten checks in about **180 ns p50 / 370 ns p99 / 570 ns p99.9**. Every decision goes into a SHA-256 hash-chained audit log, and a replay tool reproduces that log byte-for-byte.

```
 strategies ──NewOrder──▶ ┌───────────────── gate ─────────────────┐ ──OUCH bytes──▶ exchange (sim)
 (algo-tagged)            │ sanity · algo-id · kill · price bands  │                    │
                          │ qty/value · worst-case position · STP  │ ◀──fills/done──────┤
 kill commands ─────────▶ │ SEBI 10-OPS cap · token bucket · OTR   │ ◀──trade prints────┘
 timer / feed  ─────────▶ └───────┬────────────────────────────────┘
                                  │ every input + every decision (64-byte records)
                                  ▼
                       audit.log (SHA-256 chain) ──▶ arg_replay (byte-for-byte)
                                  └───────────────▶ arg_recon ◀── exchange trades.csv
```

## Results

All numbers are from an Intel i5-9300H laptop running Windows 11, built with MSVC 19.44 `/O2`. They are not from an isolated or tuned server; see [Measurement notes](#measurement-notes).

| What | Result |
|---|---|
| Whole gate, `submit()` incl. audit append | **176 ns p50, 367 ns p99, 569 ns p99.9** (2M orders, all 10 checks run) |
| Gate + OUCH encode (accept → bytes in send buffer) | 175 ns p50, 367 ns p99, 572 ns p99.9 |
| The checks themselves (kill, bands, qty/value, position, STP, rate+OTR) | **0.8–7.5 ns p50 each, ~23 ns combined** |
| Bookkeeping (`commit`: open-order table + STP price levels) | 129 ns p50, **~75% of the gate** |
| Property test: gate vs. independent oracle | **33M random orders (4.4M accepted), 3 seeds × 20M operations: 0 disagreements** |
| Mutation test: realistic bugs planted in the gate | **10/10 caught** (9 within ~550 random operations; the slowest at ~5,400) |
| Replay of a 221,843-record session log | Byte-identical; a flipped byte breaks the chain at that record; a forged-and-rehashed decision fails replay at that record |
| Determinism across compilers | Same session → **identical SHA-256 head digest on MSVC, GCC and Clang** (CI) |
| Kill switch, 1,000 resting orders | Blocked in 72 µs; **quiet (all cancels confirmed) in 350 µs**; 0 orders accepted after the kill |
| Simulated session (5 algos, 6 instruments, 10 s) | 0 self-trades reached the exchange; 0 orders accepted while killed; recon found exactly the 3 injected dropped fills |

## The checks

They run in this order, cheapest and most likely to reject first. The latencies come from the [benchmark](results/bench.txt), measured on accepted orders so that every check runs.

| # | Check | Rule | Reason code(s) | p50 / p99 ns |
|---|---|---|---|---|
| 1 | Sanity | Known instrument, qty > 0, price > 0, on tick, order id not already open | `BAD_INSTRUMENT` `BAD_ORDER` `DUPLICATE_ORDER_ID` | 10.0 / 28.3 † |
| 2 | Algo ID | Tag present and in the table of algos with an exchange-issued id | `ALGO_ID_MISSING` `ALGO_UNKNOWN` | 17.5 / 82.5 † |
| 3 | Kill switch | Global, per-algo, per-instrument | `KILL_GLOBAL` `KILL_ALGO` `KILL_INSTRUMENT` | 0.8 / 1.7 |
| 4 | Price bands | Daily range (prev close ± circuit %), and ≤ N bp from the last traded price | `PRICE_BAND_DAILY` `PRICE_BAND_LTP` | 0.8 / 1.7 |
| 5 | Size | Max qty; max value (`price × qty`, overflow-safe because qty is bounded first) | `MAX_QTY` `MAX_VALUE` | 0.8 / 1.7 |
| 6 | Position | **Worst case**: position + every open order on that side filling | `POSITION_LIMIT` | 3.3 / 5.8 |
| 7 | Self-trade | Would cross one of our own resting orders (firm-wide, across algos) | `SELF_TRADE` | 0.8 / 2.5 |
| 8 | Rate + OTR | Unregistered algos: ≤ 10 orders per calendar second. Everyone: integer token bucket. Cost doubles when OTR nears a penalty slab | `UNREGISTERED_OPS` `RATE_LIMIT` `OTR_THROTTLE` | 7.5 / 13.3 |
| – | Commit | Consume token, insert open order, update STP levels | – | 129.2 / 296.6 |
| – | Audit append | 64-byte record copy into a pre-faulted buffer | – | 5.8 / 15.0 |

† The non-serialising `rdtsc` lets the open-order hash lookup in stage 1 retire after the probe, so part of its cost lands in stage 2. Read the two stages together: about 27 ns, which is one `unordered_map::find`.

**Design rules, and why each exists:**
- **`check()` is read-only; `commit()` mutates.** An order rejected by the position check must not have consumed a rate token. It never reached the exchange.
- **Open orders count at worst case until the exchange confirms they're gone, not when we *send* the cancel.** A cancel can lose the race against a fill. (One planted bug frees the limit on the cancel request; the property test catches it within 256 operations.)
- **Cancels are never blocked.** A risk layer that stops you getting *out* of the market is worse than none.
- **The gate never reads a clock.** Time is an input. That one rule makes the gate a deterministic function of its inputs, which is what makes byte-exact replay possible.
- **All integer.** Prices in paise, quantities in lots, tokens in nano-tokens (1 order = 10⁹). No floating point touches a decision, so replay is exact across compilers.

## Kill switch

There are four triggers: a **manual command** (any scope), the **loss limit** (global mark-to-market PnL), an **OTR breach** (per algo), and **lost feed heartbeat** (no market data for N ms). On a trigger the gate:
1. sets the flag, so every new order in scope is rejected;
2. emits a mass-cancel for every resting order in scope, sorted by id so the log doesn't depend on hash-map order;
3. optionally **flattens**: marketable IOCs at the edge of the LTP band, in `max_qty` clips.

Kills are sticky until an explicit reset, and the reset is logged too.

| Resting orders | time-to-block | time-to-quiet | Orders accepted after kill |
|---:|---:|---:|---:|
| 10 | 2.8 µs | 7.2 µs | 0 |
| 100 | 6.3 µs | 32 µs | 0 |
| 1,000 | 72 µs | 350 µs | 0 |
| 10,000 | 0.69 ms | 3.5 ms | 0 |

*time-to-quiet* runs until the exchange has confirmed every cancel and the gate holds zero open orders. The exchange here is in-process; on a real venue the network round trip and the exchange's cancel processing dominate. **Known limitation:** time-to-block is O(resting orders), because the mass-cancel list is generated inside the same `kill()` call, and a single-threaded gate can't evaluate the next order until it returns. The fix is to arm the flag and return, then stream cancels on the next loop turn. Not done; see [Next steps](#what-this-is-not).

## Audit trail and replay

Every **input** (order, cancel request, fill, done, market tick, timer tick, kill command) and every **output** (decision + reason code, kill trigger, risk cancel, flatten order) is one fixed 64-byte record. The file is:

```
"ARGAUD01" | config bytes | (record, SHA-256(prev_digest ‖ record))*
```

The risk limits are embedded in the header, so the replay tool rebuilds the exact gate that wrote the log, with no config file to drift. There are two independent layers:

1. **The chain** catches corruption and naive edits. Flip one byte and `arg_replay` reports `CHAIN BROKEN at record N`.
2. **Replay** catches edits that kept the chain valid. Because the gate is deterministic, re-feeding the logged inputs must regenerate every logged output. The tests forge an accepted order into a `RATE_LIMIT` reject and recompute the entire chain. The chain verifies, and replay reports the exact record that diverged.

```
$ arg_replay results/audit.log
records: 221843
chain:   OK (head cd8267ffb276462d...)
replay:  OK -- 221808 inputs re-fed, 221843 records (8309 order decisions) reproduced byte-for-byte
```

**Hashing is off the order path.** Appending copies 64 bytes into a buffer, and the chain is computed when the log is sealed. That turned out to matter. SHA-256 chaining runs at **1.57M records/s** (635 ns/record), but the gate sustains about **3M orders/s** on one core. At peak, inline hashing would be the bottleneck. A production system would hash on a separate thread, or chain per batch rather than per record.

## Post-trade reconciliation

`arg_recon` matches the gate's booked fills (read from its own audit log) against the exchange trade file by execution id. It reports `MISSING_INTERNAL`, `MISSING_EXCHANGE` and `FIELD_MISMATCH` breaks, plus a net `POSITION` break per instrument.

The simulated session deliberately drops 3 exchange fills before they reach the gate. Recon finds exactly those 3, plus the position breaks they cause:

```
recon: internal fills 2307, exchange fills 2310, matched 2307, breaks 5
  BREAK MISSING_INTERNAL  exec 331      order 550 instr 4 S 3 @ 81935
  BREAK MISSING_INTERNAL  exec 662      order 1216 instr 3 S 8 @ 24600
  BREAK MISSING_INTERNAL  exec 993      order 2268 instr 3 S 14 @ 24740
  BREAK POSITION          exec 0        instr 3 internal 0 exchange -22 (diff -22)
  BREAK POSITION          exec 0        instr 4 internal 6 exchange 3 (diff -3)
```

**The finding worth remembering:** in the same session, the feed outage triggered a heartbeat kill with flatten. The gate flattened NATGAS to **0 by its own books**, while the exchange shows **−22**. A flatten is computed from the gate's position, so a dropped fill means the "flat" book still carries real risk, and only reconciliation sees it. COPPER also shows a separate limitation: the single IOC flatten sweep didn't fully fill in a thin book (the gate still shows +6). A production flatten would work the residual.

## Property test and mutation test

`tests/oracle.hpp` is a second implementation of the rules. It shares the spec (rule order, reason codes) with the gate but **none of the bookkeeping**:

| gate | oracle |
|---|---|
| `unordered_map` of open orders | `vector`, scanned on every call |
| Running `open_buy` / `open_sell` totals | Summed from the vector on demand |
| `std::map` price→count for STP | min/max found by scanning |
| Incremental token bucket | Closed-form arrival curve: `A_k = min(B, B + r·t_k − C_{k−1} + min_{m<k}(C_{m−1} − r·t_m))` |

The test drives both with the same random stream. That stream is about 60% orders, plus cancels, partial fills, done reports, market ticks, timer ticks with heartbeat gaps, and kill/reset commands. Orders are biased onto the boundaries: exact band edges, qty around `max_qty`, value around `max_value`, a price one tick either side of our own resting orders, bursts with zero time between them, duplicate ids, untagged orders, off-tick and non-positive prices. After **every** operation it asserts:
- the same decision **and the same reason code**;
- the same mass-cancel list on every kill;
- every 256 ops, identical positions, open quantities, LTPs, OTR counters and kill state;
- from first principles, independent of both implementations: every accepted order is inside the bands and size limits, inside the worst-case position limit on the side it adds to, and doesn't cross our own book.

```
property test: seed 1, 18225332 operations, 10997020 orders (1468908 accepted), 71182 state comparisons, 26258 kills (51846 mass-cancels)
property test: PASS -- gate and oracle agreed on every decision
```

Every reason code fires thousands of times per run ([full output](results/property_20M_seed1.txt)); the run fails if any never fires. Seeds 1–3 at 20M operations each: 32,993,938 orders, 4,399,538 accepted, 0 disagreements.

**A passing test is only evidence if it can fail.** [`tools/mutants.py`](tools/mutants.py) copies the headers, plants one bug in `gate.hpp`, rebuilds and runs the suite. It repeats this for 10 bugs of the kind that ship in real risk systems ([results](results/mutants.txt)):

| Mutant | Caught after (random ops) | First failure |
|---|---:|---|
| STP lets an equal price through (`>=` → `>`) | 123 | gate OK, oracle SELF_TRADE |
| Position check ignores open orders | 5,418 | gate RATE_LIMIT, oracle POSITION_LIMIT |
| Cancel *request* frees the limit | 256 | state diverged |
| LTP never updated (stale band) | 54 | gate OK, oracle PRICE_BAND_LTP |
| Token bucket clock never advanced | 65 | gate OK, oracle RATE_LIMIT |
| 10-OPS cap off by one | 361 | gate OK, oracle UNREGISTERED_OPS |
| Algo kill not enforced | 547 | gate PRICE_BAND_LTP, oracle KILL_ALGO |
| Partial fill leaks open qty | 163 | gate POSITION_LIMIT, oracle OK |
| LTP band ignores sign of deviation | 138 | gate OK, oracle PRICE_BAND_LTP |
| Algo-scoped kill cancels every algo's orders | 541 | mass-cancel list differs |

The slowest catch is the most dangerous bug: ignoring open orders in the position check. It only shows once enough open orders pile up near the limit, which is exactly why it survives code review and short tests in real systems.

## Regulatory context (India)

The rules are modelled on SEBI/NSE requirements. The **values are illustrative**; real limits come from the broker's risk policy and current exchange circulars.

- **Algo ID tagging.** Under SEBI's retail algo framework (Feb 2025 circular, implemented by exchanges and brokers in phases from 2025), algo orders carry a unique exchange-issued identifier. Client-built algos above **10 orders per second per exchange** must be registered through the broker. The gate rejects untagged and unknown tags, and caps unregistered algos at 10 per calendar second. *(The gate holds the exchange id in its algo table; OUCH has no algo-id field, see below.)*
- **Order-to-trade ratio.** Penalties are per algo order, on slabs at OTR 50 / 250 / 500 / 1000 / 2000. A member at OTR ≥ 2000 on a third instance within a rolling 30 days is barred from placing orders for the **first 15 minutes** of the next trading day (SEBI circular 2018/62 as amended in 2020). The gate throttles at 80% of the first slab (cost doubles at OTR 40 in the sim) and kills the algo at a configured slab (250 in the sim). Real exchange OTR definitions have exclusions that aren't modelled here.

## What this is not

- **Not connected to a real venue.** The exchange is a ~200-line price-time matching engine (`exchange_sim.hpp`). It's there to produce fills, cancel races, trade prints and a trade file to reconcile against. OUCH 4.2 is a stand-in wire format; Indian exchanges use their own binary protocols, which carry the algo id.
- **Single-threaded, single-process.** The audit buffer is in memory until sealed, so a crash loses unsealed records. Production would use an SPSC ring to a writer thread with an fsync policy.
- **The chain is tamper-evident, not tamper-proof.** Someone who can rewrite both inputs and outputs consistently and rehash can forge a log. The defence is anchoring the head digest somewhere they can't write (WORM storage, a periodic digest sent to compliance).
- **Positions are firm-level per instrument;** there are no per-algo position limits. The loss limit is global.
- **Next steps, in order:** (1) block-then-cancel kill to make time-to-block O(1); (2) replace the node-based containers in `commit` with a pre-allocated slab + open addressing (that's where 75% of the gate time is); (3) a calendar-spread executor (passive leg, IOC hedge on fill, legging-risk limit); not built.

## Reuse

- **[tick2trade](https://github.com/Other-Now/tick2trade):** the OUCH 4.2 encoder and the HdrHistogram-style recorder, vendored in `include/arg/vendor/`. The only changes are portability (no `__builtin_*`, so it builds on MSVC) and a TIF field. tick2trade's probe-effect finding is also why the benchmark uses plain `rdtsc` and reports a whole-gate number next to the per-check ones.
- **[execution-algo-lab](https://github.com/Other-Now/execution-algo-lab)** was the planned source for the exchange simulator. It wasn't reused: it replays a fixed LOBSTER tape in which our orders never interact with each other or get cancelled out from under us. Self-trade prevention, cancel races and mass-cancel confirmation all need a book that our own orders live in.

## Measurement notes

- Per-check numbers come from a TSC read at every stage boundary, with the median empty-interval cost (5.8 ns) subtracted. Their sum (182 ns p50) matches the whole-gate single-probe number (176 ns p50), so the probe effect is small here.
- **Pre-faulting the audit buffer** took the audit-append p99.9 from **9,446 ns to 24 ns**. `vector::reserve` reserves address space, and the first write to each 4 KB page faulted on the order path (1 append in 64). The `AuditLog` constructor now touches the whole buffer up front.
- Three pinned runs (`start /high /affinity 4`). Two agree within ~5% at p50 and p99 (published: run 3). One run was disturbed: whole-gate p50 doubled across every section at once, which points at the machine, not the code. On a laptop this is normal; quote the range, not the best run.

## Build and run

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/arg_tests --ops 20000000          # unit + property test
./build/arg_sim --out results             # simulated session -> audit.log, trades.csv, sim_summary.txt
./build/arg_replay results/audit.log      # verify chain + replay
./build/arg_recon results/audit.log results/trades.csv
./build/arg_bench --out results/bench.txt
python3 tools/mutants.py                  # mutation test
```

On Windows, `build_msvc.bat` builds everything with MSVC directly (no CMake needed) into `build\`. `cmake` with the Visual Studio generator works too. CI runs GCC, Clang, ASan+UBSan and MSVC.

```
include/arg/
  types.hpp config.hpp      orders, reason codes, POD risk limits
  gate.hpp                  the gate: checks, commit, kill switch, flatten
  audit.hpp sha256.hpp      64-byte records, SHA-256 chain, loader
  exchange_sim.hpp          price-time matching engine, OUCH decode, trade file
  recon.hpp                 reconciliation
  vendor/                   ouch.hpp, histogram.hpp (from tick2trade)
src/      arg_sim, arg_replay, arg_recon
bench/    arg_bench
tests/    arg_tests (unit + property), oracle.hpp
tools/    mutants.py
results/  bench.txt, sim_summary.txt, property_20M_seed1.txt, mutants.txt
```
