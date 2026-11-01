#!/usr/bin/env python3
"""Plots for the local + cross-host formal matrices of the 2026-11-01 run."""

import glob
import os
import statistics as st
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_schema

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
OUT = os.path.join(ROOT, "figures", "formal-both")
THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
COLOR = {"none": "#7f7f7f", "record-2pl": "#1f77b4", "gap-lock": "#ff7f0e",
         "next-key": "#2ca02c", "hierarchical": "#d62728",
         "rdma-next-key": "#2ca02c", "rdma-hierarchical": "#d62728"}


def latest(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    if not files:
        raise SystemExit("no result file matching %s" % pattern)
    with open(files[-1], newline="") as handle:
        return csv_schema.load_rows(handle)


def med(rows, field):
    return st.median(float(r[field]) for r in rows)


def index(rows, keyfunc):
    table = {}
    for r in rows:
        table.setdefault(keyfunc(r), []).append(r)
    return table


def setup():
    plt.rcParams.update({"figure.dpi": 140, "savefig.dpi": 140, "font.size": 9,
                         "axes.grid": True, "grid.alpha": 0.25,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "legend.frameon": False})


def save(fig, name):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    fig.tight_layout()
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)


def fig_local_scaling(local):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6), sharey=True)
    for ax, (workload, rng, title) in zip(axes, [("ycsb-e", "8", "YCSB-E (range=8)"),
                                                 ("ycsb-a", "0", "YCSB-A")]):
        subset = [r for r in local if r["workload"] == workload and r["range"] == rng]
        table = index(subset, lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
        for cc in LOCAL_CCS:
            b = 64 if cc == "hierarchical" else 0
            xs, ys = [], []
            for t in THREADS:
                key = (cc, b, t)
                if key in table:
                    xs.append(t)
                    ys.append(med(table[key], "throughput_committed_txn_s"))
            if xs:
                ax.plot(xs, ys, marker="o", ms=3.5, lw=1.6, color=COLOR[cc], label=cc)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_title(title, fontsize=9)
        ax.legend(fontsize=7, loc="lower left")
    axes[0].set_ylabel("committed txns / s (median of 3)")
    fig.suptitle("Local matrix on the new code: only the lock-free baseline stays flat", fontsize=10)
    save(fig, "fig08_local_scaling_new_code.png")


def fig_cross_throughput(cross):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6), sharey=True)
    series = [("rdma-next-key", 0, "rdma-next-key"),
              ("rdma-hierarchical", 16, "hierarchical b16"),
              ("rdma-hierarchical", 64, "hierarchical b64"),
              ("rdma-hierarchical", 256, "hierarchical b256")]
    subset = [r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"]
    table = index(subset, lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for ax, field, title in ((axes[0], "throughput_committed_txn_s", "committed txns / s"),
                             (axes[1], "throughput_attempt_txn_s", "attempted txns / s")):
        for cc, b, label in series:
            xs, ys = [], []
            for t in THREADS:
                key = (cc, b, t)
                if key in table:
                    xs.append(t)
                    ys.append(med(table[key], field))
            if xs:
                ax.plot(xs, ys, marker="o", ms=3.5, lw=1.6,
                        color=COLOR[cc], alpha=0.55 if b == 16 else 1.0,
                        ls="--" if b == 256 else "-", label=label)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_title(title, fontsize=9)
        ax.legend(fontsize=7)
    fig.suptitle("Cross-host RDMA: committed throughput is flat while attempts keep rising", fontsize=10)
    save(fig, "fig09_cross_throughput.png")


def fig_cross_abort_composition(cross):
    subset = [r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"
              and r["cc"] == "rdma-next-key"]
    table = index(subset, lambda r: int(r["threads"]))

    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6))
    ax = axes[0]
    bottoms = [0.0] * len(THREADS)
    parts = [("lock_conflicts", "#1f77b4", "conflicts"),
             ("lock_capacity_aborts", "#ff7f0e", "capacity"),
             ("lock_gate_aborts", "#d62728", "gate")]
    for field, color, label in parts:
        values = []
        for t in THREADS:
            rows = table[t]
            total = med(rows, "transactions")
            values.append(100.0 * med(rows, field) / total)
        ax.bar([str(t) for t in THREADS], values, bottom=bottoms, color=color, label=label)
        bottoms = [b + v for b, v in zip(bottoms, values)]
    ax.set_xlabel("threads")
    ax.set_ylabel("% of attempted transactions")
    ax.set_title("Abort composition, rdma-next-key", fontsize=9)
    ax.legend(fontsize=7)

    ax = axes[1]
    for cc, b, label, color in [("rdma-next-key", 0, "rdma-next-key", "#2ca02c"),
                                ("rdma-hierarchical", 64, "hierarchical b64", "#d62728")]:
        xs, ys = [], []
        for t in THREADS:
            rows = [r for r in cross if r["cc"] == cc and int(r["bucket"]) == b
                    and int(r["threads"]) == t and r["workload"] == "ycsb-e"
                    and r["range"] == "8"]
            if not rows:
                continue
            xs.append(t)
            ys.append(100.0 * med(rows, "aborted") / med(rows, "transactions"))
        ax.plot(xs, ys, marker="o", ms=4, lw=1.8, color=color, label=label)
    ax.plot(THREADS, [100 * (1 - 1.0 / t) for t in THREADS], ls=":", lw=1.2,
            color="black", label="1 - 1/threads")
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_ylim(-2, 102)
    ax.set_xlabel("threads")
    ax.set_ylabel("aborted / attempted (%)")
    ax.set_title("Abort rate follows 1 - 1/threads exactly", fontsize=9)
    ax.legend(fontsize=7)
    fig.suptitle("Cross-host aborts are structural, not workload-driven", fontsize=10)
    save(fig, "fig10_cross_abort_composition.png")


