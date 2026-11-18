#!/usr/bin/env python3
"""Capture a live all-thread stack as debugger parent; no root/ptrace override."""
import json
import os
from pathlib import Path
import signal
import subprocess
import time

root = Path(__file__).resolve().parents[1]
out = root / "reports" / "aries-kvl-contention-20261118"
out.mkdir(exist_ok=True)
binary = root / "build-aries-release/rrl_aries_kvl_runner"
cmd = ["gdb", "-q", "-batch", "-ex", "set pagination off", "-ex", "set confirm off",
       "-ex", "run", "-ex", "thread apply all bt 12", "-ex", "detach", "--args", str(binary),
       "--threads", "32", "--theta", "0", "--warmup", "1", "--duration", "8"]
(out / "gdb-command.json").write_text(json.dumps(cmd, indent=2)+"\n")
log = out / "gdb-live-stack.log"
with log.open("w") as stream:
    proc = subprocess.Popen(cmd, cwd=root, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic()+90
    interrupted = False
    while proc.poll() is None:
        if not interrupted and "; measure" in log.read_text(errors="replace"):
            time.sleep(1)
            os.kill(proc.pid, signal.SIGINT)
            interrupted = True
        if time.monotonic() > deadline:
            os.killpg(proc.pid, signal.SIGTERM)
            raise RuntimeError("gdb capture deadline exceeded")
        time.sleep(.1)
    if proc.returncode or not interrupted:
        raise RuntimeError("gdb capture failed: " + str(proc.returncode))
print(log, flush=True)
