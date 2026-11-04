#!/usr/bin/env python3
"""Bounded Deneva-style native runtime verification, never a formal matrix."""
import argparse
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from shared_rdma_services import SharedServices
from run_shared_matrices import validate_row

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build",default="build-shared")
    parser.add_argument("--cross-host",action="store_true")
    parser.add_argument("--seconds",type=int,default=1)
    parser.add_argument("--endurance",type=int,default=0)
    parser.add_argument("--only",default="")
    parser.add_argument("--fault-only",action="store_true")
    args=parser.parse_args()
    if not 1<=args.seconds<=600 or not 0<=args.endurance<=86400:raise SystemExit("duration bounds")
    root=Path(__file__).resolve().parent.parent
    os.chdir(root);build=(root/args.build).resolve()
    logs=root/"experiments/logs"/("native-check-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    logs.mkdir(parents=True)
    # Pin every executable for this verification. Re-linking the developer
    # build must never replace a running check's next service executable.
    pinned=logs/"binaries";pinned.mkdir()
    names=("rrl_workload_runner","rrl_rdma_data_server","rrl_rdma_lock_server",
           "rrl_rdma_txn_server","rrl_native_policy_test","rrl_native_shared_data_test","rrl_native_lock_failure_test")
    hashes={}
    for name in names:
        before=hashlib.sha256((build/name).read_bytes()).hexdigest()
        shutil.copy2(build/name,pinned/name)
        if hashlib.sha256((pinned/name).read_bytes()).hexdigest()!=before or hashlib.sha256((build/name).read_bytes()).hexdigest()!=before:
            raise RuntimeError("binary changed while pinning; rebuild/check sequentially")
        hashes[name]=before
    build=pinned
    (logs/"binary-sha256.json").write_text(json.dumps(hashes,indent=2))
    (logs/"source-commit.txt").write_text(subprocess.check_output(["git","rev-parse","HEAD"],text=True))
    env={k:v for k,v in os.environ.items() if not k.startswith("RRL_")}
    env["RRL_RDMA_GID_INDEX"]="3"
    ssh=["ssh","-o","BatchMode=yes","-o","IdentitiesOnly=yes","-i",
         str(Path.home()/".ssh/rdmarangelock_deploy"),"dell@10.102.35.67"]
    remote=None
    if args.cross_host:
        remote=subprocess.check_output(ssh+["mktemp -d /tmp/rrl-native-check.XXXXXX"],text=True).strip()
        if not remote.startswith("/tmp/rrl-native-check."):raise RuntimeError("unsafe remote directory")
        names=("rrl_workload_runner","rrl_native_policy_test","rrl_native_shared_data_test","rrl_native_lock_failure_test")
        subprocess.run(["scp"]+ssh[1:-1]+[str(build/n) for n in names]+[ssh[-1]+":"+remote+"/"],check=True)
        for name in names:
            digest=subprocess.check_output(ssh+["sha256sum "+shlex.quote(remote+"/"+name)],text=True).split()[0]
            if digest!=hashlib.sha256((build/name).read_bytes()).hexdigest():raise RuntimeError("remote binary mismatch")
    def actual(name,flags,e,on_remote):
        binary=(remote if on_remote else str(build))+"/"+name
        cmd=[binary]+flags
        if on_remote:cmd=ssh+[shlex.join(["env"]+[k+"="+v for k,v in e.items() if k.startswith("RRL_")]+cmd)]
        return cmd
    def run(cmd,label,e=env,timeout=180):
        (logs/(label+".command.json")).write_text(json.dumps(cmd))
        p=subprocess.run(cmd,env=e,capture_output=True,text=True,timeout=timeout)
        (logs/(label+".stdout")).write_text(p.stdout);(logs/(label+".stderr")).write_text(p.stderr)
        if p.returncode:raise RuntimeError((label,p.returncode,p.stdout[-1000:],p.stderr[-2000:]))
        return p.stdout
    base=json.loads((root/"experiments/configs/rdma_shared_smoke.json").read_text())
    cases=[]
    if not args.endurance and not args.only:
        for mode in ("clean","running","committing","gate"):
            label="native-lock-fault-"+mode
            path=logs/(label+".json");path.write_text(json.dumps(base))
            with SharedServices(build,path,logs/label,env,native=True) as service:
                e=service.test_env("10.0.0.3" if args.cross_host else "127.0.0.1")
                run(actual("rrl_native_lock_failure_test",[mode],e,args.cross_host),label,e)
                if mode=="clean":service.check()
                else:
                    if service.processes[1].wait(timeout=10)!=2:raise RuntimeError("native physical server failed open")
                    if any(p.poll() is not None for i,p in enumerate(service.processes) if i!=1):raise RuntimeError("unrelated data/state failure")
                print("PASS",label,flush=True)
        for warmup in (0,30):
            label="native-data-fault-"+("measurement" if warmup==0 else "warmup")
            cfg=dict(base,workload="ycsb-e",cc="rdma-next-key",threads=2,coroutines=2,
                     warmup_seconds=warmup,duration_seconds=30,wait_policy="wound-wait")
            path=logs/(label+".json");path.write_text(json.dumps(cfg))
            with SharedServices(build,path,logs/label,env,native=True) as service:
                host="10.0.0.3" if args.cross_host else "127.0.0.1"
                flags=["--index","rdma","--cc","rdma-next-key","--workload","ycsb-e",
                       "--keys",str(cfg["key_count"]),"--txns",str(cfg["transaction_count"]),
                       "--ops",str(cfg["operations_per_transaction"]),"--range",str(cfg["range_length"]),
                       "--seed",str(cfg["seed"]),"--order",str(cfg["order"]),"--threads","2",
                       "--coroutines","2","--warmup",str(warmup),"--duration","30",
                       "--wait-policy","wound-wait","--rdma-host",host,
                       "--data-port",str(service.data_port),"--rdma-port",str(service.lock_port),
                       "--txn-state-port",str(service.txn_state_port),"--format","csv"]
                cmd=actual("rrl_workload_runner",flags,env,args.cross_host)
                (logs/(label+".command.json")).write_text(json.dumps(cmd))
                peer=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                try:
                    e=service.test_env()
                    run(actual("rrl_native_shared_data_test",["--poison-active"],e,False),label+"-inject",e)
                    out,err=peer.communicate(timeout=90)
                    (logs/(label+".stdout")).write_text(out);(logs/(label+".stderr")).write_text(err)
                    if peer.returncode!=3:raise RuntimeError(("fault did not invalidate native measurement",peer.returncode,out,err))
                    row=list(csv.DictReader(io.StringIO(out)))[0]
                    if not any(int(row[k]) for k in ("index_errors","lock_errors","lock_release_failures")):
                        raise RuntimeError("data fault became ordinary conflict")
                    run(actual("rrl_native_shared_data_test",["--check-state-poison"],e,False),label+"-state",e)
                    # An uncertain active data fault may deliberately leave
                    # physical locks fenced. The native lock service then
                    # stops, rather than reclaiming and reopening them.
                    for number, process in enumerate(service.processes):
                        code=process.poll()
                        if code is None:continue
                        if number==1 and code==2 and "native owner disconnected" in (logs/label/"rrl_rdma_lock_server.log").read_text():continue
                        raise RuntimeError(("unexpected fault service exit",number,code))
                    print("PASS",label,flush=True)
                finally:
                    if peer.poll() is None:peer.terminate();peer.wait(timeout=30)
        if args.fault_only:
            (logs/"validated.json").write_text(json.dumps(dict(status="PASS",formal=False,fault_cases=6)))
            print("PASS; native warmup/measurement data faults; logs:",logs,flush=True);return
    for policy in ("no-wait","wait-die","wound-wait"):
        if not args.endurance and not args.only:
            cfg=logs/(policy+"-contract.json");cfg.write_text(json.dumps(base))
            with SharedServices(build,cfg,logs/(policy+"-contracts"),env,native=True) as service:
                e=service.test_env("10.0.0.3" if args.cross_host else "127.0.0.1")
                run(actual("rrl_native_policy_test",[policy,"--poison"],e,args.cross_host),policy+"-contract",e)
                service.check()
            with SharedServices(build,cfg,logs/(policy+"-shared"),env,native=True) as service:
                if args.cross_host:
                    subprocess.run(["scp"]+ssh[1:-1]+[str(cfg),ssh[-1]+":"+remote+"/dataset.json"],check=True)
                commands=[]
                for on_remote in (False,args.cross_host):
                    e=service.test_env("10.0.0.3" if on_remote else "127.0.0.1")
                    e["RRL_TEST_CONFIG"]=remote+"/dataset.json" if on_remote else str(cfg)
                    commands.append((actual("rrl_native_shared_data_test",["200",policy],e,on_remote),e))
                peers=[subprocess.Popen(c,env=e,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for c,e in commands]
                try:
                    for n,p in enumerate(peers):
                        out,err=p.communicate(timeout=90)
                        (logs/(policy+"-increment-"+str(n)+".stdout")).write_text(out)
                        (logs/(policy+"-increment-"+str(n)+".stderr")).write_text(err)
                        if p.returncode:raise RuntimeError(("shared increments",policy,n,p.returncode,err))
                finally:
                    for p in peers:
                        if p.poll() is None:p.kill();p.wait()
                e=service.test_env();e["RRL_TEST_CONFIG"]=str(cfg)
                run(actual("rrl_native_shared_data_test",["--read","400"],e,False),policy+"-exact-sum",e)
                service.check()
        for cc in ("rdma-record-2pl","rdma-gap-lock","rdma-next-key","rdma-hierarchical","rdma-predicate"):
            for workload in ("ycsb-a","ycsb-e","tpcc-payment","tpcc-new-order"):
                label="-".join((policy,cc,workload))
                if args.only and args.only!=label:continue
                if args.endurance and (policy!="wound-wait" or cc!="rdma-next-key" or workload!="ycsb-e"):continue
                cfg=dict(base,workload=workload,cc=cc,wait_policy=policy,threads=2,coroutines=2,
                         warmup_seconds=1,duration_seconds=args.endurance or args.seconds)
                path=logs/(label+".json");path.write_text(json.dumps(cfg))
                with SharedServices(build,path,logs/label,env,native=True) as service:
                    flags=["--index","rdma","--cc",cc,"--workload",workload,
                           "--keys",str(cfg["key_count"]),"--txns",str(cfg["transaction_count"]),
                           "--ops",str(cfg["operations_per_transaction"]),"--range",str(cfg["range_length"]),
                           "--seed",str(cfg["seed"]),"--order",str(cfg["order"]),
                           "--threads","2","--coroutines","2","--warmup","1",
                           "--duration",str(cfg["duration_seconds"]),"--wait-policy",policy,
                           "--data-port",str(service.data_port),"--rdma-port",str(service.lock_port),
                           "--txn-state-port",str(service.txn_state_port),"--rdma-host",
                           "10.0.0.3" if args.cross_host else "127.0.0.1","--format","csv"]
                    row=validate_row(run(actual("rrl_workload_runner",flags,env,args.cross_host),label,
                                         timeout=cfg["duration_seconds"]+180),cfg["duration_seconds"],True,"native-rdma-state")
                    if int(row["warmup_committed"])<=0 or int(row["warmup_elapsed_ns"])<1000000000:
                        raise RuntimeError("warmup did not execute real transactions")
                    if int(row["active_contexts"])!=4 or int(row["state_rdma_operations"])<=0 or int(row["committed"])<=0:
                        raise RuntimeError("native/coroutine execution missing")
                    service.check();cases.append(dict(name=label,**row))
                    (logs/"progress.json").write_text(json.dumps(cases,indent=2))
                    print("PASS",label,flush=True)
    (logs/"validated.json").write_text(json.dumps(dict(status="PASS",formal=False,cases=cases),indent=2))
    print("PASS;",len(cases),"native shared data cases; logs:",logs,flush=True)
if __name__=="__main__":main()
