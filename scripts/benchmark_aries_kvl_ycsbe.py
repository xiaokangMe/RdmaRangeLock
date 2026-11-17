#!/usr/bin/env python3
"""Independent ARIES/KVL YCSB-E sweep; edit CONFIG before running."""
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

CONFIG = {
    "skews": [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5],
    "keys": 2**21,
    "threads": 32,
    "trace-txns": 5000,
    "ops": 10,
    "range-length": 32,
    "seed": 8,
    "warmup": 10,
    "duration": 10,
    "order": 128,
    "lock-timeout-ms": 100,
    "max-attempts": 1000,
    "case_timeout_s": 600,
    "binary": "build-aries-release/rrl_aries_kvl_runner",
}


def main():
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = root / "reports" / ("aries-kvl-ycsbe-" + stamp)
    out.mkdir(parents=True, exist_ok=False)
    binary = root / CONFIG["binary"]
    def capture(args):
        return subprocess.check_output(args, text=True).strip()
    env = {
        "config": CONFIG,
        "created": datetime.datetime.now().astimezone().isoformat(),
        "host": platform.node(),
        "platform": platform.platform(),
        "git_branch": capture(["git", "branch", "--show-current"]),
        "git_head": capture(["git", "rev-parse", "HEAD"]),
        "git_status": capture(["git", "status", "--short"]),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "lscpu": capture(["lscpu"]),
        "compiler": capture(["c++", "--version"]),
        "affinity": sorted(os.sched_getaffinity(0)),
    }
    (out / "environment.json").write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n")
    (out / "source.patch").write_text(capture(["git", "diff", "HEAD"]) + "\n")
    # Include new files too: git diff by itself does not capture untracked sources.
    for source in [Path(__file__), root / "benchmarks/aries_kvl_runner.cpp", root / "CMakeLists.txt"]:
        (out / source.name).write_bytes(source.read_bytes())
    fields = ["theta", "tps", "commits", "attempts", "deadlocks", "timeouts", "exhausted", "unfinished", "late_commits", "committed_scans", "committed_inserts", "committed_scan_rows", "drain_s", "lock_requests", "lock_waits", "splits", "final_size", "load_s", "valid"]
    print("RESULT_DIR=" + str(out), flush=True)
    with (out / "summary.csv").open("w", newline="") as summary:
        writer = csv.DictWriter(summary, fieldnames=fields)
        writer.writeheader(); summary.flush()
        for theta in CONFIG["skews"]:
            cmd = [str(binary)]
            for key, value in CONFIG.items():
                if key not in ["skews", "case_timeout_s", "binary"]:
                    cmd += ["--" + key, str(value)]
            cmd += ["--theta", str(theta)]
            tag = "theta-" + str(theta)
            (out / (tag + ".command.json")).write_text(json.dumps(cmd, indent=2) + "\n")
            print("START theta=" + str(theta), flush=True)
            started = time.monotonic()
            with (out / (tag + ".json")).open("w") as stdout, (out / (tag + ".stderr.log")).open("w") as stderr:
                try:
                    proc = subprocess.run(cmd, stdout=stdout, stderr=stderr, timeout=CONFIG["case_timeout_s"])
                except subprocess.TimeoutExpired:
                    print("FAILED timeout theta=" + str(theta), flush=True)
                    return 2
            if proc.returncode:
                print("FAILED returncode=" + str(proc.returncode) + " theta=" + str(theta), flush=True)
                return proc.returncode
            result = json.loads((out / (tag + ".json")).read_text())
            m = result["measurement"]
            row = {"theta": theta, **{key: m[key] for key in fields if key in m}, **{key: result["metrics"][key] for key in ["lock_requests", "lock_waits", "splits"]}, **{key: result[key] for key in ["final_size", "load_s", "valid"]}}
            writer.writerow(row); summary.flush()
            print("DONE theta={} tps={:.3f} aborts={} unfinished={} wall={:.1f}s".format(theta, m["tps"], m["deadlocks"] + m["timeouts"], m["unfinished"], time.monotonic() - started), flush=True)
    print("COMPLETE " + str(out), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
