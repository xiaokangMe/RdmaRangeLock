import json,os,sys,tempfile,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/"scripts"))
from shared_rdma_services import SharedServices
build=Path(sys.argv[1]).resolve().parent
subprocess.run([str(build/"rrl_leaf_object_test")],check=True,timeout=30)
if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
with tempfile.TemporaryDirectory(prefix="rrl-leaf-object-") as tmp:
 cfg=json.loads((root/"experiments/configs/rdma_shared_smoke.json").read_text())
 cfg.update(workload="ycsb-a",key_count=128,transaction_count=100,operations_per_transaction=4,zipf=0,order=8,hierarchy_layout="leaf",threads=2,coroutines=1)
 path=Path(tmp)/"config.json";path.write_text(json.dumps(cfg))
 with SharedServices(build,path,Path(tmp)/"services",dict(os.environ),native=True) as service:
  env=service.test_env();env["RRL_TEST_CONFIG"]=str(path)
  subprocess.run([str(build/"rrl_leaf_object_test"),"--remote"],env=env,check=True,timeout=30)
  service.check()
