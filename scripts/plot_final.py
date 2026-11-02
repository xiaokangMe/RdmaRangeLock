#!/usr/bin/env python3
"""Figures for the final formal matrices (local + cross-host).

Reads the newest experiments/results/formal-{local,cross-host}-*.csv and writes
PNGs to figures/final/. Every series is a median over the three repetitions.
Style constants are shared with scripts/analyze_final.py so the report and the
tables cannot drift apart.
"""

import glob
import os
import statistics as st
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_schema

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
OUT = os.path.join(ROOT, "figures", "final")
THREADS = [1, 2, 4, 8, 16, 32]

LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
RDMA_CCS = ["rdma-next-key", "rdma-hierarchical"]
WORKLOADS = ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]

COLOR = {
    "none": "#6c757d",
    "record-2pl": "#1f77b4",
    "gap-lock": "#ff7f0e",
    "next-key": "#2ca02c",
    "hierarchical": "#d62728",
    "rdma-next-key": "#2ca02c",
    "rdma-hierarchical": "#d62728",
}
LABEL = {
    "none": "no locking",
    "record-2pl": "record 2PL (v3)",
    "gap-lock": "gap lock (v4)",
    "next-key": "next-key (v5)",
    "hierarchical": "hierarchical (v6)",
    "rdma-next-key": "next-key over RDMA",
    "rdma-hierarchical": "hierarchical over RDMA",
}
WL_LABEL = {
    "ycsb-a": "YCSB-A (50% update)",
    "ycsb-e": "YCSB-E (range read + insert)",
    "tpcc-payment": "TPC-C payment",
    "tpcc-new-order": "TPC-C new-order",
}
# Bucket width chosen by --bucket auto, i.e. the value the published matrix uses.
AUTO_BUCKET = {"ycsb-e": 256}


def load_latest(pattern):
    """Newest result file that passes schema validation (see analyze_final.py)."""
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)), reverse=True)
    for path in files:
        try:
            with open(path, newline="") as handle:
                return csv_schema.load_rows(handle)
        except SystemExit as error:
            print("skipping %s: %s" % (os.path.basename(path), error), file=sys.stderr)
            continue
    raise SystemExit("no parseable result file matching %s" % pattern)


def index(rows, keys):
    table = defaultdict(list)
    for row in rows:
        table[tuple(row[k] for k in keys)].append(row)
    return table


def med(table, key, field):
    values = table.get(key)
    if not values:
        return None
    return st.median(float(v[field]) for v in values)


def save(fig, name):
    path = os.path.join(OUT, name)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print("wrote", path)
    return path


def auto_bucket(workload):
    return AUTO_BUCKET.get(workload, 16)


def fig_throughput_vs_threads(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5))
    for ax, workload in zip(axes.ravel(), WORKLOADS):
        rng = "8" if workload == "ycsb-e" else "0"
        for cc in LOCAL_CCS:
            bucket = auto_bucket(workload) if cc == "hierarchical" else 0
            values = [med(table, (workload, cc, str(bucket), rng, str(t)),
                          "throughput_committed_txn_s") for t in THREADS]
            if values[0] is None:
                continue
            ax.plot(THREADS, values, marker="o", ms=3.5, lw=1.6,
                    color=COLOR[cc], label=LABEL[cc])
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_title(WL_LABEL[workload], fontsize=9)
        ax.set_xlabel("threads")
        ax.set_ylabel("committed txn/s")
        ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
    axes[0][0].legend(fontsize=7, ncol=1, loc="lower left", framealpha=0.9)
    fig.suptitle("Local matrix: committed throughput vs threads (median of 3, 10 s window)",
                 fontsize=10)
    return save(fig, "fig01_throughput_vs_threads.png")


def fig_abort_vs_threads(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5))
    for ax, workload in zip(axes.ravel(), WORKLOADS):
        rng = "8" if workload == "ycsb-e" else "0"
        for cc in LOCAL_CCS[1:]:
            bucket = auto_bucket(workload) if cc == "hierarchical" else 0
            values = []
            for t in THREADS:
                key = (workload, cc, str(bucket), rng, str(t))
                txns = med(table, key, "transactions")
                aborted = med(table, key, "aborted")
                values.append(100 * aborted / txns if txns else None)
            if values[0] is None:
                continue
            ax.plot(THREADS, values, marker="o", ms=3.5, lw=1.6,
                    color=COLOR[cc], label=LABEL[cc])
        ax.set_xscale("log", base=2)
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_ylim(0, 100)
        ax.set_title(WL_LABEL[workload], fontsize=9)
        ax.set_xlabel("threads")
        ax.set_ylabel("aborted share (%)")
        ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
    axes[0][0].legend(fontsize=7, loc="upper left", framealpha=0.9)
    fig.suptitle("Local matrix: no-wait abort share vs threads", fontsize=10)
    return save(fig, "fig02_abort_vs_threads.png")


