#!/usr/bin/env python3
"""Figures for the 10-second-window formal matrices (2026-11-01 rerun)."""

import csv
import glob
import os
import csv_schema
import statistics as st
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
OUT = os.path.join(ROOT, "figures", "formal-dur10")
THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
COLOR = {"none": "#7f7f7f", "record-2pl": "#1f77b4", "gap-lock": "#ff7f0e",
         "next-key": "#2ca02c", "hierarchical": "#d62728",
         "rdma-next-key": "#2ca02c", "rdma-hierarchical": "#d62728"}


def latest(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    if not files:
        raise SystemExit("missing " + pattern)
    with open(files[-1], newline="") as handle:
        return csv_schema.load_rows(handle)


def load(name):
    with open(os.path.join(RESULTS, name), newline="") as handle:
        return csv_schema.load_rows(handle)


def med(rows, field):
    return st.median(float(r[field]) for r in rows)


def idx(rows, keyfunc):
    out = defaultdict(list)
    for r in rows:
        out[keyfunc(r)].append(r)
    return out


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


def fig_window(local, cross):
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.2))
    for ax, (rows, title) in zip(axes, [(local, "local (756 runs)"), (cross, "cross-host (432 runs)")]):
        el = [float(r["elapsed_ns"]) / 1e9 for r in rows]
        ax.hist(el, bins=40, color="#1f77b4" if title.startswith("local") else "#d62728")
        ax.axvline(10, ls="--", lw=1.2, color="black")
        ax.set_xlabel("measured window (seconds)")
        ax.set_ylabel("runs")
        ax.set_title(title, fontsize=9)
        ax.text(0.98, 0.92, f"min {min(el):.2f}s\nmax {max(el):.1f}s",
                transform=ax.transAxes, ha="right", va="top", fontsize=7.5)
    fig.suptitle("Every case now measures for at least 10 seconds", fontsize=10)
    save(fig, "fig13_window_quality.png")


