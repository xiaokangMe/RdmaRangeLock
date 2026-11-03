#!/usr/bin/env python3
"""Six independent hour-long correctness runs; concurrent data are not rankings."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build",default="build-shared")
    p.add_argument("--seconds",type=int,default=3600)
    a=p.parse_args()
    if not 1<=a.seconds<=86400: p.error("seconds must be 1..86400")
    root=Path(__file__).resolve().parent.parent;os.chdir(root)
    logs=root/"experiments/logs"/("shared-endurance-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"));logs.mkdir(parents=True)
    binary=(root/a.build/"rrl_workload_runner").resolve()
    paths=subprocess.check_output(["git","ls-files","--cached","--others","--exclude-standard","-z"],text=True).split("\0")
    manifest={f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in paths if f and (root/f).is_file() and f.startswith(("include/","src/","benchmarks/","test/","scripts/","experiments/configs/"))}
    (logs/"source-sha256.json").write_text(json.dumps(manifest,indent=2))
    state=dict(kind="concurrent-correctness-not-performance",status="RUNNING",seconds=a.seconds,pid=os.getpid(),
               binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),cases=[])
    def save():
        text=json.dumps(state,indent=2);temp=logs/"state.tmp";temp.write_text(text);temp.replace(logs/"state.json")
    def run(cross,cc,policy):
        name=("cross-" if cross else "loopback-")+cc+"-"+policy
        cmd=["python3","-u","scripts/check_shared_rdma.py","--build",a.build,"--endurance",str(a.seconds),
             "--skip-contract","--only",cc+"-ycsb-e-"+policy]
        if cross:cmd.append("--cross-host")
        (logs/(name+".command.json")).write_text(json.dumps(cmd))
        with (logs/(name+".stdout")).open("w") as out,(logs/(name+".stderr")).open("w") as err:
            result=subprocess.run(cmd,stdout=out,stderr=err,timeout=a.seconds+180)
        return dict(name=name,returncode=result.returncode)
    tasks=[(True,cc,policy) for cc in ("rdma-next-key","rdma-hierarchical") for policy in ("wait-die","wound-wait")]
    tasks += [(False,"rdma-hierarchical",policy) for policy in ("wait-die","wound-wait")]
    save();print(str(logs),flush=True)
    with ThreadPoolExecutor(max_workers=6) as pool:
        futures=[pool.submit(run,*task) for task in tasks]
        for future in as_completed(futures):
            try:state["cases"].append(future.result())
            except BaseException as e:state["cases"].append(dict(error=repr(e),returncode=-1))
            save();print(state["cases"][-1],flush=True)
    state["status"]="PASS" if all(c["returncode"]==0 for c in state["cases"]) else "FAIL"
    save();print(state["status"],flush=True)
    if state["status"]!="PASS":raise SystemExit(1)

if __name__=="__main__":main()
