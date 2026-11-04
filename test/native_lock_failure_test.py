#!/usr/bin/env python3
"""Isolate fail-stop servers; do not poison the shared CTest fixture."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from shared_rdma_services import SharedServices
if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
build=Path(sys.argv[1]).resolve().parent
root=Path(__file__).resolve().parents[1]
for mode in ("clean","running","committing","gate"):
    with tempfile.TemporaryDirectory(prefix="rrl-native-lock-fault-") as tmp:
        with SharedServices(build,root/"experiments/configs/rdma_shared_smoke.json",tmp,dict(os.environ),native=True) as service:
            subprocess.run([str(build/"rrl_native_lock_failure_test"),mode],env=service.test_env(),check=True,timeout=30)
            if mode=="clean":service.check()
            else:
                if service.processes[1].wait(timeout=10)!=2:raise RuntimeError("lock service did not fail closed")
                if any(p.poll() is not None for i,p in enumerate(service.processes) if i!=1):raise RuntimeError("unrelated data/state service exited")
print("Native lock-only disconnect: strict commit fencing, held gates and clean cleanup OK")
