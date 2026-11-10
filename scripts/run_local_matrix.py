#!/usr/bin/env python3
"""Run the local YCSB-A/E/E-skew matrix only.

The runner is invoked once per case; no RDMA service or coordinator is started.
Profiles combine Zipf theta values with E-skew hotspot data/operation fractions.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import itertools
import json
from pathlib import Path
import subprocess

LOCAL_PROTOCOLS = (
    "none",
    "record-2pl",
    "gap-lock",
    "next-key",
    "prior-key",
    "hierarchical",
    "exact-range",
    "precision-lock",
    "griffin-pl",
    "citron",
)
WAIT_POLICIES = ("no-wait", "wait-die", "wound-wait")
WORKLOADS = ("ycsb-a", "ycsb-e", "ycsb-e-skew")


def make_matrix_config() -> dict:
    """唯一的默认矩阵配置；直接编辑这里即可改变默认实验。

    Zipf theta 用于 YCSB-A/E；request_distribution=auto 时，YCSB-E-skew
    使用下面的热点数据/请求比例。列表会做笛卡尔积。
    """
    return {
        "duration": 10,
        "warmup": 10,
        "keys": 2000000,
        "txns": 5000,
        "ops": 10,
        "threads": 32,
        "seed": 8,
        "order": 128,
        "tpcc_items": 5,
        "max_attempts": 1000,
        "wait_timeout_ms": 5000,
        "lock_budget": 16384,
        "zipf_values": [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5],
        "hotspot_data_fractions": [0.2],
        "hotspot_opn_fractions": [0.8],
        "workloads": list(WORKLOADS),
        "protocols": list(LOCAL_PROTOCOLS),
        "wait_policies": list(WAIT_POLICIES),
        "ranges": [8, 32, 128],
        "buckets": [16, 64, 256],
        "default_bucket": 64,
        "request_distribution": "auto",
        "ycsb_unique_keys": 1,
    }


def finite_fraction(value: float, name: str) -> float:
    if not 0.0 <= value <= 1.0:
        raise argparse.ArgumentTypeError(f"{name} must be in [0,1]")
    return value


def hotspot_data_fraction(value: str) -> float:
    result = float(value)
    # The current generator needs both a nonempty hot and cold key set.
    if not 0.0 < result < 1.0:
        raise argparse.ArgumentTypeError("hotspot data fraction must be in (0,1)")
    return result


def theta_value(value: str) -> float:
    result = float(value)
    if result < 0.0:
        raise argparse.ArgumentTypeError("Zipf theta must be nonnegative")
    return result


def csv_row(output: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError("runner did not return a CSV header and row")
    reader = csv.DictReader(lines)
    rows = list(reader)
    if len(rows) != 1 or not reader.fieldnames or any(value is None for value in rows[0].values()):
        raise RuntimeError("runner returned malformed CSV")
    return rows[0]


def cases(args: argparse.Namespace):
    policies = tuple(args.wait_policies)
    for theta, data_fraction, opn_fraction in itertools.product(
        args.zipf_values, args.hotspot_data_fractions, args.hotspot_opn_fractions
    ):
        profile = f"z{theta:g}-hot{data_fraction:g}-{opn_fraction:g}"
        for workload in args.workloads:
            ranges = args.ranges if workload in ("ycsb-e", "ycsb-e-skew") else (0,)
            for range_length in ranges:
                for protocol in args.protocols:
                    widths = args.buckets if protocol == "hierarchical" else (args.default_bucket,)
                    protocol_policies = ("no-wait",) if protocol in ("none", "citron") else policies
                    for bucket, wait_policy in itertools.product(widths, protocol_policies):
                        yield {
                            "profile": profile,
                            "zipf": theta,
                            "hotspot_data_fraction": data_fraction,
                            "hotspot_opn_fraction": opn_fraction,
                            "workload": workload,
                            "range": range_length,
                            "cc": protocol,
                            "bucket": bucket,
                            "wait_policy": wait_policy,
                        }


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    defaults = make_matrix_config()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=root / "build-shared")
    parser.add_argument("--output", type=Path, default=None, help="result CSV; default experiments/results/local-ycsb-<timestamp>.csv")
    parser.add_argument("--log-dir", type=Path, default=None, help="per-case stdout/stderr directory")
    parser.add_argument("--duration", type=int, default=defaults["duration"])
    parser.add_argument("--warmup", type=int, default=defaults["warmup"])
    parser.add_argument("--keys", type=int, default=defaults["keys"])
    parser.add_argument("--txns", type=int, default=defaults["txns"])
    parser.add_argument("--ops", type=int, default=defaults["ops"])
    parser.add_argument("--threads", type=int, default=defaults["threads"])
    parser.add_argument("--seed", type=int, default=defaults["seed"])
    parser.add_argument("--order", type=int, default=defaults["order"])
    parser.add_argument("--tpcc-items", type=int, default=defaults["tpcc_items"])
    parser.add_argument("--max-attempts", type=int, default=defaults["max_attempts"])
    parser.add_argument("--wait-timeout-ms", type=int, default=defaults["wait_timeout_ms"])
    parser.add_argument("--lock-budget", type=int, default=defaults["lock_budget"])
    parser.add_argument("--zipf-values", nargs="+", type=theta_value, default=defaults["zipf_values"], metavar="THETA")
    parser.add_argument("--hotspot-data-fractions", nargs="+", type=hotspot_data_fraction, default=defaults["hotspot_data_fractions"])
    parser.add_argument("--hotspot-opn-fractions", nargs="+", type=lambda v: finite_fraction(float(v), "hotspot operation fraction"), default=defaults["hotspot_opn_fractions"])
    parser.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=defaults["workloads"])
    parser.add_argument("--protocols", nargs="+", choices=LOCAL_PROTOCOLS, default=defaults["protocols"])
    parser.add_argument("--wait-policies", nargs="+", choices=WAIT_POLICIES, default=defaults["wait_policies"])
    parser.add_argument("--ranges", nargs="+", type=int, default=defaults["ranges"])
    parser.add_argument("--buckets", nargs="+", type=int, default=defaults["buckets"])
    parser.add_argument("--default-bucket", type=int, default=defaults["default_bucket"])
    parser.add_argument("--request-distribution", choices=("auto", "uniform", "zipfian", "hotspot"), default=defaults["request_distribution"])
    parser.add_argument("--ycsb-unique-keys", type=int, choices=(0, 1), default=defaults["ycsb_unique_keys"])
    parser.add_argument("--dry-run", action="store_true", help="print case count and commands without starting the runner")
    args = parser.parse_args()
    if args.threads != 32:
        raise SystemExit("this matrix is fixed to 32 OS threads")
    if args.duration < 10 or args.warmup < 0:
        raise SystemExit("duration must be >=10 seconds and warmup nonnegative")
    if any(value < 1 for value in (*args.ranges, *args.buckets, args.default_bucket)):
        raise SystemExit("ranges and bucket widths must be positive")
    if any(value < 1 for value in (args.keys, args.txns, args.ops, args.order, args.max_attempts, args.wait_timeout_ms, args.lock_budget)):
        raise SystemExit("sizes and budgets must be positive")
    if args.protocols == [] or args.workloads == []:
        raise SystemExit("protocols and workloads must not be empty")

    binary = args.build / "rrl_workload_runner"
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    output = args.output or root / "experiments" / "results" / f"local-ycsb-{stamp}.csv"
    log_dir = args.log_dir or root / "experiments" / "logs" / f"local-ycsb-{stamp}"
    plan = list(cases(args))
    print(f"cases={len(plan)} threads=32 execution_mode=threads coroutines=1")
    print(f"zipf_values={args.zipf_values} hotspot_data={args.hotspot_data_fractions} hotspot_ops={args.hotspot_opn_fractions}")
    if args.dry_run:
        return 0
    if not binary.is_file():
        raise SystemExit(f"runner not found: {binary}; build it before running this matrix")
    output.parent.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    resolved = {
        "keys": args.keys,
        "transactions": args.txns,
        "operations": args.ops,
        "threads": args.threads,
        "execution_mode": "threads",
        "coroutines": 1,
        "seed": args.seed,
        "duration_seconds": args.duration,
        "warmup_seconds": args.warmup,
        "zipf_values": args.zipf_values,
        "hotspot_data_fractions": args.hotspot_data_fractions,
        "hotspot_opn_fractions": args.hotspot_opn_fractions,
        "workloads": args.workloads,
        "protocols": args.protocols,
        "wait_policies": args.wait_policies,
        "ranges": args.ranges,
        "buckets": args.buckets,
        "request_distribution": args.request_distribution,
        "case_count": len(plan),
    }
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    campaign_path = log_dir / "campaign.json"
    campaign = {
        "source_commit": commit,
        "config": resolved,
        "config_sha256": hashlib.sha256(json.dumps(resolved, sort_keys=True).encode()).hexdigest(),
        "status": "RUNNING",
    }
    campaign_path.write_text(json.dumps(campaign, indent=2) + "\n")
    rows: list[dict[str, str]] = []
    for number, case in enumerate(plan, 1):
        command = [
            str(binary), "--index", "v2", "--cc", case["cc"], "--workload", case["workload"],
            "--keys", str(args.keys), "--txns", str(args.txns), "--ops", str(args.ops),
            "--ycsb-unique-keys", str(args.ycsb_unique_keys), "--range", str(case["range"]),
            "--threads", "32", "--seed", str(args.seed), "--zipf", str(case["zipf"]),
            "--request-distribution", args.request_distribution,
            "--hotspot-data-fraction", str(case["hotspot_data_fraction"]),
            "--hotspot-opn-fraction", str(case["hotspot_opn_fraction"]),
            "--bucket", str(case["bucket"]), "--items", str(args.tpcc_items), "--order", str(args.order),
            "--duration", str(args.duration), "--warmup", str(args.warmup), "--format", "csv",
            "--execution-mode", "threads", "--coroutines", "1",
        ]
        if case["cc"] != "none":
            command += ["--wait-policy", case["wait_policy"], "--max-attempts", str(args.max_attempts),
                        "--wait-timeout-ms", str(args.wait_timeout_ms), "--lock-budget", str(args.lock_budget)]
        label = f"{number:04d}-{case['workload']}-{case['cc']}-{case['profile']}-b{case['bucket']}-{case['wait_policy']}"
        try:
            completed = subprocess.run(command, cwd=root, text=True, capture_output=True, check=True)
            row = csv_row(completed.stdout)
        except subprocess.CalledProcessError as error:
            (log_dir / f"{label}.stdout").write_text(error.stdout or "")
            (log_dir / f"{label}.stderr").write_text(error.stderr or "")
            raise RuntimeError(f"case {label} failed; see {log_dir}") from error
        except Exception:
            (log_dir / f"{label}.stdout").write_text(getattr(completed, "stdout", "") if "completed" in locals() else "")
            raise
        (log_dir / f"{label}.stdout").write_text(completed.stdout)
        (log_dir / f"{label}.stderr").write_text(completed.stderr)
        row.update({"matrix_profile": case["profile"], "matrix_zipf": str(case["zipf"]),
                    "matrix_hotspot_data_fraction": str(case["hotspot_data_fraction"]),
                    "matrix_hotspot_opn_fraction": str(case["hotspot_opn_fraction"]),
                    "matrix_bucket": str(case["bucket"]), "matrix_wait_policy": case["wait_policy"],
                    "matrix_case": str(number)})
        rows.append(row)
        fields = list(dict.fromkeys(key for item in rows for key in item))
        with output.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        print(f"{number}/{len(plan)} {label}", flush=True)
    campaign["status"] = "COMPLETE"
    campaign["completed_cases"] = len(rows)
    campaign_path.write_text(json.dumps(campaign, indent=2) + "\n")
    print(f"completed={len(rows)} output={output} logs={log_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
