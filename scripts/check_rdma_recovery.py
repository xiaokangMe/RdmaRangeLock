#!/usr/bin/env python3
"""Run correctness tests on an isolated RoCE server; never runs a matrix.

RRL_RDMA_GID_INDEX/RRL_RDMA_DEVICE select hardware. Default GID is 3.
Usage: python3 scripts/check_rdma_recovery.py --build build-recovery-fix
"""
import argparse
import csv
import io
import os
from pathlib import Path
import socket
import subprocess
import tempfile


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def check_workload_smoke(root, build, env, port):
    cases = [("v1", "none", 1), ("v2", "none", 4)] + [
        ("v2", cc, 4) for cc in ("record-2pl", "gap-lock", "next-key", "hierarchical",
                                  "rdma-next-key", "rdma-hierarchical")]
    for index, cc, threads in cases:
        result = subprocess.run([
            str(build / "rrl_workload_runner"), "--config",
            str(root / "experiments/configs/recovery-smoke.json"),
            "--index", index, "--cc", cc, "--threads", str(threads),
            "--rdma-host", "127.0.0.1", "--rdma-port", str(port), "--format", "csv"],
            env=env, check=True, capture_output=True, text=True, timeout=30)
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        if len(rows) != 1 or None in rows[0] or any(value is None for value in rows[0].values()):
            raise RuntimeError("invalid workload CSV: " + result.stdout)
        row = rows[0]
        if any(int(row[key]) for key in ("lock_errors", "index_errors", "lock_release_failures",
                                         "failed_transactions", "rdma_timeouts", "rdma_completion_errors")):
            raise RuntimeError("hard workload error: " + result.stdout)
        if int(row["transactions"]) != 100 or int(row["committed"]) + int(row["aborted"]) != 100:
            raise RuntimeError("invalid transaction accounting: " + result.stdout)
    print("8 short workload checks: CSV schema/accounting/errors passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-recovery-fix")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = (root / args.build).resolve()
    env = os.environ.copy()
    env.setdefault("RRL_RDMA_GID_INDEX", "3")
    env["RRL_TEST_RDMA_HOST"] = "127.0.0.1"
    with tempfile.TemporaryDirectory(prefix="rrl-recovery-") as tmp:
        for case in ("suite", "unsafe", "unknown"):
            port = unused_port()
            env["RRL_TEST_RDMA_PORT"] = str(port)
            with open(Path(tmp) / (case + ".log"), "w+") as log:
                server = subprocess.Popen([str(build / "rrl_rdma_lock_server"), str(port)],
                                          env=env, stdout=log, stderr=log)
                try:
                    if case == "suite":
                        subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"],
                                       env=env, check=True, timeout=180)
                        check_workload_smoke(root, build, env, port)
                        if server.poll() is not None:
                            raise RuntimeError("server died during correctness suite")
                    else:
                        subprocess.run([str(build / "rrl_rdma_recovery_test"), case],
                                       env=env, check=True, timeout=30)
                        if server.wait(timeout=15) != 2:
                            raise RuntimeError("uncertain mutation did not fail closed")
                        log.flush()
                        log.seek(0)
                        if "uncertain mutation while holding gate" not in log.read():
                            raise RuntimeError("server failed for an unrelated reason")
                except BaseException:
                    log.flush()
                    log.seek(0)
                    print(log.read(), flush=True)
                    raise
                finally:
                    if server.poll() is None:
                        server.terminate()
                    server.wait(timeout=15)
    # Explicit environment + unavailable server must fail, never skip.
    env["RRL_TEST_RDMA_PORT"] = str(unused_port())
    result = subprocess.run([str(build / "rrl_rdma_lock_loopback_test")], env=env, timeout=15)
    if result.returncode != 1:
        raise RuntimeError("configured connection failure was not reported as test failure")
    print("RDMA suite + unsafe mutation rejection + unavailable-server check: OK", flush=True)


if __name__ == "__main__":
    main()
