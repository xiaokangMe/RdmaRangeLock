import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'scripts'))
from shared_rdma_services import SharedServices
if not os.environ.get('RRL_TEST_RDMA_PORT'):sys.exit(77)
build=Path(sys.argv[1]).resolve().parent
for mode in ('strict','root','deneva','partial','split-abort'):
 with tempfile.TemporaryDirectory(prefix='rrl-ancestor-') as tmp:
  cfg=json.loads((root/'experiments/configs/rdma_shared_smoke.json').read_text())
  cfg.update(workload='ycsb-a',key_count=3 if mode=='root' else 256,transaction_count=1,operations_per_transaction=1,zipf=0,order=3 if mode=='root' else 8,threads=1,coroutines=1,hierarchy_layout='leaf',tree_pages=32768)
  path=Path(tmp)/'config.json';path.write_text(json.dumps(cfg))
  with SharedServices(build,path,Path(tmp)/'services',dict(os.environ),native=True,data_binary='rrl_ancestor_runtime_test') as service:
   env=service.test_env();env['RRL_TEST_CONFIG']=str(path)
   subprocess.run([str(build/'rrl_ancestor_runtime_test'),mode],env=env,check=True,timeout=45);service.check()
