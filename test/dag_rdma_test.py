import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/"scripts"))
from shared_rdma_services import SharedServices
from run_shared_matrices import validate_row
build=Path(sys.argv[1]).resolve().parent
if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
env=dict(os.environ)
with tempfile.TemporaryDirectory(prefix="rrl-dag-") as directory:
    d=Path(directory)
    for workload in ("ycsb-a","ycsb-e"):
      for policy in ("no-wait","wait-die","wound-wait"):
        cfg=dict(workload=workload,cc="rdma-hierarchical",key_count=128,transaction_count=24,
                 operations_per_transaction=4,range_length=8,threads=2,coroutines=2,seed=8,zipf=0,
                 order=8,tree_pages=256,tree_records=512,warmup_seconds=0,lock_budget=256,
                 hierarchy_layout="dag",hierarchy_index_paths=2,hierarchy_read_path=1,hierarchy_bucket_width=16)
        path=d/"dag.json";path.write_text(json.dumps(cfg))
        with SharedServices(build,path,d/(workload+"-"+policy),env,native=True) as service:
            subprocess.run([str(build/"rrl_dag_hierarchy_test"),"--remote",str(path)],env=service.test_env(),check=True,timeout=30)
            cmd=[str(build/"rrl_workload_runner"),"--config",str(path),"--index","rdma","--wait-policy",policy,
                 "--rdma-host","127.0.0.1","--rdma-port",str(service.lock_port),"--data-port",str(service.data_port),
                 "--txn-state-port",str(service.txn_state_port),"--format","csv"]
            result=subprocess.run(cmd,env=env,check=True,capture_output=True,text=True,timeout=45)
            row=validate_row(result.stdout,0,True,"native-rdma-state","dag","strict",0)
            assert int(row["hierarchy_index_paths"])==2 and int(row["hierarchy_read_path"])==1 and int(row["committed"])>0
            service.check()
print("Live RDMA DAG all-parent locking, topology rejection and native A/E paths: OK")
