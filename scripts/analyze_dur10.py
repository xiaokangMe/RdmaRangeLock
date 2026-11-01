#!/usr/bin/env python3
"""Compare the 10-second-window formal matrices against the previous 5000-txn run."""

import csv
import glob
import os
import csv_schema
import statistics as st
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]


def load(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    if not files:
        raise SystemExit("missing " + pattern)
    with open(files[-1], newline="") as handle:
        return files[-1], csv_schema.load_rows(handle)


def med(rows, field):
    return st.median(float(r[field]) for r in rows)


def group(rows, keyfunc):
    out = defaultdict(list)
    for r in rows:
        out[keyfunc(r)].append(r)
    return out


def err(rows, field):
    """Sum a counter column across rows."""
    return sum(float(r.get(field, 0) or 0) for r in rows)


def main():
    lp, local = load("formal-local-20261101-*.csv")
    cp, cross = load("formal-cross-host-20261101-*.csv")
    _, old_cross = load("formal-cross-host-20261031-191357.csv")

    print("新的本地 :", os.path.basename(lp), len(local), "runs")
    print("新的跨主机:", os.path.basename(cp), len(cross), "runs")
    print("旧跨主机  :", len(old_cross), "runs (5000 事务窗口)")
    print()

    for name, rows in (("local", local), ("cross", cross)):
        el = sorted(float(r["elapsed_ns"]) / 1e9 for r in rows)
        print(f"[{name}] elapsed: min={el[0]:.2f}s  p10={el[len(el)//10]:.2f}s  "
              f"median={el[len(el)//2]:.2f}s  max={el[-1]:.2f}s")
        short = sum(1 for v in el if v < 9.0)
        print(f"[{name}] 短于 9 秒的 case: {short}/{len(el)}")
        print(f"[{name}] rdma_timeouts 合计: {err(rows, 'rdma_timeouts'):.0f}   "
              f"completion_errors: {err(rows, 'rdma_completion_errors'):.0f}   "
              f"capacity: {err(rows, 'lock_capacity_aborts'):.0f}   "
              f"gate: {err(rows, 'lock_gate_aborts'):.0f}   "
              f"conflicts: {err(rows, 'lock_conflicts'):.0f}")
    print()

    print("=== 跨主机：中止率是否还等于 1-1/T ===")
    g = group([r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"],
              lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    print("cc".ljust(22) + "".join(("t%d" % t).rjust(12) for t in THREADS))
    for cc in ("rdma-next-key", "rdma-hierarchical"):
        for b in ((0,) if cc == "rdma-next-key" else (16, 64, 256)):
            label = cc if b == 0 else "%s b%d" % (cc, b)
            cells = []
            for t in THREADS:
                k = (cc, b, t)
                if k not in g:
                    cells.append("-".rjust(12)); continue
                rate = 100 * med(g[k], "aborted") / med(g[k], "transactions")
                cells.append(("%.1f%%" % rate).rjust(12))
            print(label.ljust(22) + "".join(cells))
    print("理论 1-1/T".ljust(22) + "".join(("%.1f%%" % (100 * (1 - 1 / t))).rjust(12) for t in THREADS))
    print()

    print("=== 旧窗口 vs 新窗口：跨主机提交吞吐（ycsb-e range=8，b64、next-key t0）===")
    old_g = group([r for r in old_cross if r["workload"] == "ycsb-e" and r["range"] == "8"],
                  lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc, b in (("rdma-next-key", 0), ("rdma-hierarchical", 64)):
        print(" %s" % ("rdma-next-key" if b == 0 else "hier b64"))
        for t in THREADS:
            k = (cc, b, t)
            new = med(g[k], "throughput_committed_txn_s") if k in g else None
            old = med(old_g[k], "throughput_committed_txn_s") if k in old_g else None
            if new is None:
                continue
            print("   t%-3d old=%8.0f  new=%8.0f  (%+.0f%%)"
                  % (t, old or 0, new, 100 * (new - (old or 1)) / (old or 1)))
    print()

    print("=== 本地：ycsb-e range=8 提交吞吐（新窗口） ===")
    lg = group([r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"],
               lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    print("cc".ljust(16) + "".join(("t%d" % t).rjust(11) for t in THREADS))
    for cc in LOCAL_CCS:
        b = 64 if cc == "hierarchical" else 0
        cells = []
        for t in THREADS:
            k = (cc, b, t)
            cells.append(("%.0f" % med(lg[k], "throughput_committed_txn_s")).rjust(11) if k in lg else "-".rjust(11))
        print(cc.ljust(16) + "".join(cells))
    print()

    print("=== 本地：中止率（ycsb-e range=8） ===")
    print("cc".ljust(16) + "".join(("t%d" % t).rjust(10) for t in THREADS))
    for cc in LOCAL_CCS:
        b = 64 if cc == "hierarchical" else 0
        cells = []
        for t in THREADS:
            k = (cc, b, t)
            if k not in lg:
                cells.append("-".rjust(10)); continue
            cells.append(("%.1f%%" % (100 * med(lg[k], "aborted") / med(lg[k], "transactions"))).rjust(10))
        print(cc.ljust(16) + "".join(cells))
    print()

    print("=== 局部异常检查 ===")
    bad = [r for r in local + cross
           if float(r["transactions"]) == 0 or float(r["committed"]) == 0
           or float(r["elapsed_ns"]) < 5e9]
    print("事务数为 0 / 零提交 / 窗口短于 5 秒的 case:", len(bad))
    if bad[:5]:
        for r in bad[:5]:
            print("  ", r["cc"], r["workload"], r["threads"], r["elapsed_ns"])


if __name__ == "__main__":
    main()
