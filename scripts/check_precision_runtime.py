#!/usr/bin/env python3
"""Bounded paper-contract and shared-data correctness; never a formal matrix."""
import argparse,csv,datetime,hashlib,io,json,os,shlex,shutil,subprocess
from pathlib import Path
from shared_rdma_services import SharedServices

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build",default="build-shared")
    parser.add_argument("--cross-host",action="store_true")
    a=parser.parse_args()
    root=Path(__file__).resolve().parents[1];build=(root/a.build).resolve()
    logs=root/"experiments/logs"/("precision-check-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"));logs.mkdir(parents=True)
    pinned=logs/"binaries";pinned.mkdir()
    names=("rrl_workload_runner","rrl_rdma_data_server","rrl_rdma_lock_server","rrl_rdma_txn_server","rrl_precision_shared_test")
    hashes={}
    for name in names:
        digest=hashlib.sha256((build/name).read_bytes()).hexdigest();shutil.copy2(build/name,pinned/name)
        if hashlib.sha256((pinned/name).read_bytes()).hexdigest()!=digest:raise RuntimeError("binary changed during pinning")
        hashes[name]=digest
    build=pinned
    state=dict(status="RUNNING",formal=False,pid=os.getpid(),cases=[],
        source_commit=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip(),
        source_diff_sha256=hashlib.sha256(subprocess.check_output(["git","diff","HEAD"],cwd=root)).hexdigest(),
        binary_sha256=hashes)
    def save():
        tmp=logs/"state.tmp";tmp.write_text(json.dumps(state,indent=2));tmp.replace(logs/"state.json")
    save();print(logs,flush=True)
    env={k:v for k,v in os.environ.items() if not k.startswith("RRL_")};env["RRL_RDMA_GID_INDEX"]="3"
    ssh=["ssh","-o","BatchMode=yes","-o","IdentitiesOnly=yes","-i",str(Path.home()/".ssh/rdmarangelock_deploy"),"dell@10.102.35.67"]
    remote=None
    if a.cross_host:
        remote=subprocess.check_output(ssh+["mktemp -d /tmp/rrl-precision-check.XXXXXX"],text=True).strip()
        if not remote.startswith("/tmp/rrl-precision-check."):raise RuntimeError("unsafe remote binary path")
        for name in ("rrl_workload_runner","rrl_precision_shared_test"):
            subprocess.run(["scp"]+ssh[1:-1]+[str(build/name),ssh[-1]+":"+remote+"/"],check=True)
            if subprocess.check_output(ssh+["sha256sum "+shlex.quote(remote+"/"+name)],text=True).split()[0]!=hashes[name]:
                raise RuntimeError("remote binary mismatch")
    def launch(binary,args,e,cross=False):
        cmd=[(remote if cross else str(build))+"/"+binary,*args]
        if cross:
            cmd=ssh+[shlex.join(["env",*[k+"="+str(v) for k,v in e.items() if k.startswith("RRL_")],*cmd])]
        return subprocess.Popen(cmd,env=e,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    def collect(peer,label):
        try:out,err=peer.communicate(timeout=120)
        except BaseException:peer.kill();peer.wait();raise
        (logs/(label+".stdout")).write_text(out);(logs/(label+".stderr")).write_text(err)
        if peer.returncode:raise RuntimeError((label,peer.returncode,out[-2000:],err[-2000:]))
        return out
    def passed(label):
        state["cases"].append(dict(name=label,status="PASS"));save();print("PASS",label,flush=True)
    base=json.loads((root/"experiments/configs/precision_lock_correctness.json").read_text())
    try:
        for protocol in ("precision-lock","griffin-pl"):
            for policy in ("no-wait","wait-die","wound-wait"):
                label=protocol+"-"+policy;cfg=dict(base,cc="rdma-"+protocol,index="rdma",wait_policy=policy)
                path=logs/(label+".json");path.write_text(json.dumps(cfg,indent=2))
                with SharedServices(build,path,logs/label,env,native=True) as svc:
                    local=dict(svc.test_env(),RRL_TEST_CONFIG=str(path))
                    collect(launch("rrl_precision_shared_test",["contract"],local),label+"-contracts");passed(label+"-contracts")
                    if a.cross_host:
                        subprocess.run(["scp"]+ssh[1:-1]+[str(path),ssh[-1]+":"+remote+"/config.json"],check=True)
                        remote_env=dict(svc.test_env("10.0.0.3"),RRL_TEST_CONFIG=remote+"/config.json")
                    peers=[]
                    try:
                        for i,cross in enumerate((False,a.cross_host)):
                            peers.append((launch("rrl_precision_shared_test",[protocol,"increment",policy,"15"],
                                                 remote_env if cross else local,cross),label+"-writer-"+str(i)))
                        for peer,name in peers:collect(peer,name)
                    finally:
                        for peer,_ in peers:
                            if peer.poll() is None:peer.kill();peer.wait()
                    collect(launch("rrl_precision_shared_test",[protocol,"read","30"],local),label+"-sum")
                    passed(label+"-two-writer-sum")
                    # Keep the trace identical to the service's E seed image.
                    flags=["--config",remote+"/config.json" if a.cross_host else str(path),
                           "--format","csv","--rdma-host","10.0.0.3" if a.cross_host else "127.0.0.1",
                           "--rdma-port",str(svc.lock_port),"--data-port",str(svc.data_port),
                           "--txn-state-port",str(svc.txn_state_port)]
                    out=collect(launch("rrl_workload_runner",flags,env,a.cross_host),label+"-ycsb-e")
                    row=next(csv.DictReader(io.StringIO(out)))
                    for field in ("lock_errors","index_errors","lock_release_failures","failed_transactions","rdma_timeouts","rdma_completion_errors"):
                        if int(row[field]):raise RuntimeError((label,field,row[field]))
                    if (int(row["committed"])<=0 or int(row["data_rdma_operations"])<=0 or
                        row["execution_mode"]!="threads" or int(row["threads"])!=32):
                        raise RuntimeError((label,"no 32-thread shared-data progress",row))
                    passed(label+"-ycsb-e")
                    svc.check()
        state["status"]="PASS";save();print("PASS;",len(state["cases"]),"checks;",logs,flush=True)
    except BaseException as error:
        state.update(status="FAIL",error=repr(error));save();raise
if __name__=="__main__":main()
