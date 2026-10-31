#!/usr/bin/env python3
"""Plot the formal performance matrix produced by scripts/run_formal_matrix.sh.

Reads experiments/results/formal-<mode>-*.csv, aggregates the 3 repetitions by
median, and writes PNG figures into figures/formal/ plus a summary markdown.

Usage:
    scripts/plot_formal_matrix.py [--mode local] [--csv PATH] [--out DIR]
"""

import argparse
import csv
import glob
import os
import statistics as stats
from collections import defaultdict

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    raise SystemExit(
        "缺少 matplotlib，无法绘图。\n"
        "安装：python3 -m pip install -r scripts/requirements.txt"
    )

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CC_ORDER = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
CC_COLOR = {
    "none": "#7f7f7f",
    "record-2pl": "#1f77b4",
    "gap-lock": "#ff7f0e",
    "next-key": "#2ca02c",
    "hierarchical": "#d62728",
}
CC_LABEL = {
    "none": "none (no lock)",
    "record-2pl": "record-2pl",
    "gap-lock": "gap-lock",
    "next-key": "next-key",
    "hierarchical": "hierarchical (b{})",
}
WORKLOAD_LABEL = {
    "ycsb-a": "YCSB-A (50% update)",
    "ycsb-e": "YCSB-E (range + insert)",
    "tpcc-payment": "TPC-C payment",
    "tpcc-new-order": "TPC-C new-order",
}
THREADS = [1, 2, 4, 8, 16, 32]


def load(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def number(row, field):
    return float(row[field])


def index_rows(rows):
    table = defaultdict(list)
    for row in rows:
        key = (
            row["workload"],
            row["cc"],
            int(row["bucket"]),
            int(row["range"]),
            int(row["threads"]),
        )
        table[key].append(row)
    return table


def median(table, key, field):
    group = table.get(key)
    if not group:
        return None
    return stats.median(number(row, field) for row in group)


def setup():
    plt.rcParams.update({
        "figure.dpi": 140,
        "savefig.dpi": 140,
        "font.size": 9,
        "axes.grid": True,
        "grid.alpha": 0.25,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "legend.frameon": False,
    })


def save(fig, out_dir, name):
    path = os.path.join(out_dir, name)
    fig.tight_layout()
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)
    return path


def pick_bucket(table, workload, cc, rng):
    """Hierarchical has three bucket widths; use 64 for cross-cc comparison."""
    for candidate in (64, 16, 256):
        if (workload, cc, candidate, rng, 1) in table:
            return candidate
    return 0


def fig_scaling(table, out_dir):
    """Throughput vs thread count, one panel per workload, at range=8."""
    panels = [("ycsb-a", 0), ("ycsb-e", 8), ("tpcc-payment", 0), ("tpcc-new-order", 0)]
    fig, axes = plt.subplots(1, 4, figsize=(15, 3.4), sharey=True)
    for ax, (workload, rng) in zip(axes, panels):
        for cc in CC_ORDER:
            bucket = pick_bucket(table, workload, cc, rng)
            xs, ys = [], []
            for threads in THREADS:
                value = median(table, (workload, cc, bucket, rng, threads),
                               "throughput_committed_txn_s")
                if value is not None:
                    xs.append(threads)
                    ys.append(value)
            if not xs:
                continue
            label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
            ax.plot(xs, ys, marker="o", ms=3.5, lw=1.6, color=CC_COLOR[cc], label=label)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_title(WORKLOAD_LABEL[workload], fontsize=9)
        ax.set_xlabel("threads")
    axes[0].set_ylabel("committed txns / s (median of 3)")
    axes[0].legend(fontsize=7, loc="lower left")
    fig.suptitle("Committed throughput vs thread count (log-log); lock protocols collapse", fontsize=10)
    return save(fig, out_dir, "fig01_throughput_vs_threads.png")


def fig_abort(table, out_dir):
    """Abort ratio vs thread count for the two contention-heavy workloads."""
    panels = [("ycsb-a", 0), ("tpcc-new-order", 0)]
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4), sharey=True)
    for ax, (workload, rng) in zip(axes, panels):
        for cc in CC_ORDER:
            bucket = pick_bucket(table, workload, cc, rng)
            xs, ys = [], []
            for threads in THREADS:
                key = (workload, cc, bucket, rng, threads)
                total = median(table, key, "transactions")
                aborted = median(table, key, "aborted")
                if total is None or not total:
                    continue
                xs.append(threads)
                ys.append(100.0 * aborted / total)
            if not xs:
                continue
            label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
            ax.plot(xs, ys, marker="o", ms=3.5, lw=1.6, color=CC_COLOR[cc], label=label)
        ax.set_xscale("log", base=2)
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_ylim(-2, 102)
        ax.set_title(WORKLOAD_LABEL[workload], fontsize=9)
        ax.set_xlabel("threads")
    axes[0].set_ylabel("aborted / attempted (%)")
    axes[0].legend(fontsize=7, loc="upper left")
    fig.suptitle("No-wait abort ratio: this is conflict, not blocking time", fontsize=10)
    return save(fig, out_dir, "fig02_abort_ratio_vs_threads.png")


