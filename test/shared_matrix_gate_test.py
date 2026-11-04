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
from run_shared_matrices import cases,validate_row,config_fingerprint,validate_resume_rows,resume_keys,config_from_environment
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

# A resumable campaign must match every resolved parameter, including geometry
# absent from a runner CSV (keys/ops/zipf) and the new runtime settings.
runtime=dict(config,coordination_backend="native-rdma-state",coroutines=4,
             coroutine_stack_bytes=65536,warmup_seconds=1)
saved=dict(row,source_commit="baseline",result_kind="smoke-not-formal",
           coordination="local",coroutines="4",warmup_seconds="1",execution_stack_bytes="65536",
           campaign_config_sha256=config_fingerprint(runtime),range="0",bucket="0",seed="8")
validate_resume_rows([saved],runtime,"baseline","smoke-not-formal","local")
for key,value in (("source_commit","old"),("coordination","native-rdma-state"),
                  ("coroutines","1"),("warmup_seconds","0"),("execution_stack_bytes","262144"),
                  ("campaign_config_sha256","old")):
    bad=dict(saved);bad[key]=value
    try:validate_resume_rows([bad],runtime,"baseline","smoke-not-formal","local")
    except ValueError:pass
    else:raise AssertionError(("mixed resume accepted",key))
changed=dict(runtime,operations=runtime["operations"]+1)
try:validate_resume_rows([saved],changed,"baseline","smoke-not-formal","local")
except ValueError:pass
else:raise AssertionError("changed operation geometry accepted")
baseline=dict(saved,cc="none",wait_policy="none")
key=next(iter(resume_keys([baseline],runtime)))
assert key[5]==runtime["wait_policies"][0], "unlocked baseline would be duplicated by resume"
import os
from unittest.mock import patch
with patch.dict(os.environ,{"RRL_COROUTINES":"4","RRL_WARMUP_SECONDS":"7","RRL_COROUTINE_STACK_BYTES":"65536",
                            "RRL_COORDINATION_BACKEND":"native-rdma-state"}):
    e=config_from_environment()
    assert e["coroutines"]==4 and e["warmup_seconds"]==7 and e["coroutine_stack_bytes"]==65536
print("Exact resume configuration, baseline deduplication and shell runtime controls: OK")

# The report must validate the same signature instead of silently grouping
# rows whose dimensions look alike but whose workload parameters differ.
import tempfile
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory)
    small=dict(runtime,duration_seconds=0,runs=1,seeds=[8],threads=[2],workloads=["ycsb-a"],
               local_ccs=["next-key"],ranges=[8],buckets=[16],wait_policies=["wait-die"],
               coroutines=1,coroutine_stack_bytes=262144,warmup_seconds=0)
    report_row=dict(row,source_commit="baseline",result_kind="smoke-not-formal",
                    transport="local-no-rdma",run="1",seed="8",range="0",bucket="0",
                    campaign_config_sha256=config_fingerprint(small))
    data=d/"rows.csv";data.write_text(text(report_row))
    state=d/"campaign.json";state.write_text(json.dumps(dict(status="complete",kind="smoke-not-formal",
        source_commit="baseline",config=small,result_files={"local":str(data)})))
    load_campaign(state,True)
    report_row["campaign_config_sha256"]="other-geometry";data.write_text(text(report_row))
    try:load_campaign(state,True)
    except ValueError:pass
    else:raise AssertionError("report accepted a different workload configuration")
print("Report configuration signature: OK")

with tempfile.TemporaryDirectory() as directory:
    cfg=dict(config,name="resume-integration",local_ccs=["none"],threads=[1],workloads=["ycsb-a"],
             warmup_seconds=0,coroutines=1,coordination_backend="native-rdma-state",
             keys=16,transactions=16,operations=2)
    path=Path(directory)/"config.json";path.write_text(json.dumps(cfg))
    artifacts=Path(directory)/"artifacts";result_dir=artifacts/"results"
    before=set(result_dir.glob("smoke-not-formal-*.json"))
    cmd=[sys.executable,str(root/"scripts/run_shared_matrices.py"),"--config",str(path),
         "--mode","local","--smoke","--keep-geometry","--no-build","--build",str(Path(sys.argv[1]).resolve().parent),
         "--artifact-root",str(artifacts),"--state-file",str(Path(directory)/"live.json")]
    first=subprocess.run(cmd,cwd=root,check=True,capture_output=True,text=True,timeout=30)
    manifests=set(result_dir.glob("smoke-not-formal-*.json"))-before
    assert len(manifests)==1, manifests
    initial=json.loads(next(iter(manifests)).read_text())
    original=Path(initial["result_files"]["local"])
    second=subprocess.run(cmd+["--resume",str(original)],cwd=root,check=True,capture_output=True,text=True,timeout=30)
    resumed=set(result_dir.glob("smoke-not-formal-*.json"))-before-manifests
    assert len(resumed)==1, resumed
    state=json.loads(next(iter(resumed)).read_text())
    output=Path(state["result_files"]["local"])
    assert state["status"]=="complete" and output.is_file()
    assert output.read_bytes()==original.read_bytes()
    assert not list(Path(state["log_dir"]).glob("local-*.command.json")), "completed resume reran a case"
    load_campaign(next(iter(resumed)),True)
print("Completed resume writes its CSV, deduplicates none and remains reportable: OK")

# Real leaf results cannot silently enter an old bucket campaign.
leaf_config=dict(config,hierarchy_layout="leaf")
assert len(list(cases(leaf_config,"local")))<len(list(cases(config,"local")))
leaf_command=list(command);leaf_command[2]="hierarchical";leaf_command += ["--hierarchy-layout","leaf"]
leaf_output=subprocess.run(leaf_command,check=True,capture_output=True,text=True,timeout=20).stdout
assert validate_row(leaf_output,0,False,expected_layout="leaf")["hierarchy_layout"]=="leaf"
try:validate_row(leaf_output,0,False,expected_layout="bucket")
except RuntimeError:pass
else:raise AssertionError("real leaf results accepted as bucket")
print("Real leaf execution and layout isolation: OK")
