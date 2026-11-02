#!/usr/bin/env python3
"""Analysis of the final formal matrices (local and cross-host).

Reads the newest experiments/results/formal-local-*.csv and
formal-cross-host-*.csv, validates their schema, checks the measurement window,
and writes a stack of markdown tables to figures/final/tables.md while printing
the same text to stdout.

Every table is a median over the three repetitions per configuration, and every
column named here is required: a missing or renamed column fails at load time
rather than producing a plausible but wrong number.
"""

import argparse
import glob
import hashlib
import os
import statistics as st
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_schema

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
OUT_DIR = os.path.join(ROOT, "figures", "final")
THREADS = [1, 2, 4, 8, 16, 32]

LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
# The value --bucket auto picks, so every table below describes the same
# configuration the published matrix uses.
AUTO_BUCKET = {"ycsb-e": 256}


def auto_bucket(workload):
    return AUTO_BUCKET.get(workload, 16)
RDMA_CCS = ["rdma-next-key", "rdma-hierarchical"]
WORKLOADS = ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]

REQUIRED = [
    "workload", "cc", "threads", "transactions", "committed", "aborted",
    "elapsed_ns", "lock_acquires", "lock_conflicts", "lock_capacity_aborts",
    "lock_gate_aborts", "rows_found", "rows_rechecked", "rdma_timeouts",
    "rdma_completion_errors", "rdma_operations", "throughput_committed_txn_s",
    "throughput_attempt_txn_s", "rdma_connections", "hierarchy_bucket_width",
]


def load_latest(pattern):
    """Newest result file that passes schema validation.

    Old matrices written before the header fix have a 35-column header over
    45-column rows; they are kept as historical evidence, so they are skipped
    with a warning instead of aborting the whole analysis.
    """
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)), reverse=True)
    for path in files:
        try:
            with open(path, newline="") as handle:
                rows = csv_schema.load_rows(handle)
            csv_schema.require(rows, REQUIRED)
        except SystemExit as error:
            print("skipping %s: %s" % (os.path.basename(path), error), file=sys.stderr)
            continue
        if files[0] != path:
            print("note: using %s (newer files were rejected)" % os.path.basename(path),
                  file=sys.stderr)
        return path, rows
    return None, None


def num(row, field):
    return float(row[field])


def group(rows, keys):
    table = defaultdict(list)
    for row in rows:
        table[tuple(row[k] for k in keys)].append(row)
    return table


def med(table, key, field):
    values = table.get(key)
    if not values:
        return None
    return st.median(num(v, field) for v in values)


def fmt(value, digits=0):
    if value is None:
        return "-"
    if digits == 0:
        return "%d" % round(value)
    return ("%%.%df" % digits) % value


def window_report(rows, label, out):
    windows = [num(r, "elapsed_ns") / 1e9 for r in rows]
    short = [w for w in windows if w < 9.5]
    out.append("### %s：窗口与完整性\n" % label)
    out.append("| 项 | 数值 |")
    out.append("|---|---:|")
    out.append("| 运行数 | %d |" % len(rows))
    out.append("| 窗口中位数 | %.2f s |" % st.median(windows))
    out.append("| 窗口最小 / 最大 | %.2f / %.2f s |" % (min(windows), max(windows)))
    out.append("| 短于 9.5 秒的运行 | %d |" % len(short))
    out.append("| 超过 12 秒的运行 | %d |" % len([w for w in windows if w > 12]))
    out.append("| elapsed 反推与记录值一致 | %s |" %
               ("是" if all(abs(num(r, "transactions") / num(r, "throughput_attempt_txn_s")
                                - num(r, "elapsed_ns") / 1e9) < 0.05 for r in rows) else "否"))
    out.append("")


def error_report(rows, label, out):
    out.append("### %s：失败与传输错误\n" % label)
    out.append("| 指标 | 合计 |")
    out.append("|---|---:|")
    for field in ("lock_capacity_aborts", "lock_gate_aborts", "rdma_timeouts",
                  "rdma_completion_errors"):
        out.append("| `%s` | %d |" % (field, sum(num(r, field) for r in rows)))
    out.append("")


