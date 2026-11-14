#!/usr/bin/env python3
"""Create tables and a throughput plot from a compact YCSB-E skew campaign."""
from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


ALGORITHMS = ["record-2pl", "next-key", "gap-lock", "prior-key", "hierarchical"]
POLICIES = ["no-wait", "wait-die", "wound-wait"]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_compact_ycsbe_skew.py CAMPAIGN_DIR")
    campaign = Path(sys.argv[1]).resolve()
    with (campaign / "results.csv").open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if len(rows) != 135:
        raise SystemExit(f"expected 135 successful cases, found {len(rows)}")

    grouped = defaultdict(list)
    for row in rows:
        grouped[(row["algorithm"], row["wait_policy"], float(row["zipf_theta"]))].append(row)
    if any(len(values) != 1 for values in grouped.values()) or len(grouped) != 135:
        raise SystemExit("duplicate or missing algorithm/policy/theta combinations")

    thetas = sorted({float(row["zipf_theta"]) for row in rows})
    lines = [
        "# YCSB-E Compact Lock Zipf Sweep",
        "",
        "单机 `.66`，单次 campaign：32 个线程，`2^21 = 2,097,152` keys，5,000 条 trace 事务、每事务 10 个操作、range 32、seed 8；预热 10 秒、测量 10 秒。YCSB-E trace 使用 ordered Zipf；theta=0 对应均匀采样。全部 135 个 case 成功，未发生 runner 错误。本报告是单次探索性测量，不包含独立重复轮次或置信区间。",
        "",
        "层级锁固定 bucket width=64、implicit root=1、padding=0。五种实现均使用 compact 锁表及 full metrics。吞吐是提交事务/秒；重试、中止、等待和未完成事务是各 theta 九个 case 的累积计数，`abort/commit`、`retry/commit` 以累计提交数归一化，wait ms/commit 以累计提交数归一化。",
        "",
        "## Committed TPS",
    ]
    for policy in POLICIES:
        lines += ["", f"### {policy}", "", "| Zipf theta | " + " | ".join(ALGORITHMS) + " |", "|---:|" + "---:|" * len(ALGORITHMS)]
        for theta in thetas:
            cells = [f"{float(grouped[(algorithm, policy, theta)][0]['throughput_committed_txn_s']):,.0f}" for algorithm in ALGORITHMS]
            lines.append(f"| {theta:g} | " + " | ".join(cells) + " |")

    lines += [
        "",
        "## Conflict And Completion",
        "",
        "| Algorithm | Policy | Mean TPS across theta | Aborts / commit | Retries / commit | Wait ms / commit | Retry exhausted | Deadline aborts | Unfinished |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    summary_rows = []
    for algorithm in ALGORITHMS:
        for policy in POLICIES:
            sample = [grouped[(algorithm, policy, theta)][0] for theta in thetas]
            committed = sum(int(row["committed"]) for row in sample)
            aborted = sum(int(row["aborted"]) for row in sample)
            retries = sum(int(row["retries"]) for row in sample)
            wait_ns = sum(int(row["lock_wait_total_ns"]) for row in sample)
            exhausted = sum(int(row["retry_exhausted"]) for row in sample)
            deadlines = sum(int(row["deadline_aborts"]) for row in sample)
            unfinished = sum(int(row["logical_unfinished"]) for row in sample)
            mean_tps = statistics.mean(float(row["throughput_committed_txn_s"]) for row in sample)
            values = {
                "algorithm": algorithm,
                "policy": policy,
                "mean_tps": mean_tps,
                "aborts_per_commit": aborted / committed if committed else float("inf"),
                "retries_per_commit": retries / committed if committed else float("inf"),
                "wait_ms_per_commit": wait_ns / committed / 1_000_000 if committed else float("inf"),
                "retry_exhausted": exhausted,
                "deadline_aborts": deadlines,
                "unfinished": unfinished,
            }
            summary_rows.append(values)
            lines.append(
                f"| {algorithm} | {policy} | {mean_tps:,.0f} | {values['aborts_per_commit']:.2f} | "
                f"{values['retries_per_commit']:.2f} | {values['wait_ms_per_commit']:.4f} | "
                f"{exhausted:,} | {deadlines:,} | {unfinished:,} |"
            )

    lines += [
        "",
        "## Interpretation Notes",
        "",
        "- record-2PL is a record-lock baseline and does not provide phantom protection for range scans; it is not isolation-equivalent to next-key, gap, prior-key, or hierarchical bucket locking.",
        "- The hierarchical no-wait results have high retry exhaustion and unfinished transaction counts in the more contended cases. Read its throughput beside the completion columns; high attempt throughput does not imply all logical transactions completed.",
        "- theta=0.9 and above changes throughput sharply for several strategies. Since each point is a single run, confirm those changes with at least three independently repeated, order-balanced rounds before drawing a performance conclusion.",
        "- This is the project runner's YCSB-E variant: 95% range-read / 5% insert request mix, fixed key-width scans, finite keys, and a repeated trace. Inserts after warmup may overwrite existing keys; this is not a growing-keyspace insert benchmark.",
        "- Raw per-case commands, stdout/stderr, JSON, CSV, environment, branch, source hashes, and config snapshot are retained beside this report.",
        "",
    ]
    (campaign / "summary.md").write_text("\n".join(lines), encoding="utf-8")
    with (campaign / "summary.csv").open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    fig, axes = plt.subplots(1, 3, figsize=(18, 5), sharey=True)
    colors = dict(zip(ALGORITHMS, ("#2676b8", "#d4543c", "#32936f", "#9b6ab0", "#a67b24")))
    for ax, policy in zip(axes, POLICIES):
        for algorithm in ALGORITHMS:
            values = [float(grouped[(algorithm, policy, theta)][0]["throughput_committed_txn_s"]) for theta in thetas]
            ax.plot(thetas, values, marker="o", linewidth=1.8, markersize=4, label=algorithm, color=colors[algorithm])
        ax.set_title(policy)
        ax.set_xticks(thetas, [f"{theta:g}" for theta in thetas])
        ax.set_xlabel("Zipf theta")
        ax.grid(True, alpha=0.25)
        ax.set_yscale("log")
    axes[0].set_ylabel("Committed transactions / second (log scale)")
    axes[-1].legend(loc="best", fontsize=8)
    fig.suptitle("YCSB-E compact lock tables: skew sensitivity")
    fig.tight_layout()
    fig.savefig(campaign / "throughput.png", dpi=180)
    plt.close(fig)
    print(f"wrote {campaign / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
