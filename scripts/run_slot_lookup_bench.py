#!/usr/bin/env python3
"""Run the full snapshot-lookup microbenchmark; default configuration has 3 repeats."""
import argparse
import csv
import io
import json
import os
import platform
import subprocess
from pathlib import Path


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-correctness-fix")
    parser.add_argument("--config", default="experiments/configs/slot-lookup-bench.json")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if git(root, "status", "--porcelain"):
        raise SystemExit("commit source/config/scripts before measuring")
    commit = git(root, "rev-parse", "HEAD")
    config = json.loads((root / args.config).read_text())
    if config["repetitions"] < 3:
        raise SystemExit("at least 3 repeats required")
    build = (root / args.build).resolve()
    subprocess.run(["cmake", "--build", str(build), "--target", "rrl_slot_lookup_bench", "-j4"], check=True)
    output = (root / args.output).resolve()
    if output.parent != (root / "experiments/results").resolve():
        raise SystemExit("output must be directly in experiments/results")
    fields = ["source_commit", "host", "repeat", "slots", "shape", "policy",
              "iterations", "total_ns", "ns_per_lookup", "checksum", "warmup"]
    with output.open("x", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for repeat in range(config["repetitions"]):
            for size in config["slots"]:
                for shape in config["shapes"]:
                    reference = None
                    # Alternate order to avoid always measuring one policy first.
                    for policy in config["policies"][::1 if repeat % 2 == 0 else -1]:
                        result = subprocess.run([
                            str(build / "rrl_slot_lookup_bench"), str(size), str(config["iterations"]),
                            shape, policy, str(config["seed"])], check=True, capture_output=True, text=True)
                        rows = list(csv.DictReader(io.StringIO(result.stdout)))
                        if len(rows) != 1 or None in rows[0] or None in rows[0].values():
                            raise RuntimeError("invalid benchmark CSV")
                        row = rows[0]
                        identity = (row["checksum"], row["warmup"])
                        if reference is not None and reference != identity:
                            raise RuntimeError("policy checksums differ")
                        reference = identity
                        writer.writerow(dict(source_commit=commit, host=platform.node(), repeat=repeat, **row))
                        handle.flush()
    if git(root, "rev-parse", "HEAD") != commit or git(root, "status", "--porcelain"):
        raise SystemExit("source changed during measurement; discard output")
    metadata = dict(source_commit=commit, config=config, host=platform.node(),
                    environment={k: v for k, v in os.environ.items() if k.startswith("RRL_")})
    metadata["runtime"] = platform.platform()
    with output.with_suffix(".json").open("x") as handle:
        json.dump(metadata, handle, indent=2)
    print(output)


if __name__ == "__main__":
    main()
