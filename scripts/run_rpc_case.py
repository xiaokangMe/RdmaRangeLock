#!/usr/bin/env python3
"""Run one RPC workload case: .66 client, .67 data/index server."""
from __future__ import annotations
import argparse, os, shlex, subprocess, time
from pathlib import Path
WORKLOADS=("ycsb-a","ycsb-e","ycsb-e-skew","tpcc-payment","tpcc-new-order")
PROTOCOLS=("none","record-2pl","gap-lock","next-key","prior-key","hierarchical","exact-range","precision-lock","griffin-pl")
WAIT_POLICIES=("no-wait","wait-die","wound-wait")
def add_common(p):
    p.add_argument("--workload",choices=WORKLOADS,required=True)
    p.add_argument("--protocol",choices=PROTOCOLS,required=True)
    p.add_argument("--wait-policy",choices=WAIT_POLICIES,default="no-wait")
    p.add_argument("--keys",type=int,default=2_097_152); p.add_argument("--transactions",type=int,default=5_000)
    p.add_argument("--ops",type=int,default=10); p.add_argument("--range",type=int,default=32)
    p.add_argument("--threads",type=int,default=32); p.add_argument("--seed",type=int,default=8)
    p.add_argument("--zipf",type=float,default=0.99); p.add_argument("--distribution",choices=("auto","uniform","zipfian","hotspot"),default="auto")
    p.add_argument("--hotspot-data",type=float,default=0.2); p.add_argument("--hotspot-ops",type=float,default=0.8)
    p.add_argument("--bucket",type=int,default=64); p.add_argument("--items",type=int,default=5); p.add_argument("--order",type=int,default=128)
    p.add_argument("--max-attempts",type=int,default=1000); p.add_argument("--wait-timeout-ms",type=int,default=5000); p.add_argument("--lock-budget",type=int,default=16_384)
def common_flags(a):
    return ["--workload",a.workload,"--protocol",a.protocol,"--wait-policy",a.wait_policy,"--keys",str(a.keys),
            "--transactions",str(a.transactions),"--ops",str(a.ops),"--range",str(a.range),"--threads",str(a.threads),
            "--seed",str(a.seed),"--zipf",str(a.zipf),"--distribution",a.distribution,"--hotspot-data",str(a.hotspot_data),
            "--hotspot-ops",str(a.hotspot_ops),"--bucket",str(a.bucket),"--items",str(a.items),"--order",str(a.order),
            "--max-attempts",str(a.max_attempts),"--wait-timeout-ms",str(a.wait_timeout_ms),"--lock-budget",str(a.lock_budget)]
def main():
    root=Path(__file__).resolve().parents[1]; p=argparse.ArgumentParser(description=__doc__); add_common(p)
    p.add_argument("--build",type=Path,default=root/"build-shared"); p.add_argument("--remote-host",default="dell@10.102.35.67")
    p.add_argument("--remote-binary",default="/tmp/rrl_rpc_workload"); p.add_argument("--remote-log",default="/tmp/rrl-rpc-server.log")
    p.add_argument("--port",type=int,default=9622); p.add_argument("--warmup-transactions",type=int,default=0); p.add_argument("--dry-run",action="store_true")
    a=p.parse_args()
    if a.threads!=32: raise SystemExit("RPC cases are fixed to 32 client threads")
    binary=a.build/"rrl_rpc_workload"
    if not binary.is_file(): raise SystemExit(f"RPC binary not found: {binary}; build rrl_rpc_workload first")
    key=os.environ.get("RRL_SSH_KEY", str(Path.home()/".ssh/rdmarangelock_deploy"))
    ssh_opts=["-o","BatchMode=yes","-o","IdentitiesOnly=yes","-i",key]
    flags=common_flags(a)
    server=["--role","server"]+flags+["--port",str(a.port)]
    client=["--role","client","--host",a.remote_host.split("@")[-1],"--port",str(a.port),"--warmup-transactions",str(a.warmup_transactions)]+flags
    start="nohup "+shlex.join([a.remote_binary]+server)+" >"+shlex.quote(a.remote_log)+" 2>&1 < /dev/null & echo $!"
    if a.dry_run:
        print("scp",binary,f"{a.remote_host}:{a.remote_binary}"); print("ssh",a.remote_host,start); print("local",binary,shlex.join(client)); return 0
    upload=a.remote_binary+".new"
    subprocess.run(["scp"]+ssh_opts+[str(binary),f"{a.remote_host}:{upload}"],check=True)
    subprocess.run(["ssh"]+ssh_opts+[a.remote_host, "mv "+shlex.quote(upload)+" "+shlex.quote(a.remote_binary)],check=True)
    pid=subprocess.check_output(["ssh"]+ssh_opts+[a.remote_host,start],text=True).strip().splitlines()[-1]
    try:
        for _ in range(180):
            if subprocess.run(["ssh"]+ssh_opts+[a.remote_host,f"grep -q rpc_server_ready {shlex.quote(a.remote_log)}"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0: break
            time.sleep(1)
        else: raise RuntimeError("RPC server did not become ready; inspect "+a.remote_log)
        subprocess.run([str(binary)]+client,check=True)
    finally:
        subprocess.run(["ssh"]+ssh_opts+[a.remote_host,f"kill {shlex.quote(pid)}"],check=False,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        time.sleep(1)
        subprocess.run(["ssh"]+ssh_opts+[a.remote_host,f"kill -9 {shlex.quote(pid)}"],check=False,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return 0
if __name__=="__main__": raise SystemExit(main())
