#!/usr/bin/env python3
"""Sequential YCSB-E comparison of generic and compressed exact-range locks.

Edit CONFIG before running. All repetitions use identical trace parameters;
randomized case order and alternating variants reduce execution-order bias.
"""
import csv
import datetime as dt
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import time


CONFIG = {
    "runner": "build-record/rrl_workload_runner",
    "output_root": "reports",
    "keys": 2_097_152,
    "transactions": 5_000,
    "operations": 10,
    "range": 32,
    "threads": 32,
    "seed": 8,
    "zipf_values": [0.0, 0.9, 1.5],
    "wait_policies": ["no-wait", "wait-die", "wound-wait"],
    "rounds": 3,
    "warmup": 1,
    "duration": 3,
    "max_attempts": 1_000,
    "wait_timeout_ms": 100,
    "variants": [("generic", "full"), ("compact", "full"), ("compact", "fast")],
}


def main():
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = root / CONFIG["output_root"] / ("exact-range-ycsbe-" + stamp)
    out.mkdir(parents=True, exist_ok=False)
    runner = (root / CONFIG["runner"]).resolve()
    sources = ["CMakeLists.txt", "benchmarks/workload_runner.cpp", "src/txn/record_workload.cpp",
               "src/txn/compact_exact_plan.h", "include/lock/record_lock_table.h",
               "include/txn/policy_workload.h", "include/txn/record_workload.h",
               "test/compact_exact_plan_test.cpp", "test/compact_exact_runner_test.py",
               "scripts/benchmark_compact_exact.py"]
    metadata = {"started_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "config": CONFIG,
                "affinity": sorted(os.sched_getaffinity(0)), "loadavg": os.getloadavg(),
                "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
                "source_sha256": {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sources}}
    for name in sources:
        target = out / "sources" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(Path(name).read_bytes())
    for name, cmd in [("cpu", ["lscpu"]), ("head", ["git", "rev-parse", "HEAD"]),
                      ("branch", ["git", "branch", "--show-current"]), ("diff", ["git", "diff"])]:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        (out / (name + ".txt")).write_text(result.stdout)
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2))
    cases = list(itertools.product(CONFIG["zipf_values"], CONFIG["wait_policies"]))
    total = len(cases) * CONFIG["rounds"] * len(CONFIG["variants"])
    number = 0
    rows, attempts, failures = [], [], []
    print(f"OUTPUT {out}; cases={total}", flush=True)
    for repeat in range(1, CONFIG["rounds"] + 1):
        order = cases[:]
        random.Random(20261114 + repeat).shuffle(order)
        for theta, policy in order:
            variants = CONFIG["variants"][:] if repeat % 2 else list(reversed(CONFIG["variants"]))
            for backend, metrics in variants:
                number += 1
                name = f"{number:03d}-r{repeat}-z{theta:g}-{policy}-{backend}-{metrics}"
                cmd = [str(runner), "--index", "v2", "--cc", "exact-range", "--exact-lock-table", backend,
                       "--workload", "ycsb-e", "--keys", str(CONFIG["keys"]), "--txns", str(CONFIG["transactions"]),
                       "--ops", str(CONFIG["operations"]), "--range", str(CONFIG["range"]),
                       "--zipf", str(theta), "--seed", str(CONFIG["seed"]), "--threads", str(CONFIG["threads"]),
                       "--wait-policy", policy, "--execution-mode", "threads", "--max-attempts", str(CONFIG["max_attempts"]),
                       "--wait-timeout-ms", str(CONFIG["wait_timeout_ms"]), "--warmup", str(CONFIG["warmup"]),
                       "--duration", str(CONFIG["duration"]), "--format", "csv"]
                if metrics == "fast":
                    cmd += ["--exact-latency", "0", "--exact-statistics", "0"]
                record = {"run": name, "command": cmd}
                start = time.monotonic()
                try:
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                    (out / (name + ".stdout")).write_text(result.stdout)
                    (out / (name + ".stderr")).write_text(result.stderr)
                    record.update(returncode=result.returncode, wall_seconds=time.monotonic()-start)
                    if result.returncode:
                        raise RuntimeError(result.stderr)
                    parsed = list(csv.DictReader(result.stdout.splitlines()))
                    if len(parsed) != 1 or None in parsed[0]:
                        raise RuntimeError("malformed CSV")
                    row = parsed[0]
                    for field in ("index_errors", "lock_errors", "lock_release_failures", "failed_transactions"):
                        if int(row[field]):
                            raise RuntimeError(f"{field}={row[field]}")
                    if int(row["logical_started"]) != int(row["committed"]) + int(row["logical_unfinished"]):
                        raise RuntimeError("logical transaction accounting mismatch")
                    row.update(run=name, repeat=repeat, backend=backend, metrics=metrics, theta=theta)
                    rows.append(row)
                except Exception as error:
                    record["error"] = str(error)
                    failures.append(record)
                    if isinstance(error, subprocess.TimeoutExpired):
                        for stream in ("stdout", "stderr"):
                            data = getattr(error, stream) or b""
                            (out / (name + "." + stream)).write_bytes(data.encode() if isinstance(data, str) else data)
                attempts.append(record)
                (out / (name + ".json")).write_text(json.dumps(record, indent=2))
                if rows:
                    with (out / "results.csv").open("w", newline="") as file:
                        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
                        writer.writeheader();writer.writerows(rows)
                (out / "failures.json").write_text(json.dumps(failures, indent=2))
                print(f"{number}/{total} {name} failures={len(failures)}", flush=True)
    summary = []
    for theta, policy in cases:
        for backend, metrics in CONFIG["variants"]:
            sample = [r for r in rows if r["theta"] == theta and r["wait_policy"] == policy and r["backend"] == backend and r["metrics"] == metrics]
            if not sample:
                continue
            tps = [float(r["throughput_committed_txn_s"]) for r in sample]
            commit = sum(int(r["committed"]) for r in sample)
            summary.append({"theta": theta, "wait_policy": policy, "backend": backend, "metrics": metrics,
                "samples": len(sample), "median_tps": statistics.median(tps), "min_tps": min(tps), "max_tps": max(tps),
                "abort_per_commit": sum(int(r["aborted"]) for r in sample) / max(commit, 1),
                "retry_per_commit": sum(int(r["retries"]) for r in sample) / max(commit, 1),
                "wait_ms_per_commit": sum(int(r["lock_wait_total_ns"]) for r in sample) / max(commit, 1) / 1e6,
                "unfinished": sum(int(r["logical_unfinished"]) for r in sample),
                "retry_exhausted": sum(int(r["retry_exhausted"]) for r in sample),
                "lock_bytes": sample[0]["exact_lock_bytes"], "plan_bytes": sample[0]["exact_plan_bytes"]})
    with (out / "summary.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
    metadata.update(finished_utc=dt.datetime.now(dt.timezone.utc).isoformat(), passed=len(rows), failures=len(failures))
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2))
    print(f"COMPLETE {out} valid={len(rows)} failures={len(failures)}", flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
