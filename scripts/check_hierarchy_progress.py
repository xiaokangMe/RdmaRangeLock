#!/usr/bin/env python3
"""Validate a directed hierarchy diagnostic, never launch a benchmark."""
import argparse
import json
from pathlib import Path
from csv_schema import load_rows


def check(csv_path, progress_path):
    with Path(csv_path).open(newline="") as handle:
        rows = load_rows(handle)
    if len(rows) != 1:
        raise ValueError("expected exactly one directed case")
    row = rows[0]
    workers = [json.loads(line) for line in Path(progress_path).read_text().splitlines()]
    count = int(row["threads"])
    if sorted(w["worker"] for w in workers) != list(range(count)):
        raise ValueError("missing or duplicate worker diagnostics")
    for w in workers:
        if w["kind"] != "hierarchy-progress" or w["upgrade_requests"] != 0:
            raise ValueError("wrong diagnostic kind or in-flight conversion")
        if w["attempts"] != w["committed"] + w["aborted"] + w["failed_transactions"]:
            raise ValueError("worker transaction conservation failed")
        if w["committed"] == 0:
            raise ValueError("directed case made no commit progress on worker %s" % w["worker"])
        if row["workload"] == "ycsb-a" and (w["root_conflicts"] or w["bucket_conflicts"]):
            raise ValueError("point-only case reported a coarse-grained conflict")
    for local, total in [("attempts", "transactions"), ("committed", "committed"),
                         ("aborted", "aborted"), ("failed_transactions", "failed_transactions")]:
        if sum(w[local] for w in workers) != int(row[total]):
            raise ValueError("worker aggregate disagrees with CSV: " + total)
    if sum(w[phase] for w in workers for phase in ("root_conflicts", "bucket_conflicts", "record_conflicts")) != int(row["lock_conflicts"]):
        raise ValueError("conflict-layer counters disagree with CSV")
    for field in ("lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
                  "lock_capacity_aborts", "lock_gate_aborts", "rdma_timeouts", "rdma_completion_errors",
                  "rdma_count_errors", "rdma_local_errors"):
        if int(row[field]) != 0:
            raise ValueError("directed case has errors: " + field)
    return {"kind": "directed-validation-only", "workers": count,
            "worker_commits": [w["committed"] for w in workers],
            "max_no_commit_ns": max(w["max_no_commit_ns"] for w in workers),
            "max_release_ns": max(w["max_release_ns"] for w in workers),
            "max_acquire_ns": max(w["max_acquire_ns"] for w in workers)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv")
    parser.add_argument("progress_jsonl")
    args = parser.parse_args()
    print(json.dumps(check(args.csv, args.progress_jsonl), ensure_ascii=False))