def fig_range(table, out_dir):
    """YCSB-E: how range length changes cost, at t=1 and t=4."""
    ranges = [8, 32, 128]
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.4), sharey=True)
    for ax, threads in zip(axes, (1, 4)):
        width = 0.16
        positions = range(len(ranges))
        for offset, cc in enumerate(CC_ORDER):
            bucket = pick_bucket(table, "ycsb-e", cc, 8)
            xs, ys = [], []
            for i, rng in enumerate(ranges):
                value = median(table, ("ycsb-e", cc, bucket, rng, threads),
                               "throughput_committed_txn_s")
                if value is None:
                    continue
                xs.append(i + (offset - 2) * width)
                ys.append(value)
            label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
            ax.bar(xs, ys, width=width, color=CC_COLOR[cc], label=label)
        ax.set_xticks(list(positions))
        ax.set_xticklabels([f"range={r}" for r in ranges])
        ax.set_title(f"threads = {threads}", fontsize=9)
        ax.set_yscale("log")
    axes[0].set_ylabel("committed txns / s")
    axes[0].legend(fontsize=7, ncol=2)
    fig.suptitle("YCSB-E: range length dominates cost, and locks amplify it", fontsize=10)
    return save(fig, out_dir, "fig03_range_length_effect.png")


def fig_locks(table, out_dir):
    """Lock acquires vs range length at t=1, and rechecked rows."""
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.4))
    ranges = [8, 32, 128]
    width = 0.2
    ax = axes[0]
    for offset, cc in enumerate(["record-2pl", "gap-lock", "next-key", "hierarchical"]):
        bucket = pick_bucket(table, "ycsb-e", cc, 8)
        xs, ys = [], []
        for i, rng in enumerate(ranges):
            value = median(table, ("ycsb-e", cc, bucket, rng, 1), "lock_acquires")
            if value is None:
                continue
            xs.append(i + (offset - 1.5) * width)
            ys.append(value)
        label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
        ax.bar(xs, ys, width=width, color=CC_COLOR[cc], label=label)
    ax.set_xticks(range(len(ranges)))
    ax.set_xticklabels([f"range={r}" for r in ranges])
    ax.set_ylabel("lock acquires per run")
    ax.set_title("Lock acquisitions (threads=1, median of 3)", fontsize=9)
    ax.legend(fontsize=7)

    ax = axes[1]
    for cc in ["record-2pl", "gap-lock", "next-key", "hierarchical"]:
        bucket = pick_bucket(table, "ycsb-e", cc, 8)
        xs, ys = [], []
        for rng in ranges:
            value = median(table, ("ycsb-e", cc, bucket, rng, 1), "lock_acquires")
            found = median(table, ("ycsb-e", cc, bucket, rng, 1), "rows_found")
            if value is None or not found:
                continue
            xs.append(rng)
            ys.append(value / found)
        if not xs:
            continue
        label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
        ax.plot(xs, ys, marker="o", ms=4, color=CC_COLOR[cc], label=label)
    ax.axhline(1.0, ls="--", lw=1, color="black", alpha=0.6)
    ax.set_xticks(ranges)
    ax.set_yscale("log")
    ax.set_xlabel("range length")
    ax.set_ylabel("locks per returned row")
    ax.set_title("Locks paid per row returned (threads=1)", fontsize=9)
    ax.legend(fontsize=7)
    return save(fig, out_dir, "fig04_lock_and_scan_cost.png")


def fig_bucket(table, out_dir):
    """Hierarchical bucket width sweep."""
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.4))
    for ax, (workload, rng) in zip(axes, [("ycsb-e", 8), ("ycsb-a", 0)]):
        for bucket, color in [(16, "#8c564b"), (64, "#d62728"), (256, "#e377c2")]:
            xs, ys = [], []
            for threads in THREADS:
                value = median(table, (workload, "hierarchical", bucket, rng, threads),
                               "throughput_committed_txn_s")
                if value is None:
                    continue
                xs.append(threads)
                ys.append(value)
            if xs:
                ax.plot(xs, ys, marker="o", ms=3.5, lw=1.6, color=color, label=f"bucket={bucket}")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_title(f"hierarchical, {WORKLOAD_LABEL[workload]}", fontsize=9)
        ax.legend(fontsize=7)
    axes[0].set_ylabel("committed txns / s")
    fig.suptitle("Bucket width sweep: coarser buckets cut lock count but add conflicts", fontsize=10)
    return save(fig, out_dir, "fig05_bucket_sweep.png")


