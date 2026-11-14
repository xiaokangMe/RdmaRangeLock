#!/usr/bin/env python3
"""Single-machine YCSB-E Zipf sweep for the five optimized lock tables.

Edit CONFIG below before a run. Cases execute sequentially with a shared seed
and trace parameters; every command and raw runner output is retained.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import itertools
import json
import os
import platform
import random
import subprocess
import time
from pathlib import Path


# All experiment choices live here so the next run only needs this file edited.
CONFIG = {
    "runner": "build-record/rrl_workload_runner",
    "output_root": "reports",
    "campaign_name": "ycsbe-zipf-compact",
    "algorithms": ["record-2pl", "next-key", "gap-lock", "prior-key", "hierarchical"],
    "wait_policies": ["no-wait", "wait-die", "wound-wait"],
    "zipf_thetas": [0.0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5],
    "keys": 2_097_152,  # Deneva SYNTH_TABLE_SIZE = 2^21.
    "transactions": 5_000,
    "operations_per_transaction": 10,
    "range_length": 32,
    "threads": 32,
    "seed": 8,
    "warmup_seconds": 10,
    "duration_seconds": 10,
    "max_attempts": 1_000,
    "wait_timeout_ms": 100,
    "hierarchy_bucket_width": 64,
    "hierarchy_implicit_root": 1,
    "hierarchy_padded": 0,
    "full_metrics": True,
    "case_timeout_seconds": 180,
    "retry_failed_cases": 1,
    "shuffle_seed": 20261113,
}


SOURCE_FILES = [
    "CMakeLists.txt",
    "benchmarks/workload_runner.cpp",
    "include/lock/record_lock_table.h",
    "include/lock/classic_lock_table.h",
    "include/lock/hierarchy_lock_table.h",
    "include/txn/policy_workload.h",
    "include/txn/record_workload.h",
    "src/txn/record_workload.cpp",
    "src/txn/compact_hierarchy_plan.h",
    "scripts/benchmark_compact_ycsbe_skew.py",
]
ERROR_FIELDS = ("index_errors", "lock_errors", "lock_release_failures", "failed_transactions")


def csv_row(stdout: str) -> dict[str, str]:
    lines = [line for line in stdout.splitlines() if line.strip()]
    rows = list(csv.DictReader(lines))
    if len(rows) != 1:
        raise RuntimeError(f"expected one CSV result row, got {len(rows)}")
    return rows[0]


def command_for(runner: Path, algorithm: str, policy: str, theta: float) -> list[str]:
    command = [str(runner), "--index", "v2", "--cc", algorithm]
    if algorithm == "record-2pl":
        command += ["--record-lock-table", "compact", "--record-latency", "1"]
    elif algorithm in ("next-key", "gap-lock", "prior-key"):
        command += ["--key-lock-table", "compact"]
        if CONFIG["full_metrics"]:
            command += ["--key-latency", "1", "--key-statistics", "1"]
    elif algorithm == "hierarchical":
        command += [
            "--hierarchy-lock-table", "compact",
            "--bucket", str(CONFIG["hierarchy_bucket_width"]),
            "--hierarchy-implicit-root", str(CONFIG["hierarchy_implicit_root"]),
            "--hierarchy-padded", str(CONFIG["hierarchy_padded"]),
        ]
        if CONFIG["full_metrics"]:
            command += ["--hierarchy-latency", "1", "--hierarchy-statistics", "1"]
    command += [
        "--workload", "ycsb-e",
        "--keys", str(CONFIG["keys"]),
        "--txns", str(CONFIG["transactions"]),
        "--ops", str(CONFIG["operations_per_transaction"]),
        "--range", str(CONFIG["range_length"]),
        "--zipf", repr(theta),
        "--seed", str(CONFIG["seed"]),
        "--threads", str(CONFIG["threads"]),
        "--wait-policy", policy,
        "--execution-mode", "threads",
        "--max-attempts", str(CONFIG["max_attempts"]),
        "--wait-timeout-ms", str(CONFIG["wait_timeout_ms"]),
        "--warmup", str(CONFIG["warmup_seconds"]),
        "--duration", str(CONFIG["duration_seconds"]),
        "--format", "csv",
    ]
    return command


def run_case(case: dict, runner: Path, out: Path, ordinal: int, attempt: int) -> dict:
    theta = case["theta"]
    theta_label = f"{theta:g}".replace(".", "p")
    name = (f"{ordinal:03d}-{case['algorithm']}-{case['policy']}-"
            f"z{theta_label}-try{attempt}")
    command = command_for(runner, case["algorithm"], case["policy"], theta)
    record = {**case, "name": name, "attempt": attempt, "command": command}
    started = time.monotonic()
    try:
        result = subprocess.run(
            command, capture_output=True, text=True,
            timeout=CONFIG["case_timeout_seconds"], check=False,
        )
        (out / f"{name}.stdout").write_text(result.stdout, encoding="utf-8")
        (out / f"{name}.stderr").write_text(result.stderr, encoding="utf-8")
        record.update(returncode=result.returncode, wall_seconds=time.monotonic() - started)
        if result.returncode:
            raise RuntimeError(f"runner returned {result.returncode}: {result.stderr[-2000:]}")
        row = csv_row(result.stdout)
        row.update(
            run=name, algorithm=case["algorithm"], zipf_theta=repr(theta),
            wait_policy=case["policy"], experiment_seed=str(CONFIG["seed"]),
            measured_seconds=str(CONFIG["duration_seconds"]),
        )
        for field in ERROR_FIELDS:
            if field in row and int(float(row[field])) != 0:
                raise RuntimeError(f"{field}={row[field]}")
        record["status"] = "PASS"
        record["csv_row"] = row
    except Exception as error:
        record["status"] = "FAIL"
        record["error"] = str(error)
        if isinstance(error, subprocess.TimeoutExpired):
            for stream in ("stdout", "stderr"):
                data = getattr(error, stream) or b""
                if isinstance(data, bytes):
                    data = data.decode(errors="replace")
                (out / f"{name}.{stream}").write_text(data, encoding="utf-8")
    (out / f"{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    runner = (root / CONFIG["runner"]).resolve()
    if not runner.is_file():
        raise SystemExit(f"runner not found: {runner}")
    if CONFIG["threads"] > 63:
        raise SystemExit("compact backend supports at most 63 active contexts")
    if CONFIG["duration_seconds"] < 10:
        raise SystemExit("duration_seconds must be at least 10")
    if any(theta < 0 for theta in CONFIG["zipf_thetas"]):
        raise SystemExit("Zipf theta must be nonnegative")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = root / CONFIG["output_root"] / f"{CONFIG['campaign_name']}-{stamp}"
    out.mkdir(parents=True, exist_ok=False)
    cases = [
        {"algorithm": algorithm, "policy": policy, "theta": theta}
        for theta, policy, algorithm in itertools.product(
            CONFIG["zipf_thetas"], CONFIG["wait_policies"], CONFIG["algorithms"]
        )
    ]
    random.Random(CONFIG["shuffle_seed"]).shuffle(cases)
    metadata = {
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "branch": subprocess.check_output(["git", "branch", "--show-current"], cwd=root, text=True).strip(),
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "platform": platform.platform(),
        "affinity": sorted(os.sched_getaffinity(0)),
        "loadavg_before": os.getloadavg(),
        "config": CONFIG,
        "case_count": len(cases),
        "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
        "source_sha256": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in SOURCE_FILES if (root / name).is_file()
        },
    }
    for name, command in (("cpu", ["lscpu"]), ("git_diff", ["git", "diff"])):
        result = subprocess.run(command, cwd=root, capture_output=True, text=True)
        (out / f"{name}.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    (out / "config_snapshot.py").write_text(Path(__file__).read_text(encoding="utf-8"), encoding="utf-8")

    records = []
    rows = []
    total = len(cases)
    print(f"campaign={out} cases={total} keys={CONFIG['keys']} theta={CONFIG['zipf_thetas']}", flush=True)
    for ordinal, case in enumerate(cases, 1):
        record = run_case(case, runner, out, ordinal, 1)
        records.append(record)
        if record["status"] == "PASS":
            rows.append(record["csv_row"])
        for retry in range(1, CONFIG["retry_failed_cases"] + 1):
            if record["status"] == "PASS":
                break
            print(f"RETRY {ordinal}/{total}: {case} attempt={retry + 1}", flush=True)
            record = run_case(case, runner, out, ordinal, retry + 1)
            records.append(record)
            if record["status"] == "PASS":
                rows.append(record["csv_row"])
        with (out / "attempts.json").open("w", encoding="utf-8") as file:
            json.dump(records, file, indent=2)
        if rows:
            with (out / "results.csv").open("w", newline="", encoding="utf-8") as file:
                fieldnames = list(dict.fromkeys(key for row in rows for key in row))
                writer = csv.DictWriter(file, fieldnames=fieldnames, extrasaction="ignore")
                writer.writeheader()
                writer.writerows(rows)
        print(f"{ordinal}/{total} {case['algorithm']} {case['policy']} theta={case['theta']:g} {record['status']}", flush=True)

    metadata["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    metadata["loadavg_after"] = os.getloadavg()
    metadata["passed_cases"] = len(rows)
    latest = {}
    for record in records:
        latest[(record["algorithm"], record["policy"], record["theta"])] = record
    metadata["failed_cases"] = sum(1 for record in latest.values() if record["status"] == "FAIL")
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"finished passed={len(rows)} failed_attempts={metadata['failed_cases']} output={out}", flush=True)
    return 1 if metadata["failed_cases"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
