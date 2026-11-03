#!/usr/bin/env python3
"""Targeted next-key profile on an already isolated server; never starts a full matrix."""
import argparse
import csv
import io
import json
import os
import platform
import subprocess
from pathlib import Path
from run_slot_lookup_bench import git


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--build", default="build-correctness-fix")
    parser.add_argument("--config", default="experiments/configs/rdma-lookup-profile.json")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if git(root, "status", "--porcelain"):
        raise SystemExit("commit source/config/scripts before measuring")
    commit = git(root, "rev-parse", "HEAD")
    config = json.loads((root / args.config).read_text())
    # Conservative resident-slot bound: one range workload txn may take
    # ops*(keys+2) next-key intervals; every interval could touch a shard.
    if config["cc"] != "rdma-next-key" or config["workload"] != "ycsb-e":
        raise SystemExit("this targeted preflight supports only next-key YCSB-E")
    worst_slots = config["threads"] * config["operations_per_transaction"] * (config["key_count"] + 2)
    if worst_slots >= 8192:
        raise SystemExit(f"conservative shard capacity bound {worst_slots} >= 8192; reduce config")
    build = (root / args.build).resolve()
    subprocess.run(["cmake", "--build", str(build), "--target", "rrl_workload_runner", "-j4"], check=True)
    output = (root / args.output).resolve()
    if output.parent != (root / "experiments/results").resolve():
        raise SystemExit("output must be directly in experiments/results")
    # Pin non-policy knobs; GID/device remain caller selected and are recorded.
    profile_env = os.environ.copy()
    profile_env.update(RRL_RDMA_PROFILE="1", RRL_GATE_RETRY="64",
                       RRL_LOCAL_GATE="1", RRL_RDMA_BUCKET_WIDTH=str(config["hierarchy_bucket_width"]))
    writer = None
    with output.open("x", newline="") as handle:
        for repeat in range(3):
            for policy in (["index", "linear"] if repeat % 2 == 0 else ["linear", "index"]):
                env = profile_env.copy()
                env["RRL_RDMA_PROFILE"] = "1"
                env["RRL_SLOT_LOOKUP"] = policy
                result = subprocess.run([
                    str(build / "rrl_workload_runner"), "--rdma-lock-only", "1", "--config", str(root / args.config),
                    "--rdma-host", args.host, "--rdma-port", str(args.port), "--format", "csv"],
                    env=env, check=True, capture_output=True, text=True, timeout=120)
                rows = list(csv.DictReader(io.StringIO(result.stdout)))
                if len(rows) != 1 or None in rows[0] or None in rows[0].values():
                    raise RuntimeError("invalid workload CSV")
                row = rows[0]
                errors = ("lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
                          "rdma_timeouts", "rdma_completion_errors", "rdma_count_errors", "rdma_local_errors",
                          "lock_capacity_aborts")
                if any(int(row[key]) for key in errors):
                    raise RuntimeError("invalid profile: " + result.stdout)
                if int(row["committed"]) + int(row["aborted"]) != int(row["transactions"]):
                    raise RuntimeError("invalid transaction accounting")
                data = dict(source_commit=commit, client_host=platform.node(), server=args.host,
                            port=args.port, repeat=repeat, lookup_policy=policy, **row)
                if writer is None:
                    writer = csv.DictWriter(handle, fieldnames=list(data))
                    writer.writeheader()
                writer.writerow(data)
                handle.flush()
    if git(root, "rev-parse", "HEAD") != commit or git(root, "status", "--porcelain"):
        raise SystemExit("source changed during measurement; discard output")
    metadata = dict(source_commit=commit, config=config, host=platform.node(),
                    environment={k: v for k, v in os.environ.items() if k.startswith("RRL_")})
    metadata["runtime"] = platform.platform()
    metadata["fixed_environment"] = {k: v for k, v in profile_env.items() if k.startswith("RRL_")}
    metadata["server"] = dict(host=args.host, port=args.port, requires_isolated_empty_server=True)
    with output.with_suffix(".json").open("x") as handle:
        json.dump(metadata, handle, indent=2)
    print(output)


if __name__ == "__main__":
    main()
