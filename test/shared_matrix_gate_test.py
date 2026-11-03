#!/usr/bin/env python3
"""Reject measurements from a different backend, malformed counts or failed runs."""
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(root/"scripts"))
from run_shared_matrices import cases,validate_row
from report_shared_matrices import load_campaign
config=json.loads((root/"experiments/configs/formal_shared_10s.json").read_text())
assert len(list(cases(config,"local")))==756
assert len(list(cases(config,"cross-host")))==432
command=[sys.argv[1],"--cc","next-key","--keys","16","--txns","40","--ops","4",
         "--threads","2","--wait-policy","wait-die","--format","csv"]
result=subprocess.run(command,check=True,capture_output=True,text=True,timeout=15)
row=validate_row(result.stdout,0,False)
assert row["data_backend"]=="local" and int(row["committed"])>0

def text(r):
    out=io.StringIO();w=csv.DictWriter(out,fieldnames=list(r));w.writeheader();w.writerow(r);return out.getvalue()

def reject(r,shared=False):
    try:validate_row(text(r),0,shared)
    except (RuntimeError,AssertionError,ValueError):return
    raise AssertionError("invalid result accepted")

bad=dict(row);bad["index_errors"]="1";reject(bad)
for counter in ("lock_capacity_aborts","lock_gate_aborts"):
    bad=dict(row);bad[counter]="1";reject(bad)
bad=dict(row);bad["transactions"]=str(int(row["transactions"])+1);reject(bad)
bad=dict(row);bad["logical_completion_count"]="0";reject(bad)
# A local index with remote locks cannot masquerade as a shared-data result.
bad=dict(row);bad["cc"]="rdma-next-key";reject(bad,True)
# Empty completion sample is a tagged no-progress observation, not a fake p99.
zero=dict(row)
for key in zero:
    if key in ("committed","logical_completion_count","successful_attempt_count") or key.startswith(("successful_attempt_","logical_completion_")):
        zero[key]="0"
zero.update(transactions="1",aborted="1",logical_started="1",logical_unfinished="1",logical_failed="0",retries="0",completed_retries="0",unfinished_retries="0")
assert validate_row(text(zero),0,False)["progress_status"]=="no-commits"
# Default runner must fail before network setup for the retired lock-only path.
r=subprocess.run([sys.argv[1],"--cc","rdma-next-key","--index","v2"],capture_output=True,text=True,timeout=10)
assert r.returncode!=0 and "--rdma-lock-only" in r.stderr
print("Matrix dimensions, backend identity, hard errors, count conservation and no-progress labeling: OK")