def fig_range_effect(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    ranges = [8, 32, 128]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    for ax, threads in zip(axes, (1, 8)):
        width = 0.16
        for i, cc in enumerate(LOCAL_CCS):
            bucket = auto_bucket("ycsb-e") if cc == "hierarchical" else 0
            values = [med(table, ("ycsb-e", cc, str(bucket), str(r), str(threads)),
                          "throughput_committed_txn_s") for r in ranges]
            if values[0] is None:
                continue
            xs = [j + i * width - 2 * width for j in range(len(ranges))]
            ax.bar(xs, values, width=width, color=COLOR[cc], label=LABEL[cc])
        ax.set_xticks(range(len(ranges)))
        ax.set_xticklabels([str(r) for r in ranges])
        ax.set_xlabel("range length")
        ax.set_ylabel("committed txn/s")
        ax.set_yscale("log")
        ax.set_title("YCSB-E, %d thread%s" % (threads, "" if threads == 1 else "s"), fontsize=9)
        ax.grid(True, axis="y", ls=":", lw=0.5, alpha=0.6)
    axes[0].legend(fontsize=7, ncol=2, framealpha=0.9)
    fig.suptitle("Longer scan: row-wise locking pays per key, hierarchical pays per bucket",
                 fontsize=10)
    return save(fig, "fig03_range_effect.png")


def fig_lock_cost(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    ranges = [8, 32, 128]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    ax = axes[0]
    for cc in LOCAL_CCS[1:]:
        bucket = auto_bucket("ycsb-e") if cc == "hierarchical" else 0
        values = []
        for r in ranges:
            key = ("ycsb-e", cc, str(bucket), str(r), "1")
            txns = med(table, key, "transactions")
            values.append(med(table, key, "lock_acquires") / txns if txns else None)
        ax.plot(ranges, values, marker="o", ms=4, color=COLOR[cc], label=LABEL[cc])
    ax.set_yscale("log")
    ax.set_xticks(ranges)
    ax.set_xlabel("range length")
    ax.set_ylabel("lock acquisitions per transaction")
    ax.set_title("Per-transaction lock traffic grows with the scan", fontsize=9)
    ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
    ax.legend(fontsize=7)

    ax = axes[1]
    for cc in LOCAL_CCS[1:]:
        bucket = auto_bucket("ycsb-e") if cc == "hierarchical" else 0
        values = []
        for r in ranges:
            key = ("ycsb-e", cc, str(bucket), str(r), "1")
            txns = med(table, key, "transactions")
            acquires = med(table, key, "lock_acquires")
            found = med(table, key, "rows_found")
            values.append(acquires / found if txns and found else None)
        ax.plot(ranges, values, marker="o", ms=4, color=COLOR[cc], label=LABEL[cc])
    ax.set_yscale("log")
    ax.axhline(1.0, ls="--", lw=1, color="black", alpha=0.5)
    ax.set_xticks(ranges)
    ax.set_xlabel("range length")
    ax.set_ylabel("locks per returned row")
    ax.set_title("Row-wise protocols pay about one lock per row", fontsize=9)
    ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
    fig.suptitle("Lock cost is the mechanism behind the range-length gap", fontsize=10)
    return save(fig, "fig04_lock_cost.png")


def fig_bucket_sweep(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    targets = [("ycsb-e", "8", "YCSB-E (range=8)"), ("ycsb-a", "0", "YCSB-A"),
               ("tpcc-payment", "0", "TPC-C payment"),
               ("tpcc-new-order", "0", "TPC-C new-order")]
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5))
    shades = {16: "#4c72b0", 64: "#dd8452", 256: "#55a868"}
    for ax, (workload, rng, title) in zip(axes.ravel(), targets):
        for bucket in (16, 64, 256):
            values = [med(table, (workload, "hierarchical", str(bucket), rng, str(t)),
                          "throughput_committed_txn_s") for t in THREADS]
            if values[0] is None:
                continue
            ax.plot(THREADS, values, marker="o", ms=3.5, lw=1.6, color=shades[bucket],
                    label="bucket=%d" % bucket)
        chosen = auto_bucket(workload)
        ax.set_title("%s  (auto picks %d)" % (title, chosen), fontsize=9)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_ylabel("committed txn/s")
        ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
        ax.legend(fontsize=7)
    fig.suptitle("v6 bucket width: wider wins for range scans, narrower for point writes",
                 fontsize=10)
    return save(fig, "fig05_bucket_sweep.png")


def fig_single_thread(rows):
    table = index(rows, ["workload", "cc", "bucket", "range", "threads"])
    fig, ax = plt.subplots(figsize=(11, 4.2))
    width = 0.16
    for i, cc in enumerate(LOCAL_CCS):
        values = []
        for workload in WORKLOADS:
            rng = "8" if workload == "ycsb-e" else "0"
            bucket = auto_bucket(workload) if cc == "hierarchical" else 0
            values.append(med(table, (workload, cc, str(bucket), rng, "1"),
                              "throughput_committed_txn_s"))
        xs = [j + i * width - 2 * width for j in range(len(WORKLOADS))]
        ax.bar(xs, values, width=width, color=COLOR[cc], label=LABEL[cc])
    ax.set_xticks(range(len(WORKLOADS)))
    ax.set_xticklabels([WL_LABEL[w] for w in WORKLOADS])
    ax.set_ylabel("committed txn/s")
    ax.set_title("Single thread: the only comparison free of lock-table queuing", fontsize=9)
    ax.grid(True, axis="y", ls=":", lw=0.5, alpha=0.6)
    ax.legend(fontsize=7, ncol=3)
    return save(fig, "fig06_single_thread.png")


def fig_cross_throughput(rows):
    table = index(rows, ["cc", "bucket", "range", "threads"])
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    for ax, cc in zip(axes, RDMA_CCS):
        bucket = auto_bucket("ycsb-e") if cc == "rdma-hierarchical" else 0
        for rng in ("8", "32", "128"):
            values = [med(table, (cc, str(bucket), rng, str(t)),
                          "throughput_committed_txn_s") for t in THREADS]
            if values[0] is None:
                continue
            ax.plot(THREADS, values, marker="o", ms=3.5, lw=1.6,
                    label="range=%s" % rng)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_ylabel("committed txn/s")
        ax.set_title(LABEL[cc], fontsize=9)
        ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
        ax.legend(fontsize=7)
    fig.suptitle("Cross-host (.66 server -> .67 client): throughput vs threads", fontsize=10)
    return save(fig, "fig07_cross_throughput.png")


def fig_cross_abort(rows):
    table = index(rows, ["cc", "bucket", "range", "threads"])
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    ax = axes[0]
    for cc in RDMA_CCS:
        bucket = auto_bucket("ycsb-e") if cc == "rdma-hierarchical" else 0
        values = []
        for t in THREADS:
            key = (cc, str(bucket), "8", str(t))
            txns = med(table, key, "transactions")
            aborted = med(table, key, "aborted")
            values.append(100 * aborted / txns if txns else None)
        ax.plot(THREADS, values, marker="o", ms=3.5, lw=1.6, color=COLOR[cc],
                label=LABEL[cc])
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_ylim(0, 100)
    ax.set_xlabel("threads")
    ax.set_ylabel("aborted share (%)")
    ax.set_title("Abort share (range=8)", fontsize=9)
    ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.6)
    ax.legend(fontsize=7)

    ax = axes[1]
    labels, conflicts, capacity, gate = [], [], [], []
    for cc in RDMA_CCS:
        bucket = auto_bucket("ycsb-e") if cc == "rdma-hierarchical" else 0
        key = (cc, str(bucket), "8", "32")
        labels.append(LABEL[cc])
        conflicts.append(med(table, key, "lock_conflicts") or 0)
        capacity.append(med(table, key, "lock_capacity_aborts") or 0)
        gate.append(med(table, key, "lock_gate_aborts") or 0)
    xs = range(len(labels))
    ax.bar(xs, conflicts, color="#c44e52", label="lock conflict")
    ax.bar(xs, capacity, bottom=conflicts, color="#dd8452", label="slot capacity")
    bottom = [c + k for c, k in zip(conflicts, capacity)]
    ax.bar(xs, gate, bottom=bottom, color="#4c72b0", label="gate")
    ax.set_xticks(list(xs))
    ax.set_xticklabels(labels, fontsize=8)
    ax.set_ylabel("aborted transactions at t=32")
    ax.set_title("Abort composition at t=32", fontsize=9)
    ax.grid(True, axis="y", ls=":", lw=0.5, alpha=0.6)
    ax.legend(fontsize=7)
    fig.suptitle("Cross-host aborts come from lock conflicts, not from the gate or the slots",
                 fontsize=10)
    return save(fig, "fig08_cross_abort.png")


def main():
    os.makedirs(OUT, exist_ok=True)
    local = load_latest("formal-local-*.csv")
    local_paths = [fig_throughput_vs_threads(local), fig_abort_vs_threads(local),
                   fig_range_effect(local), fig_lock_cost(local),
                   fig_bucket_sweep(local), fig_single_thread(local)]
    try:
        cross = load_latest("formal-cross-host-*.csv")
    except SystemExit:
        print("no cross-host data yet")
        cross = None
    if cross:
        local_paths += [fig_cross_throughput(cross), fig_cross_abort(cross)]
    print("%d figures" % len(local_paths))


if __name__ == "__main__":
    main()
