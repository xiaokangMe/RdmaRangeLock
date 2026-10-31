#!/usr/bin/env python3
"""Compare the BTreeLatch update/insert path matrices of two builds.

Reads experiments/results/latch-path-<label>-*.csv, takes the median of the three
repetitions per (build, mode, threads), and prints a side-by-side table.

Usage:
    scripts/analyze_latch_path.py --old 50fd8a5 --new 03eb28e
"""

import argparse
import csv
import glob
import os
import statistics as stats
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
THREADS = [1, 2, 4, 8, 16, 32]


def load(pattern):
    rows = []
    for path in sorted(glob.glob(os.path.join(RESULTS, pattern))):
        with open(path, newline="") as handle:
            rows.extend(csv.DictReader(handle))
    return rows


def group(rows):
    table = defaultdict(list)
    for row in rows:
        table[(row["build"], row["mode"], int(row["threads"]))].append(row)
    return table


def median(table, key, field):
    values = table.get(key)
    if not values:
        return None
    return stats.median(float(v[field]) for v in values)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--old", required=True, help="label of the baseline build")
    parser.add_argument("--new", required=True, help="label of the optimised build")
    parser.add_argument("--pattern", default="latch-path-*.csv")
    args = parser.parse_args()

    rows = [r for r in load(args.pattern) if r["build"] in (args.old, args.new)]
    if not rows:
        raise SystemExit("no matching latch-path CSV found")
    table = group(rows)

    print(f"cases: {len(rows)}  builds: {args.old} vs {args.new}")
    for mode in ("update", "insert"):
        print()
        print(f"=== {mode} ===")
        print(f"{'threads':>8}{args.old + ' ops/s':>18}{args.new + ' ops/s':>18}"
              f"{'new/old':>10}{'new scaling':>14}")
        base_old = median(table, (args.old, mode, 1), "ops_per_sec")
        base_new = median(table, (args.new, mode, 1), "ops_per_sec")
        for threads in THREADS:
            old = median(table, (args.old, mode, threads), "ops_per_sec")
            new = median(table, (args.new, mode, threads), "ops_per_sec")
            if old is None or new is None:
                continue
            print(f"{threads:>8}{old:>18,.0f}{new:>18,.0f}"
                  f"{new / old:>9.2f}x{new / base_new:>13.2f}x")
        old_ratio = median(table, (args.old, mode, 32), "ops_per_sec") / base_old
        new_ratio = median(table, (args.new, mode, 32), "ops_per_sec") / base_new
        print(f"  t32/t1: {args.old} = {old_ratio:.2f}   {args.new} = {new_ratio:.2f}")

    print()
    print("=== integrity at 32 threads (median of 3) ===")
    for build in (args.old, args.new):
        for mode in ("update", "insert"):
            key = (build, mode, 32)
            if key not in table:
                continue
            print(f"{build:>10} {mode:7} validate={median(table, key, 'validate_ok'):.0f}"
                  f"  size={median(table, key, 'size_before'):,.0f}->"
                  f"{median(table, key, 'size_after'):,.0f}"
                  f"  height={median(table, key, 'height_before'):.0f}->"
                  f"{median(table, key, 'height_after'):.0f}"
                  f"  committed={median(table, key, 'committed'):,.0f}"
                  f"  failed={median(table, key, 'failed'):.0f}")

    failing = [r for r in rows if r["validate_ok"] != "1" or r["failed"] != "0"]
    print()
    print(f"cases with validate failure or failed ops: {len(failing)}")


if __name__ == "__main__":
    main()
