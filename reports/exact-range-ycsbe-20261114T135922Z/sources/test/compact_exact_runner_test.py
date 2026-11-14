import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

runner = sys.argv[1]


def run(backend, policy="no-wait", threads=1, extra=(), ok=True):
    options = {
        "--index": "v2", "--cc": "exact-range", "--exact-lock-table": backend,
        "--workload": "ycsb-e", "--keys": "256", "--txns": "128",
        "--ops": "10", "--range": "32", "--zipf": "0.9", "--seed": "8",
        "--threads": str(threads), "--wait-policy": policy,
        "--execution-mode": "threads", "--max-attempts": "1000", "--format": "csv",
    }
    options.update(zip(extra[::2], extra[1::2]))
    command = [runner, *[value for pair in options.items() for value in pair]]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if not ok:
        assert result.returncode, command
        return
    assert result.returncode == 0, (command, result.stderr)
    parsed = list(csv.reader(result.stdout.splitlines()))
    assert len(parsed) == 2 and len(parsed[0]) == len(parsed[1]), command
    row = dict(zip(*parsed))
    for field in ("index_errors", "lock_errors", "lock_release_failures", "failed_transactions"):
        assert row[field] == "0", (command, field, row[field])
    assert int(row["transactions"]) == int(row["committed"]) + int(row["aborted"])
    assert int(row["transactions"]) == int(row["logical_started"]) + int(row["retries"])
    assert int(row["logical_started"]) == int(row["committed"]) + int(row["logical_unfinished"])
    return row


for policy in ("no-wait", "wait-die", "wound-wait"):
    a, b = run("generic", policy), run("compact", policy)
    assert b["exact_lock_table"] == "compact"
    for field in ("committed", "checksum", "rows_found", "operations", "inserts", "range_reads"):
        assert a[field] == b[field], (policy, field, a[field], b[field])
    for threads in (4, 32, 63):
        run("compact", policy, threads, ("--keys", "65536", "--max-attempts", "100"))
    run("compact", policy, 2, ("--execution-mode", "coroutines", "--coroutines", "2"))

run("compact", threads=64, ok=False)
assert run("auto", threads=64, extra=("--keys", "65536", "--max-attempts", "1"))["exact_lock_table"] == "generic"
assert run("auto")["exact_lock_table"] == "compact"
row = run("compact", threads=2, extra=("--warmup", "1", "--duration", "1"))
assert int(row["warmup_committed"]) > 0 and int(row["elapsed_ns"]) >= 1_000_000_000
row = run("compact", extra=("--lock-budget", "1", "--max-attempts", "1"))
assert int(row["lock_capacity_aborts"]) > 0
row = run("compact", extra=("--exact-latency", "0", "--exact-statistics", "0"))
assert row["rows_found"] == "0" and row["successful_attempt_count"] == "0" and int(row["committed"]) == 128
run("generic", extra=("--exact-latency", "0"), ok=False)
run("compact", extra=("--cc", "next-key"), ok=False)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "latency.json"
    row = run("compact", extra=("--latency-histogram", str(path)))
    assert json.loads(path.read_text())["histograms"]["logical_completion"]["count"] == int(row["committed"])
    config = Path(directory) / "config.json"
    config.write_text(json.dumps({"workload": "ycsb-e", "threads": 1, "key_count": 256,
        "transaction_count": 32, "exact_lock_table": "compact", "exact_latency": 0, "exact_statistics": 0}))
    result = subprocess.run([runner, "--config", str(config), "--cc", "exact-range", "--format", "csv"], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
print("exact-range YCSB-E: generic parity, three policies, threads/coroutines, cleanup, metrics, config and capacity checks passed")
