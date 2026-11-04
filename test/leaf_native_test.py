import os,sys,subprocess
from pathlib import Path
if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
root=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,str(root/"scripts/check_leaf_runtime.py"),"--build",str(Path(sys.argv[1]).resolve().parent),"--contracts-only"],check=True,timeout=150)