def fig_single_thread(table, out_dir):
    """Single-thread cost, the only dimension free of mutex queuing."""
    workloads = ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]
    fig, ax = plt.subplots(figsize=(9, 3.4))
    width = 0.15
    for offset, cc in enumerate(CC_ORDER):
        ys = []
        for workload in workloads:
            rng = 8 if workload == "ycsb-e" else 0
            bucket = pick_bucket(table, workload, cc, rng)
            value = median(table, (workload, cc, bucket, rng, 1), "throughput_committed_txn_s")
            ys.append(value or 0)
        xs = [i + (offset - 2) * width for i in range(len(workloads))]
        label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
        ax.bar(xs, ys, width=width, color=CC_COLOR[cc], label=label)
    ax.set_xticks(range(len(workloads)))
    ax.set_xticklabels([WORKLOAD_LABEL[w] for w in workloads], fontsize=8)
    ax.set_ylabel("committed txns / s")
    ax.set_yscale("log")
    ax.set_ylim(top=ax.get_ylim()[1] * 3.2)
    ax.set_title("Single-thread throughput (no mutex queuing): the defensible overhead comparison", fontsize=9)
    ax.legend(fontsize=7, ncol=3, loc="upper left", frameon=True, framealpha=0.95)
    return save(fig, out_dir, "fig06_single_thread_overhead.png")


def write_summary(table, out_dir, csv_path):
    lines = ["# Formal matrix summary", "",
             f"source: `{os.path.relpath(csv_path, ROOT)}`", "",
             "All values are medians over the three repetitions.", ""]
    for workload in ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]:
        rng = 8 if workload == "ycsb-e" else 0
        lines.append(f"## {workload} (range={rng})")
        lines.append("")
        lines.append("| cc | bucket | t1 | t4 | t32 | abort% @t1 | abort% @t32 |")
        lines.append("|---|---|---|---|---|---|---|")
        for cc in CC_ORDER:
            bucket = pick_bucket(table, workload, cc, rng)
            values = []
            abort = []
            for threads in (1, 32):
                key = (workload, cc, bucket, rng, threads)
                value = median(table, key, "throughput_committed_txn_s")
                total = median(table, key, "transactions")
                aborted = median(table, key, "aborted")
                values.append(value)
                abort.append(100.0 * aborted / total if total else 0.0)
            t1 = median(table, (workload, cc, bucket, rng, 1), "throughput_committed_txn_s")
            t4 = median(table, (workload, cc, bucket, rng, 4), "throughput_committed_txn_s")
            t32 = median(table, (workload, cc, bucket, rng, 32), "throughput_committed_txn_s")
            fmt = lambda v: f"{v:,.0f}" if v else "-"
            lines.append(f"| {cc} | {bucket or '-'} | {fmt(t1)} | {fmt(t4)} | {fmt(t32)} | "
                         f"{abort[0]:.1f}% | {abort[1]:.1f}% |")
        lines.append("")
    path = os.path.join(out_dir, "formal-summary.md")
    with open(path, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print("wrote", path)
    return path


def fig_wasted_work(table, out_dir):
    """Committed vs attempted throughput: how much work is thrown away."""
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.4), sharey=True)
    for ax, workload in zip(axes, ["ycsb-a", "tpcc-new-order"]):
        for cc, style in [("record-2pl", "-"), ("hierarchical", "--")]:
            bucket = pick_bucket(table, workload, cc, 0)
            xs, committed, attempted = [], [], []
            for threads in THREADS:
                key = (workload, cc, bucket, 0, threads)
                c = median(table, key, "throughput_committed_txn_s")
                a = median(table, key, "throughput_attempt_txn_s")
                if c is None:
                    continue
                xs.append(threads)
                committed.append(c)
                attempted.append(a)
            if not xs:
                continue
            label = CC_LABEL[cc].format(bucket) if cc == "hierarchical" else CC_LABEL[cc]
            ax.plot(xs, attempted, style, marker="^", ms=3.5, lw=1.4,
                    color=CC_COLOR[cc], alpha=0.55, label=f"{label} (attempted)")
            ax.plot(xs, committed, style, marker="o", ms=3.5, lw=1.8,
                    color=CC_COLOR[cc], label=f"{label} (committed)")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_xticklabels([str(t) for t in THREADS])
        ax.set_xlabel("threads")
        ax.set_title(WORKLOAD_LABEL[workload], fontsize=9)
        ax.legend(fontsize=6.5)
    axes[0].set_ylabel("txns / s")
    fig.suptitle("Attempted vs committed: up to 97% of transaction work is discarded", fontsize=10)
    return save(fig, out_dir, "fig07_wasted_work.png")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", default="local")
    parser.add_argument("--csv")
    parser.add_argument("--out", default=os.path.join(ROOT, "figures", "formal"))
    args = parser.parse_args()

    csv_path = args.csv
    if not csv_path:
        matches = sorted(glob.glob(os.path.join(
            ROOT, "experiments", "results", f"formal-{args.mode}-*.csv")))
        if not matches:
            raise SystemExit(f"no formal-{args.mode}-*.csv found")
        csv_path = matches[-1]

    rows = load(csv_path)
    print(f"{len(rows)} cases from {csv_path}")
    table = index_rows(rows)
    os.makedirs(args.out, exist_ok=True)
    setup()

    fig_scaling(table, args.out)
    fig_abort(table, args.out)
    fig_range(table, args.out)
    fig_locks(table, args.out)
    fig_bucket(table, args.out)
    fig_single_thread(table, args.out)
    fig_wasted_work(table, args.out)
    write_summary(table, args.out, csv_path)


if __name__ == "__main__":
    main()
