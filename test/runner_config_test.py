#!/usr/bin/env python3
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

runner = sys.argv[1]
base = [runner, "--keys", "8", "--txns", "1", "--ops", "1", "--range", "2", "--format", "csv"]

def reject(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=10)
    if result.returncode != 2 or result.stdout or not result.stderr.strip():
        raise RuntimeError("invalid configuration accepted or not diagnosed: %r %r" % (args, result))

for workload in ("ycsb-a", "ycsb-b", "tpcc-payment", "tpcc-new-order"):
    result = subprocess.run([runner, "--workload", workload, "--range", "0", "--keys", "8",
                             "--txns", "1", "--ops", "1", "--format", "csv"],
                            capture_output=True, text=True, check=True, timeout=10)
    if list(csv.DictReader(io.StringIO(result.stdout)))[0]["workload"] != workload:
        raise RuntimeError("formal matrix's non-range zero marker changed")
reject([runner, "--workload", "ycsb-e", "--range", "0"])

for cc in ("none", "record-2pl", "gap-lock", "next-key", "hierarchical", "rdma-next-key", "rdma-hierarchical"):
    reject(base + ["--index", "v1", "--cc", cc, "--threads", "2"])
for args in (["--index", "typo"], ["--cc", "typo"], ["--workload", "typo"],
             ["--rdma-port", "0"], ["--rdma-port", "65536"], ["--rdma-port", "-1"],
             ["--threads", "-1"], ["--threads", "0"], ["--threads", "1x"], ["--threads", "1.5"],
             ["--threads", "18446744073709551616"], ["--zipf", "nan"], ["--zipf", "inf"],
             ["--zipf", "0.5x"], ["--zipf", "-1"], ["--format", "typo"], ["--bogus", "1"],
             ["--order", "2"], ["--rdma-host", "typo"], ["--threads"],
             ["--hierarchy-diagnostics", "2"], ["--bucket", "0"], ["--duration", "18446744073709551615"]):
    reject(base + args)
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "config.json"
    for text in ('{"threads":1.5}', '{"threads":-1}', '{"threads":0}', '{"threads":"2"}',
                 '{"threads":2,"index":"v1","cc":"next-key"}', '{"cc":"typo"}',
                 '{"workload":"typo"}', '{"lock_server_port":65536}', '{"threads":01}',
                 '{"threads":1} garbage', '{"threads":1,}', '{"threads":1,"threads":2}',
                 '{"key_count":8,"keys":9}', '{"threadz":2}', '{"threads":[1,2]}',
                 '{"hierarchy_diagnostics":2}', '{"hierarchy_diagnostics":"1"}', '{"zipf_theta":1e999}', '{"zipf_theta":NaN}'):
        path.write_text(text)
        reject([runner, "--config", str(path)])
    config = {"workload": "e", "index": "btree_latch_coupling", "cc": "next-key",
              "key_count": 8, "transaction_count": 1, "operations_per_transaction": 1,
              "threads": 1, "lock_server_port": 19876, "seed": 7}
    path.write_text(json.dumps(config))
    result = subprocess.run([runner, "--config", str(path), "--format", "csv"],
                            capture_output=True, text=True, check=True, timeout=10)
    row = list(csv.DictReader(io.StringIO(result.stdout)))[0]
    if row["index"] != "v2" or row["cc"] != "next-key" or int(row["transactions"]) != 1:
        raise RuntimeError("valid aliases/config lost")
print("runner configuration rejection: OK")

# Diagnostic records have their own channel and conserve worker counters.
args = [runner, "--cc", "hierarchical", "--keys", "128", "--txns", "200",
        "--ops", "4", "--threads", "4", "--bucket", "64", "--format", "csv"]
plain = subprocess.run(args, capture_output=True, text=True, check=True, timeout=10)
if plain.stderr:
    raise RuntimeError("diagnostics must be disabled by default")
with tempfile.TemporaryDirectory() as tmp:
    cfg = Path(tmp) / "diagnostic.json"
    cfg.write_text(json.dumps({"hierarchy_diagnostics": 1}))
    enabled = subprocess.run(args + ["--config", str(cfg)], capture_output=True,
                             text=True, check=True, timeout=10)
    row = list(csv.DictReader(io.StringIO(enabled.stdout)))[0]
    workers = [json.loads(line) for line in enabled.stderr.splitlines()]
    if len(workers) != 4 or any(w["kind"] != "hierarchy-progress" or w["upgrade_requests"] for w in workers):
        raise RuntimeError("missing worker diagnostics or in-flight upgrades")
    for field in ("committed", "aborted"):
        if sum(w[field] for w in workers) != int(row[field]):
            raise RuntimeError("worker diagnostics do not conserve " + field)
    if sum(w["attempts"] for w in workers) != int(row["transactions"]):
        raise RuntimeError("worker attempt counts lost")
    if any(w["root_conflicts"] + w["bucket_conflicts"] for w in workers):
        raise RuntimeError("pure point workload should conflict only on records")
print("hierarchy diagnostic channel and counters: OK")

reject([runner,"--keys","1","--ops","10"])
reject(base+["--ycsb-unique-keys","2"])
subprocess.run([runner,"--keys","1","--ops","10","--txns","2","--ycsb-unique-keys","0"],check=True,capture_output=True,timeout=10)
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/"unique.json"
    path.write_text(json.dumps(dict(keys=1,operations=10,transactions=2,ycsb_unique_keys=0)))
    subprocess.run([runner,"--config",str(path)],check=True,capture_output=True,timeout=10)
    path.write_text(json.dumps(dict(keys=1,operations=10,transactions=2,ycsb_unique_keys=2)))
    reject([runner,"--config",str(path)])
print("Deneva unique-key configuration and explicit repeated-key mode: OK")