def fig_local(local):
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    table = idx([r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"],
                lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc in LOCAL_CCS:
        b = 64 if cc == "hierarchical" else 0
        xs, ys = [], []
        for t in THREADS:
            k = (cc, b, t)
            if k in table:
                xs.append(t)
                ys.append(med(table[k], "throughput_committed_txn_s"))
        if xs:
            ax.plot(xs, ys, marker="o", ms=4, lw=1.7, color=COLOR[cc], label=cc)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_xlabel("threads")
    ax.set_ylabel("committed txns / s")
    ax.set_title("Local YCSB-E (range=8): baseline holds, lock protocols still fall", fontsize=9)
    ax.legend(fontsize=7.5)
    save(fig, "fig14_local_scaling_dur10.png")


def fig_local_abort(local):
    fig, ax = plt.subplots(figsize=(7.2, 3.4))
    table = idx([r for r in local if r["workload"] == "ycsb-e" and r["range"] == "8"],
                lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc in ["record-2pl", "gap-lock", "next-key", "hierarchical"]:
        b = 64 if cc == "hierarchical" else 0
        xs, ys = [], []
        for t in THREADS:
            k = (cc, b, t)
            if k in table:
                xs.append(t)
                ys.append(100 * med(table[k], "aborted") / med(table[k], "transactions"))
        ax.plot(xs, ys, marker="o", ms=4, lw=1.7, color=COLOR[cc], label=cc)
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_xlabel("threads")
    ax.set_ylabel("aborted / attempted (%)")
    ax.set_title("Local YCSB-E abort rate saturates near 30%: real range conflicts", fontsize=9)
    ax.legend(fontsize=7.5)
    save(fig, "fig15_local_abort_dur10.png")


def fig_cross_abort(cross):
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    table = idx([r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"],
                lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for cc, b, label, color in [("rdma-next-key", 0, "rdma-next-key", "#2ca02c"),
                                ("rdma-hierarchical", 16, "hier b16", "#f0a0a0"),
                                ("rdma-hierarchical", 64, "hier b64", "#d62728"),
                                ("rdma-hierarchical", 256, "hier b256", "#8b1a1a")]:
        xs, ys = [], []
        for t in THREADS:
            k = (cc, b, t)
            if k in table:
                xs.append(t)
                ys.append(100 * med(table[k], "aborted") / med(table[k], "transactions"))
        if xs:
            ax.plot(xs, ys, marker="o", ms=4, lw=1.7, color=color, label=label)
    ax.plot(THREADS, [100 * (1 - 1.0 / t) for t in THREADS], ls=":", lw=1.2,
            color="black", label="old 1 - 1/threads artefact")
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_ylim(-2, 102)
    ax.set_xlabel("threads")
    ax.set_ylabel("aborted / attempted (%)")
    ax.set_title("Cross-host abort rate: no longer pinned to 1 - 1/threads", fontsize=9)
    ax.legend(fontsize=7)
    save(fig, "fig16_cross_abort_dur10.png")


def fig_cross_composition(cross):
    subset = [r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"
              and r["cc"] == "rdma-next-key"]
    table = idx(subset, lambda r: int(r["threads"]))
    fig, ax = plt.subplots(figsize=(7.2, 3.5))
    bottoms = [0.0] * len(THREADS)
    for field, color, label in [("lock_conflicts", "#1f77b4", "conflicts"),
                                ("lock_capacity_aborts", "#ff7f0e", "capacity"),
                                ("lock_gate_aborts", "#d62728", "gate")]:
        values = []
        for t in THREADS:
            rows = table[t]
            values.append(100 * med(rows, field) / med(rows, "transactions"))
        ax.bar([str(t) for t in THREADS], values, bottom=bottoms, color=color, label=label)
        bottoms = [b + v for b, v in zip(bottoms, values)]
    ax.set_xlabel("threads")
    ax.set_ylabel("% of attempted transactions")
    ax.set_title("rdma-next-key: capacity aborts reappear once gate leakage is fixed", fontsize=9)
    ax.legend(fontsize=7.5)
    save(fig, "fig17_cross_abort_composition.png")


def fig_old_vs_new(old, cross):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.5), sharey=True)
    new_g = idx([r for r in cross if r["workload"] == "ycsb-e" and r["range"] == "8"],
                lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    old_g = idx([r for r in old if r["workload"] == "ycsb-e" and r["range"] == "8"],
                lambda r: (r["cc"], int(r["bucket"]), int(r["threads"])))
    for ax, (cc, b, title) in zip(axes, [("rdma-next-key", 0, "rdma-next-key"),
                                         ("rdma-hierarchical", 64, "rdma-hierarchical b64")]):
        for table, ls, label, color in [(old_g, "--", "5000-txn window", "#999999"),
                                        (new_g, "-", "10-second window", COLOR[cc])]:
            xs, ys = [], []
            for t in THREADS:
                k = (cc, b, t)
                if k in table:
                    xs.append(t)
                    ys.append(med(table[k], "throughput_committed_txn_s"))
            if xs:
                ax.plot(xs, ys, ls=ls, marker="o", ms=4, lw=1.7, color=color, label=label)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_title(title, fontsize=9)
        ax.legend(fontsize=7)
    axes[0].set_ylabel("committed txns / s")
    fig.suptitle("Cross-host throughput: the old curve was flat because aborts were instant",
                 fontsize=10)
    save(fig, "fig18_old_vs_new_window.png")


def main():
    setup()
    local = latest("formal-local-20261101-*.csv")
    cross = latest("formal-cross-host-20261101-*.csv")
    old = load("formal-cross-host-20261031-191357.csv")
    print("local", len(local), "cross", len(cross), "old", len(old))
    fig_window(local, cross)
    fig_local(local)
    fig_local_abort(local)
    fig_cross_abort(cross)
    fig_cross_composition(cross)
    fig_old_vs_new(old, cross)


if __name__ == "__main__":
    main()