def local_tables(rows, out):
    table = group(rows, ["workload", "cc", "bucket", "range", "threads"])
    out.append("## 二、本地矩阵：吞吐（committed txn/s，三次中位数）\n")
    for workload in WORKLOADS:
        out.append("### %s\n" % workload)
        header = "| cc | bucket | " + " | ".join("t%d" % t for t in THREADS) + " | t32/t1 |"
        out.append(header)
        out.append("|---|---|" + "---:|" * len(THREADS) + "---:|")
        ranges = ("8", "32", "128") if workload == "ycsb-e" else ("0",)
        for cc in LOCAL_CCS:
            buckets = [16, 64, 256] if cc == "hierarchical" else [0]
            for bucket in buckets:
                for r in ranges:
                    values = [med(table, (workload, cc, str(bucket), r, str(t)),
                                  "throughput_committed_txn_s") for t in THREADS]
                    if values[0] is None:
                        continue
                    ratio = values[-1] / values[0] if values[0] else None
                    label = cc if cc != "hierarchical" else "hierarchical b%d" % bucket
                    if workload == "ycsb-e":
                        label += " r=%s" % r
                    out.append("| %s | %d | %s | %.2f× |" % (
                        label, bucket, " | ".join(fmt(v) for v in values), ratio))
        out.append("")

    out.append("## 三、本地矩阵：t=32 的中止率与锁数量\n")
    out.append("| workload | cc | abort% | conflicts | gate | capacity | lock_acquires/txn |")
    out.append("|---|---|---:|---:|---:|---:|---:|")
    for workload in WORKLOADS:
        rng = "8" if workload == "ycsb-e" else "0"
        for cc in LOCAL_CCS:
            bucket = auto_bucket(workload) if cc == "hierarchical" else 0
            key = (workload, cc, str(bucket), rng, "32")
            txns = med(table, key, "transactions")
            if not txns:
                continue
            aborted = med(table, key, "aborted")
            acquires = med(table, key, "lock_acquires")
            out.append("| %s | %s | %.1f%% | %d | %d | %d | %.0f |" % (
                workload, cc, 100 * aborted / txns,
                med(table, key, "lock_conflicts"), med(table, key, "lock_gate_aborts"),
                med(table, key, "lock_capacity_aborts"), acquires / txns))
    out.append("")

    out.append("## 四、锁成本随扫描宽度的变化（YCSB-E，t=1）\n")
    out.append("| cc | range | lock_acquires/txn | rows_found/txn | 锁/返回行 |")
    out.append("|---|---:|---:|---:|---:|")
    for cc in LOCAL_CCS:
        bucket = auto_bucket("ycsb-e") if cc == "hierarchical" else 0
        for rng in ("8", "32", "128"):
            key = ("ycsb-e", cc, str(bucket), rng, "1")
            txns = med(table, key, "transactions")
            if not txns:
                continue
            acquires = med(table, key, "lock_acquires") / txns
            rows_found = med(table, key, "rows_found") / txns
            out.append("| %s | %s | %.1f | %.1f | %.3f |" % (
                cc, rng, acquires, rows_found,
                acquires / rows_found if rows_found else 0))
    out.append("")

    out.append("## 五、v6 桶宽扫描（hierarchical，committed txn/s）\n")
    targets = [("ycsb-e", "8"), ("ycsb-a", "0"), ("tpcc-payment", "0"),
               ("tpcc-new-order", "0")]
    for workload, rng in targets:
        out.append("### %s%s\n" % (workload, "（range=8）" if rng == "8" else ""))
        out.append("| bucket | " + " | ".join("t%d" % t for t in THREADS) + " |")
        out.append("|---|" + "---:|" * len(THREADS))
        for bucket in (16, 64, 256):
            values = [med(table, (workload, "hierarchical", str(bucket), rng, str(t)),
                          "throughput_committed_txn_s") for t in THREADS]
            if values[0] is None:
                continue
            out.append("| %d | %s |" % (bucket, " | ".join(fmt(v) for v in values)))
        out.append("")


def cross_tables(rows, out):
    table = group(rows, ["cc", "bucket", "range", "threads"])
    out.append("## 六、跨主机矩阵：吞吐与中止（committed txn/s，三次中位数）\n")
    out.append("| cc | bucket | range | " + " | ".join("t%d" % t for t in THREADS) + " |")
    out.append("|---|---|---:|" + "---:|" * len(THREADS))
    for cc in RDMA_CCS:
        buckets = [16, 64, 256] if cc == "rdma-hierarchical" else [0]
        for bucket in buckets:
            for rng in ("8", "32", "128"):
                values = [med(table, (cc, str(bucket), rng, str(t)),
                              "throughput_committed_txn_s") for t in THREADS]
                if values[0] is None:
                    continue
                label = cc if cc != "rdma-hierarchical" else "rdma-hier"
                out.append("| %s | %d | %s | %s |" % (
                    label, bucket, rng, " | ".join(fmt(v) for v in values)))
    out.append("")

    out.append("## 七、跨主机矩阵：t=32 中止构成\n")
    out.append("| cc | abort% | conflicts | capacity | gate | ops/txn | connections |")
    out.append("|---|---:|---:|---:|---:|---:|---:|")
    for cc in RDMA_CCS:
        bucket = auto_bucket("ycsb-e") if cc == "rdma-hierarchical" else 0
        key = (cc, str(bucket), "8", "32")
        txns = med(table, key, "transactions")
        if not txns:
            continue
        out.append("| %s | %.1f%% | %d | %d | %d | %.1f | %d |" % (
            cc, 100 * med(table, key, "aborted") / txns,
            med(table, key, "lock_conflicts"), med(table, key, "lock_capacity_aborts"),
            med(table, key, "lock_gate_aborts"),
            med(table, key, "rdma_operations") / txns,
            med(table, key, "rdma_connections")))
    out.append("")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(OUT_DIR, "tables.md"))
    args = parser.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    local_path, local = load_latest("formal-local-*.csv")
    cross_path, cross = load_latest("formal-cross-host-*.csv")

    out = []
    out.append("# 正式矩阵汇总（自动生成）\n")
    out.append("生成脚本：`scripts/analyze_final.py`\n")
    for label, path in (("本地", local_path), ("跨主机", cross_path)):
        if not path:
            out.append("- %s：无数据\n" % label)
            continue
        with open(path, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        out.append("- %s：`%s`（%d 行，sha256 `%s`）\n" %
                   (label, os.path.basename(path), sum(1 for _ in open(path)) - 1, digest))
    out.append("")

    if local:
        out.append("## 一、本地数据完整性\n")
        window_report(local, "本地", out)
        error_report(local, "本地", out)
        local_tables(local, out)
    if cross:
        out.append("## 五、跨主机数据完整性\n")
        window_report(cross, "跨主机", out)
        error_report(cross, "跨主机", out)
        cross_tables(cross, out)

    text = "\n".join(out) + "\n"
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(text)
    print(text)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
