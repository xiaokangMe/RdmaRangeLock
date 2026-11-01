#!/usr/bin/env python3
"""Summarise the local + cross-host formal matrices of the 2026-11-01 run."""

import csv
import glob
import os
import csv_schema
import statistics as st
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]


def latest(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    if not files:
        raise SystemExit("no file for " + pattern)
    with open(files[-1], newline="") as handle:
        return files[-1], csv_schema.load_rows(handle)


def med(rows, field):
    return st.median(float(r[field]) for r in rows)


def group(rows, key):
    out = defaultdict(list)
    for r in rows:
        out[key(r)].append(r)
    return out


def main():
    path_local, local = latest("formal-local-2026*.csv")
    path_cross, cross = latest("formal-cross-host-2026*.csv")
    print("local :", os.path.basename(path_local), len(local), "cases")
    print("cross :", os.path.basename(path_cross), len(cross), "cases")
    print()

    for workload, rng in (("ycsb-e", "8"), ("ycsb-a", "0")):
        g = group([r for r in local if r["workload"] == workload and r["range"] == rng],
                  lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
        print("=== local %s range=%s : committed txn/s ===" % (workload, rng))
        header = "cc".ljust(14) + "".join("t%d" % t for t in THREADS).rjust(0)
        print("cc".ljust(14) + "".join(("t%d" % t).rjust(11) for t in THREADS))
        for cc in LOCAL_CCS:
            b = 64 if cc == "hierarchical" else 0
            cells = []
            for t in THREADS:
                k = (cc, b, t)
                cells.append(("%.0f" % med(g[k], "throughput_committed_txn_s")).rjust(11) if k in g else "-".rjust(11))
            print(cc.ljust(14) + "".join(cells))
        print()

    print("=== local abort rate by cc (ycsb-e range=8, t1 / t32) ===")
    g = group([r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"],
              lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc in LOCAL_CCS:
        b = 64 if cc == "hierarchical" else 0
        cells = []
        for t in (1, 32):
            k = (cc, b, t)
            if k not in g:
                cells.append("-")
                continue
            rate = 100 * med(g[k], "aborted") / med(g[k], "transactions")
            cells.append("%.1f%%" % rate)
        print("%-14s t1=%-8s t32=%s" % (cc, cells[0], cells[1]))
    print()

    print("=== cross-host: abort composition ===")
    fields = ["transactions", "committed", "aborted", "lock_conflicts",
              "lock_capacity_aborts", "lock_gate_aborts"]
    totals = {f: sum(float(r[f]) for r in cross) for f in fields}
    for f in fields:
        print("  %-22s %15.0f" % (f, totals[f]))
    aborted = max(1.0, totals["aborted"])
    print("  abort rate             %14.1f%%" % (100 * totals["aborted"] / totals["transactions"]))
    print("  conflicts / aborted    %14.1f%%" % (100 * totals["lock_conflicts"] / aborted))
    print("  capacity  / aborted    %14.1f%%" % (100 * totals["lock_capacity_aborts"] / aborted))
    print("  gate      / aborted    %14.1f%%" % (100 * totals["lock_gate_aborts"] / aborted))
    print()

    print("=== cross-host: by cc and threads (committed txn/s, abort%) ===")
    gx = group([r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"],
               lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    print("cc".ljust(20) + "".join(("t%d" % t).rjust(16) for t in THREADS))
    for cc in ("rdma-next-key", "rdma-hierarchical"):
        for b in ((0,) if cc == "rdma-next-key" else (16, 64, 256)):
            label = cc if b == 0 else "%s b%d" % (cc, b)
            cells = []
            for t in THREADS:
                k = (cc, b, t)
                if k not in gx:
                    cells.append("-".rjust(16))
                    continue
                tput = med(gx[k], "throughput_committed_txn_s")
                rate = 100 * med(gx[k], "aborted") / med(gx[k], "transactions")
                cells.append(("%.0f (%d%%)" % (tput, rate)).rjust(16))
            print(label.ljust(20) + "".join(cells))
    print()

    print("=== cross-host: t1 throughput sanity ===")
    for cc in ("rdma-next-key", "rdma-hierarchical"):
        for b in ((0,) if cc == "rdma-next-key" else (64,)):
            k = (cc, b, 1)
            if k in gx:
                rows = gx[k]
                print("  %-18s t1 committed=%.0f aborted=%.0f tput=%.0f"
                      % (cc, med(rows, "committed"), med(rows, "aborted"),
                         med(rows, "throughput_committed_txn_s")))


if __name__ == "__main__":
    main()
