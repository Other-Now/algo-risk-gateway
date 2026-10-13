#!/usr/bin/env python3
"""Mutation test: does the test suite actually catch realistic risk-check bugs?

For each mutant, copy include/ to build/mutants/<name>/, make ONE source edit to
gate.hpp (a bug of the kind that ships in real risk systems), rebuild arg_tests
against the mutated header, and run it. A mutant is "killed" if the tests fail.
The oracle (tests/oracle.hpp) is never mutated.

    python tools/mutants.py [--ops 1000000]

Windows: uses MSVC through msvc_env.bat. Elsewhere: $CXX (default g++).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIN = os.name == "nt"

MUTANTS = [
    ("stp-allows-equal-price",
     "if (!in.own_asks.empty() && o.price >= in.own_asks.begin()->first) return Reason::SelfTrade;",
     "if (!in.own_asks.empty() && o.price > in.own_asks.begin()->first) return Reason::SelfTrade;"),
    ("position-ignores-open-orders",
     "if (in.pos + in.open_buy + o.qty > ic.max_pos) return Reason::PositionLimit;",
     "if (in.pos + o.qty > ic.max_pos) return Reason::PositionLimit;"),
    ("cancel-request-frees-limit",
     "it->second.cancel_sent = true;\n        const std::uint32_t a = it->second.algo;",
     "it->second.cancel_sent = true;\n"
     "        (it->second.side == Side::Buy ? ins_[it->second.instr].open_buy : ins_[it->second.instr].open_sell) -= it->second.leaves;\n"
     "        it->second.leaves = 0;\n"
     "        const std::uint32_t a = it->second.algo;"),
    ("ltp-never-updated",
     "if (instr < kMaxInstr && ltp > 0) ins_[instr].ltp = ltp;",
     "(void)instr; (void)ltp;"),
    ("bucket-clock-not-advanced",
     "as.last_refill = std::max(as.last_refill, ts);",
     "(void)ts;"),
    ("ops-cap-off-by-one",
     "if (used >= cfg_.unregistered_ops_cap) return Reason::UnregisteredOps;",
     "if (used > cfg_.unregistered_ops_cap) return Reason::UnregisteredOps;"),
    ("algo-kill-not-enforced",
     "if (kill_algo_[o.algo_id]) return Reason::KillAlgo;",
     ""),
    ("partial-fill-leaks-open-qty",
     "(o.side == Side::Buy ? in.open_buy : in.open_sell) -= used;",
     "(void)used;"),
    ("ltp-band-uses-wrong-sign",
     "const Price dev = o.price > ltp ? o.price - ltp : ltp - o.price;",
     "const Price dev = o.price - ltp;"),
    ("mass-cancel-forgets-scope",
     "(s == Scope::Algo && o.algo == id) ||",
     "(s == Scope::Algo) ||"),
]


def build(inc: Path, exe: Path) -> bool:
    src = ROOT / "tests" / "arg_tests.cpp"
    if WIN:
        # cmd.exe quoting is easier to get right inside a .bat than through subprocess
        bat = exe.with_suffix(".bat")
        bat.write_text(f'@call "{ROOT / "msvc_env.bat"}"\r\n'
                       f'@cl -nologo -std:c++20 -O2 -EHsc -D_CRT_SECURE_NO_WARNINGS -I"{inc}" "{src}" '
                       f'-Fe:"{exe}" -Fo:"{exe.with_suffix(".obj")}"\r\n')
        r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    else:
        cxx = os.environ.get("CXX", "g++")
        r = subprocess.run([cxx, "-std=c++20", "-O2", f"-I{inc}", str(src), "-o", str(exe)],
                           capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
    return r.returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ops", type=int, default=1_000_000)
    args = ap.parse_args()

    rows = []
    for name, old, new in MUTANTS:
        d = ROOT / "build" / "mutants" / name
        if d.exists():
            shutil.rmtree(d)
        shutil.copytree(ROOT / "include", d / "include")
        g = d / "include" / "arg" / "gate.hpp"
        text = g.read_text()
        if text.count(old) != 1:
            print(f"{name}: pattern not found exactly once -- mutant is stale")
            return 2
        g.write_text(text.replace(old, new))
        r = None
        # Windows Application Control on the dev box intermittently blocks freshly written
        # .exe files; a reused / alternate output name clears it.
        for exe_name in (["mut_a.exe", "mut_b.exe", "mut_c.exe"] if WIN else ["arg_tests"]):
            exe = (ROOT / "build" / exe_name) if WIN else d / exe_name
            if not build(d / "include", exe):
                print(f"{name}: build failed")
                return 2
            try:
                r = subprocess.run([str(exe), "--ops", str(args.ops)], capture_output=True, text=True, cwd=d)
                break
            except OSError as e:
                print(f"  ({exe_name} blocked: {e.strerror}; retrying under another name)", flush=True)
        if r is None:
            return 2
        out = r.stdout
        unit = re.search(r"unit tests: \d+ checks, (\d+) failures", out)
        unit_fail = int(unit.group(1)) if unit else -1
        prop_fail = "property test: FAIL" in out
        first = re.findall(r"PROPERTY FAIL op (\d+): ([^\n]*)", out)
        # the main property run prints after the unit-test summary line
        main_part = out.split("unit tests:", 1)[-1]
        first_main = re.search(r"PROPERTY FAIL op (\d+): ([^\n]*)", main_part)
        killed = r.returncode != 0
        detail = first_main.group(2)[:70] if first_main else (first[0][1][:70] if first else "")
        op = first_main.group(1) if first_main else "-"
        rows.append((name, killed, unit_fail, prop_fail, op, detail))
        print(f"{name:32s} {'KILLED' if killed else 'SURVIVED'}  unit-fails={unit_fail:<3d} "
              f"property={'FAIL' if prop_fail else 'pass'}  first-op={op}  {detail}", flush=True)

    survived = [r for r in rows if not r[1]]
    print(f"\n{len(rows) - len(survived)}/{len(rows)} mutants killed")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
