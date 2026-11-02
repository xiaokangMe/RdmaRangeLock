#!/usr/bin/env python3
"""Sequential, ten-second local/RoCE matrices with capacity and result gates."""
import argparse
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import traceback


def command(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def read_rows(path):
    with Path(path).open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    if len(reader.fieldnames) != len(set(reader.fieldnames)) or any(None in r or None in r.values() for r in rows):
        raise RuntimeError("bad CSV schema: " + str(path))
    return rows


ERRORS = ("lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
          "rdma_timeouts", "rdma_completion_errors", "rdma_count_errors", "rdma_local_errors")


def validate(rows, duration, expected=None):
    if expected is not None and len(rows) != expected:
        raise RuntimeError(f"expected {expected} cases; got {len(rows)}")
    for row in rows:
        if int(row["elapsed_ns"]) < duration * 1_000_000_000:
            raise RuntimeError("measurement window below configured duration")
        if int(row["committed"]) + int(row["aborted"]) != int(row["transactions"]):
            raise RuntimeError("transaction accounting mismatch")
        if any(int(row[field]) for field in ERRORS):
            raise RuntimeError("hard measurement error")
        if int(row["rdma_profile_enabled"]):
            raise RuntimeError("formal profile must be disabled")
        if row["cc"].startswith("rdma-") and int(row["rdma_slot_lookup_linear"]) != 1:
            raise RuntimeError("wrong RDMA lookup policy")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="experiments/configs/formal_both_10s_20261102.json")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    if subprocess.check_output(["git", "status", "--porcelain"], text=True).strip():
        raise SystemExit("source/config must be committed before running")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    config = json.loads((root / args.config).read_text())
    if config["duration_seconds"] < 10 or config["runs"] < 3:
        raise SystemExit("formal matrices require >=10 seconds and >=3 repetitions")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    logdir = root / "experiments/logs" / ("verified-" + stamp)
    logdir.mkdir()
    build = root / "build-formal"
    key = Path.home() / ".ssh/rdmarangelock_deploy"
    ssh = ["ssh", "-i", str(key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
           "-o", "StrictHostKeyChecking=yes", "-o",
           "UserKnownHostsFile=/tmp/rrl-correctness-known-hosts", "dell@10.102.35.67"]
    env = {k: v for k, v in os.environ.items() if not k.startswith("RRL_")}
    env.update(RRL_DURATION=str(config["duration_seconds"]), RRL_RUNS=str(config["runs"]),
               RRL_KEYS=str(config["keys"]), RRL_TRANSACTIONS=str(config["transactions"]),
               RRL_OPERATIONS=str(config["operations"]), RRL_ZIPF=str(config["zipf"]),
               RRL_TPCC_ITEMS=str(config["tpcc_items"]), RRL_BUILD_JOBS="4",
               RRL_RDMA_GID_INDEX=str(config["gid_index"]), RRL_RDMA_PROFILE="0",
               RRL_SLOT_LOOKUP="linear", RRL_LOCAL_GATE="1", RRL_GATE_RETRY="64",
               RRL_RDMA_CONNECTIONS="auto", RRL_SYNC_REMOTE="0")
    for name, keyname in (("THREADS", "threads"), ("WORKLOADS", "workloads"),
                          ("RANGE_LENGTHS", "ranges"), ("BUCKETS", "buckets"),
                          ("LOCAL_CCS", "local_ccs"), ("RDMA_CCS", "rdma_ccs")):
        env["RRL_" + name] = ",".join(map(str, config[keyname]))
    state = dict(source_commit=commit, config=config, started=datetime.datetime.now().isoformat(),
                 log_dir=str(logdir), pid=os.getpid(), status="building",
                 environment={k: v for k, v in env.items() if k.startswith("RRL_")})
    statepath = root / "experiments/results" / ("verified-campaign-" + stamp + ".json")

    def publish(status):
        state["status"] = status
        state["updated"] = datetime.datetime.now().isoformat()
        # Temporary file stays in ignored log directory.
        temp = logdir / "state.tmp"
        temp.write_text(json.dumps(state, indent=2))
        temp.replace(statepath)
        (Path("/tmp/rrl-verified-campaign-state.json")).write_text(json.dumps(state, indent=2))
        print(status, flush=True)

    def remote(command_text, **kwargs):
        return subprocess.run(ssh + [command_text], check=True, **kwargs)

    publish("building")
    for host, argv in (("server", ["uname", "-a"]), ("cpu", ["lscpu"])):
        (logdir / (host + ".txt")).write_text(subprocess.check_output(argv, text=True))
    (logdir / "remote-hardware.txt").write_text(
        subprocess.check_output(ssh + ["uname -a; lscpu; ibv_devinfo"], text=True))
    with (logdir / "build.log").open("w") as log:
        command(["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                stdout=log, stderr=log)
        command(["cmake", "--build", str(build), "-j4"], stdout=log, stderr=log)
    remote_parent = subprocess.check_output(ssh + ["mktemp -d /tmp/rrl-formal10-XXXXXX"], text=True).strip()
    if not remote_parent.startswith("/tmp/rrl-formal10-") or remote_parent.count("/") != 2:
        raise RuntimeError("unexpected remote temp path")
    remote_root = remote_parent + "/RdmaRangeLock"
    remote_build = remote_root + "/build-formal"
    archive = logdir / "source.tar"
    command(["git", "archive", "--output", str(archive), commit])
    state["source_archive_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    remote("mkdir " + shlex.quote(remote_root))
    with archive.open("rb") as handle:
        remote("tar -x -C " + shlex.quote(remote_root), stdin=handle)
    state["remote_root"] = remote_root
    remote("printf '%s\\n' " + shlex.quote(commit) + " >" + shlex.quote(remote_root + "/SOURCE_COMMIT"))
    with (logdir / "remote-build.log").open("w") as log:
        remote("cmake -S " + shlex.quote(remote_root) + " -B " + shlex.quote(remote_build) +
               " -DCMAKE_BUILD_TYPE=Release && cmake --build " + shlex.quote(remote_build) + " -j4",
               stdout=log, stderr=log)
    env.update(RRL_REMOTE_ROOT=remote_root, RRL_REMOTE_BUILD_DIR=remote_build)
    server = None
    try:
        publish("correctness")
        with (logdir / "correctness.log").open("w") as log:
            command(["python3", "scripts/check_rdma_recovery.py", "--build", str(build)],
                    env=env, stdout=log, stderr=log)
        with socket.socket() as sock:
            sock.bind(("0.0.0.0", 0))
            port = sock.getsockname()[1]
        serverlog = (logdir / "preflight-server.log").open("w")
        server = subprocess.Popen([str(build / "rrl_rdma_lock_server"), str(port)],
                                  env=env, stdout=serverlog, stderr=serverlog)
        case_env = env.copy()
        case_env.update(RRL_TEST_RDMA_HOST="127.0.0.1", RRL_TEST_RDMA_PORT=str(port))
        # Startup is synchronized by the test's real connection handshake.
        with (logdir / "cross-correctness.log").open("w") as log:
            remote("env RRL_RDMA_GID_INDEX=3 RRL_TEST_RDMA_HOST=10.0.0.3 RRL_TEST_RDMA_PORT=" +
                   str(port) + " ctest --test-dir " + shlex.quote(remote_build) +
                   " -R 'rrl_rdma_(workspace|profile|lock_loopback)_test' --output-on-failure",
                   stdout=log, stderr=log)
        publish("capacity-preflight")
        points = [(cc, length, threads, bucket, 8)
                  for cc, lengths, bucket in (("rdma-next-key", [8, 32, 128], 0),
                                             ("rdma-hierarchical", [128], 256))
                  for length in lengths for threads in (1, 8, 32)]
        points += [(cc, 128, 32, bucket, seed) for seed in (9, 10)
                   for cc, bucket in (("rdma-next-key", 0), ("rdma-hierarchical", 256))]
        pfpath = root / "experiments/results" / ("capacity-preflight-verified-" + stamp + ".csv")
        writer = None
        with pfpath.open("x", newline="") as handle:
            for index, (cc, length, threads, bucket, seed) in enumerate(points):
                argv = [remote_build + "/rrl_workload_runner", "--index", "v2", "--cc", cc,
                        "--workload", "ycsb-e", "--keys", str(config["keys"]),
                        "--txns", str(config["transactions"]), "--ops", str(config["operations"]),
                        "--range", str(length), "--threads", str(threads), "--seed", str(seed),
                        "--zipf", str(config["zipf"]), "--duration", str(config["duration_seconds"]),
                        "--rdma-host", config["server_roce"], "--rdma-port", str(port), "--format", "csv"]
                if bucket:
                    argv += ["--bucket", str(bucket)]
                with (logdir / ("preflight-" + str(index) + ".stderr")).open("w") as error:
                    result = remote("timeout 180 env RRL_RDMA_GID_INDEX=3 RRL_RDMA_PROFILE=0 "
                                    "RRL_SLOT_LOOKUP=linear RRL_LOCAL_GATE=1 RRL_GATE_RETRY=64 " +
                                    shlex.join(argv), stdout=subprocess.PIPE, stderr=error, text=True)
                (logdir / ("preflight-" + str(index) + ".stdout")).write_text(result.stdout)
                rows = list(csv.DictReader(io.StringIO(result.stdout)))
                validate(rows, config["duration_seconds"], 1)
                row = dict(source_commit=commit, point=index, range=length, bucket=bucket, seed=seed, **rows[0])
                if writer is None:
                    writer = csv.DictWriter(handle, fieldnames=list(row))
                    writer.writeheader()
                writer.writerow(row)
                handle.flush()
                if int(rows[0]["lock_capacity_aborts"]) or int(rows[0]["lock_gate_aborts"]):
                    raise RuntimeError("capacity/gate preflight failed; do not launch full matrices")
                state["preflight_completed"] = index + 1
                publish("capacity-preflight")
        state["preflight_csv"] = str(pfpath)
        server.terminate()
        server.wait(timeout=15)
        server = None
        serverlog.close()
        for mode in ("local", "cross-host"):
            if subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip() != commit:
                raise RuntimeError("source commit changed during campaign")
            with socket.socket() as sock:
                sock.bind(("0.0.0.0", 0))
                env["RRL_RDMA_PORT"] = str(sock.getsockname()[1])
            matrixlog = logdir / mode
            matrixlog.mkdir()
            env["RRL_LOG_DIR"] = str(matrixlog)
            state["active_mode"] = mode
            publish("running-" + mode)
            before = set((root / "experiments/results").glob("formal-" + mode + "-*.csv"))
            with (logdir / (mode + "-driver.log")).open("w") as log:
                result = subprocess.run(["bash", "scripts/run_formal_matrix.sh", "--mode", mode],
                                        env=env, stdout=log, stderr=log)
            new = set((root / "experiments/results").glob("formal-" + mode + "-*.csv")) - before
            if len(new) != 1:
                raise RuntimeError("cannot identify unique result for " + mode)
            path = next(iter(new))
            state[mode + "_csv"] = str(path)
            state[mode + "_exit_code"] = result.returncode
            rows = read_rows(path)
            expected = config["expected_local_cases" if mode == "local" else "expected_cross_host_cases"]
            state[mode + "_cases"] = len(rows)
            state[mode + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            publish("checking-" + mode)
            validate(rows, config["duration_seconds"], expected)
            if result.returncode:
                raise RuntimeError("matrix driver failed: " + mode)
        state["finished"] = datetime.datetime.now().isoformat()
        publish("complete")
    except BaseException as error:
        state["error"] = str(error)
        state["traceback"] = traceback.format_exc()
        publish("failed")
        raise
    finally:
        if server is not None and server.poll() is None:
            server.terminate()
            server.wait(timeout=15)


if __name__ == "__main__":
    main()
