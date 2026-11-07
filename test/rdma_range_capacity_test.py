"""Directed loopback correctness checks; no performance matrix."""
import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/"scripts"))
from shared_rdma_services import SharedServices
from run_shared_matrices import lock_service_flags,validate_row
exe=Path(sys.argv[1]).resolve();build=exe.parent
subprocess.run([str(exe)],check=True,timeout=10)
if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
env=dict(os.environ)
base=dict(workload="ycsb-e",key_count=128,transaction_count=24,operations_per_transaction=2,
          range_length=8,threads=2,coroutines=1,seed=8,zipf=0,order=8,tree_pages=256,
          tree_records=512,warmup_seconds=0,lock_budget=64)
with tempfile.TemporaryDirectory(prefix="rrl-range-capacity-") as directory:
    d=Path(directory)
    for capacity in (8,32768):
        cfg=dict(base,cc="rdma-gap-lock",rdma_lock_slots_per_shard=capacity,native_object_locks=0)
        path=d/"range.json";path.write_text(json.dumps(cfg))
        with SharedServices(build,path,d/str(capacity),env,native=True) as service:
            subprocess.run([str(exe),"--capacity"],env=service.test_env(),check=True,timeout=45)
            service.check()
    for protocol in ("rdma-record-2pl","rdma-gap-lock","rdma-next-key","rdma-prior-key"):
      for policy in ("no-wait","wait-die","wound-wait"):
        cfg=dict(base,cc=protocol,rdma_lock_slots_per_shard=32768,
                 native_object_locks=int(protocol=="rdma-record-2pl"))
        path=d/"runtime.json";path.write_text(json.dumps(cfg))
        with SharedServices(build,path,d/(protocol+"-"+policy),env,native=True) as service:
            if cfg["native_object_locks"]:
                subprocess.run([str(exe),"--reject-range"],env=service.test_env(),check=True,timeout=30)
            cmd=[str(build/"rrl_workload_runner"),"--index","rdma","--cc",protocol,
                 "--workload","ycsb-e","--keys","128","--txns","24","--ops","2","--range","8",
                 "--threads","2","--coroutines","2","--order","8","--seed","8","--zipf","0","--format","csv",
                 "--wait-policy",policy,"--rdma-host","127.0.0.1","--rdma-port",str(service.lock_port),
                 "--data-port",str(service.data_port),"--txn-state-port",str(service.txn_state_port)]
            out=subprocess.run(cmd+lock_service_flags(cfg),env=env,check=True,capture_output=True,text=True,timeout=45)
            row=validate_row(out.stdout,0,True,"native-rdma-state")
            assert int(row["native_object_locks"])==cfg["native_object_locks"] and int(row["committed"])>0
            assert not int(row["lock_capacity_aborts"]) and not int(row["lock_release_failures"])
            assert int(row["rows_rechecked"])>0
            service.check()
print("Range geometry and native record/gap paths under all three wait policies: OK")

# Runtime conversions share the native state + remote data path.
with tempfile.TemporaryDirectory(prefix="rrl-bucket-conversion-") as directory:
    d=Path(directory)
    for workload in ("ycsb-a","ycsb-e"):
      for policy in ("no-wait","wait-die","wound-wait"):
        cfg=dict(base,workload=workload,cc="rdma-hierarchical",operations_per_transaction=8,
                 hierarchy_bucket_width=128,hierarchy_escalation_threshold=2,hierarchy_deescalate_after=4,
                 rdma_lock_slots_per_shard=8192,native_object_locks=0)
        path=d/"runtime.json";path.write_text(json.dumps(cfg))
        with SharedServices(build,path,d/(workload+"-"+policy),env,native=True) as service:
            cmd=[str(build/"rrl_workload_runner"),"--config",str(path),"--index","rdma","--wait-policy",policy,
                 "--rdma-host","127.0.0.1","--rdma-port",str(service.lock_port),"--data-port",str(service.data_port),
                 "--txn-state-port",str(service.txn_state_port),"--format","csv"]
            row=validate_row(subprocess.run(cmd,check=True,capture_output=True,text=True,env=env,timeout=45).stdout,0,True,"native-rdma-state")
            assert int(row["deescalations"])>0
            assert int(row["escalation_attempts"])==int(row["escalations"])+int(row["escalation_failures"])
            assert int(row["deescalation_attempts"])==int(row["deescalations"])+int(row["deescalation_failures"])
            service.check()