def fig_local_abort(local):
    fig, ax = plt.subplots(figsize=(6.4, 3.4))
    subset = [r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"]
    table = index(subset, lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc in ["record-2pl", "gap-lock", "next-key", "hierarchical"]:
        b = 64 if cc == "hierarchical" else 0
        xs, ys = [], []
        for t in THREADS:
            key = (cc, b, t)
            if key in table:
                xs.append(t)
                ys.append(100 * med(table[key], "aborted") / med(table[key], "transactions"))
        ax.plot(xs, ys, marker="o", ms=4, lw=1.7, color=COLOR[cc], label=cc)
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_xlabel("threads")
    ax.set_ylabel("aborted / attempted (%)")
    ax.set_title("Local YCSB-E aborts are now non-zero: the phantom path is live", fontsize=9)
    ax.legend(fontsize=7)
    save(fig, "fig11_local_abort_rate.png")


def fig_t1_cost(local, cross):
    labels, values, colors = [], [], []
    for cc in LOCAL_CCS:
        rows = [r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"
                and r["cc"] == cc and int(r["threads"]) == 1]
        b = 64
        rows = [r for r in rows if int(r["bucket"]) == (b if cc == "hierarchical" else 0)]
        if rows:
            labels.append("local " + cc)
            values.append(med(rows, "throughput_committed_txn_s"))
            colors.append(COLOR[cc])
    for cc, b, label in [("rdma-next-key", 0, "cross rdma-next-key"),
                         ("rdma-hierarchical", 64, "cross rdma-hier b64")]:
        rows = [r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"
                and r["cc"] == cc and int(r["bucket"]) == b and int(r["threads"]) == 1]
        if rows:
            labels.append(label)
            values.append(med(rows, "throughput_committed_txn_s"))
            colors.append(COLOR[cc])
    fig, ax = plt.subplots(figsize=(8.4, 3.4))
    ax.bar(labels, values, color=colors)
    for i, v in enumerate(values):
        ax.text(i, v * 1.05, f"{v:,.0f}", ha="center", fontsize=7.5)
    ax.set_yscale("log")
    ax.set_ylabel("committed txns / s (t=1)")
    ax.set_title("Single-client baseline: transport cost is ~100x, not a lock-protocol effect", fontsize=9)
    plt.setp(ax.get_xticklabels(), rotation=18, ha="right", fontsize=8)
    save(fig, "fig12_t1_local_vs_cross.png")


def main():
    setup()
    local = latest("formal-local-2026*.csv")
    cross = latest("formal-cross-host-2026*.csv")
    print("local", len(local), "cross", len(cross))
    fig_local_scaling(local)
    fig_cross_throughput(cross)
    fig_cross_abort_composition(cross)
    fig_local_abort(local)
    fig_t1_cost(local, cross)


if __name__ == "__main__":
    main()
