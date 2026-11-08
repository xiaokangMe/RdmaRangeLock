"""Small correctness traces only, never a formal performance matrix."""
import csv,io,json,subprocess,sys,tempfile
from pathlib import Path
runner=Path(sys.argv[1]).resolve()
root=Path(__file__).resolve().parents[1]
base=json.loads((root/"experiments/configs/precision_lock_correctness.json").read_text())
errors=("lock_errors","index_errors","lock_release_failures","failed_transactions",
        "rdma_timeouts","rdma_completion_errors")
with tempfile.TemporaryDirectory(prefix="rrl-precision-runner-") as tmp:
    cfg=Path(tmp)/"case.json"
    for policy in ("no-wait","wait-die","wound-wait"):
        for workload in ("ycsb-a","ycsb-e","ycsb-e-skew","tpcc-payment","tpcc-new-order"):
            for index,protocol in (("v2","precision-lock"),("v2","griffin-pl"),("griffin","griffin-pl")):
                c=dict(base,index=index,cc=protocol,workload=workload,wait_policy=policy)
                cfg.write_text(json.dumps(c))
                r=subprocess.run([str(runner),"--config",str(cfg),"--format","csv"],check=True,
                                 capture_output=True,text=True,timeout=45)
                rows=list(csv.DictReader(io.StringIO(r.stdout)));assert len(rows)==1
                row=rows[0];assert None not in row and all(v is not None for v in row.values())
                assert all(int(row[k])==0 for k in errors),(c,row)
                assert int(row["committed"])>0 and row["index"]==index and row["cc"]==protocol
                assert row["execution_mode"]=="threads" and row["coroutines"]=="1"
                assert int(row["coroutine_switches_total"])==0
                assert int(row["index_sync_pending_aborts"])>=0
    for flags in (["--index","griffin","--cc","none"],["--index","griffin","--cc","precision-lock"],
                  ["--index","griffin","--cc","rdma-griffin-pl"],["--griffin-queue-capacity","0"]):
        r=subprocess.run([str(runner),*flags],capture_output=True,text=True,timeout=10)
        assert r.returncode==2 and not r.stdout,(flags,r)
print("Precision/Griffin runner: five workloads x three policies x three backend/protocol combinations OK")
