#!/usr/bin/env python3
"""Shared-data correctness and small workload checks; never a formal matrix."""
import argparse
import datetime
import json
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
from shared_rdma_services import SharedServices
from check_wait_policies import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-shared")
    parser.add_argument("--cross-host", action="store_true")
    parser.add_argument("--seconds", type=int, default=1)
    parser.add_argument("--endurance", type=int, default=0)
    parser.add_argument("--only")
    parser.add_argument("--skip-contract", action="store_true")
    args = parser.parse_args()
    def interrupted(signum, frame):raise SystemExit(128+signum)
    for sig in (signal.SIGTERM,signal.SIGINT):signal.signal(sig,interrupted)
    root = Path(__file__).resolve().parent.parent
    build = (root / args.build).resolve()
    logs = root / "experiments/logs" / ("shared-check-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    logs.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("RRL_")}
    env["RRL_RDMA_GID_INDEX"] = "3"
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-i",
           str(Path.home() / ".ssh/rdmarangelock_deploy"), "dell@10.102.35.67"]
    remote = None
    if args.cross_host:
        remote = subprocess.check_output(ssh + ["mktemp -d /tmp/rrl-shared-check.XXXXXX"], text=True).strip()
        if not remote.startswith("/tmp/rrl-shared-check."): raise RuntimeError("unsafe remote directory")
        subprocess.run(["scp"] + ssh[1:-1] + [str(build / n) for n in ("rrl_workload_runner", "rrl_rdma_shared_tree_test")]
                       + ["dell@10.102.35.67:" + remote + "/"], check=True)
    cases = []
    def run(cmd, name, runenv=env, timeout=180):
        (logs / (name + ".command.json")).write_text(json.dumps(cmd))
        r = subprocess.run(cmd, env=runenv, capture_output=True, text=True, timeout=timeout)
        (logs / (name + ".stdout")).write_text(r.stdout)
        (logs / (name + ".stderr")).write_text(r.stderr)
        if r.returncode: raise RuntimeError((name, r.returncode, r.stdout[-1000:], r.stderr))
        return r.stdout
    config = root / "experiments/configs/rdma_shared_smoke.json"
    if not args.skip_contract:
        with SharedServices(build, config, logs / "contract-services", env) as service:
            ctest_output=run(["ctest", "--test-dir", str(build), "--output-on-failure"], "ctest", service.test_env())
            if "Skipped" in ctest_output or "100% tests passed" not in ctest_output:
                raise RuntimeError("all hardware CTest cases must execute, not skip")
            # A separate executable/process must see values committed by CTest.
            run([str(build / "rrl_rdma_shared_tree_test"), "--read", "777", "12345"], "local-independent-read", service.test_env())
            def shared_command(action, on_remote):
                e = service.test_env("10.0.0.3" if on_remote else "127.0.0.1")
                cmd = ["env"] + [k + "=" + v for k, v in e.items() if k.startswith("RRL_")]
                cmd += [(remote if on_remote else str(build)) + "/rrl_rdma_shared_tree_test"] + action
                return ssh + [shlex.join(cmd)] if on_remote else cmd
            holder = subprocess.Popen(shared_command(["--hold-range"], False), stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            try:
                if not select.select([holder.stdout], [], [], 30)[0]: raise RuntimeError("holder readiness timeout")
                line = holder.stdout.readline()
                if not line.startswith("holding "): raise RuntimeError((line, holder.stderr.read()))
                run(shared_command(["--try-insert"], args.cross_host), "cross-process-phantom-insert")
                out, err = holder.communicate("release\n", timeout=30)
                if holder.returncode: raise RuntimeError((out, err))
                (logs / "cross-process-phantom.stdout").write_text(line + out)
            finally:
                if holder.poll() is None: holder.kill(); holder.wait()
            if args.cross_host:
                run(shared_command([], True), "cross-contract")
                run(shared_command(["--read", "777", "12345"], True), "cross-independent-read")
            # Two independent processes on distinct hosts update one protected
            # shared record. The final exact sum detects private copies/lost writes.
            commands = [shared_command(["--increment", "500"], remote_host) for remote_host in (False, args.cross_host)]
            peers = [subprocess.Popen(c, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env) for c in commands]
            try:
                for number, peer in enumerate(peers):
                    out, err = peer.communicate(timeout=60)
                    (logs / ("concurrent-increment-" + str(number) + ".stdout")).write_text(out)
                    (logs / ("concurrent-increment-" + str(number) + ".stderr")).write_text(err)
                    if peer.returncode: raise RuntimeError(("concurrent increment", number, peer.returncode, err))
            finally:
                for peer in peers:
                    if peer.poll() is None: peer.kill(); peer.wait()
            run(shared_command(["--read", "777", "13345"], True), "cross-increment-exact-sum")
            run([str(build / "rrl_rdma_shared_tree_test"), "--poison"], "poison", service.test_env())
    if not args.skip_contract:
        # Inject a real shared-data fault while workload transactions are active.
        # It must terminate the run and poison coordination, not publish normal
        # results after releasing an uncertain active transaction.
        import socket, struct
        with SharedServices(build, config, logs / "failure-services", env) as service:
            failure_cmd=[str(build/"rrl_workload_runner"),"--config",str(config),"--index","rdma",
                         "--rdma-host","127.0.0.1","--data-port",str(service.data_port),
                         "--rdma-port",str(service.lock_port),"--coordinator-port",str(service.coordinator_port),
                         "--duration","30","--format","csv"]
            process=subprocess.Popen(failure_cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                run([str(build/"rrl_rdma_shared_tree_test"),"--poison-active"],"active-data-poison",service.test_env())
                out,err=process.communicate(timeout=30)
                (logs/"active-failure.stdout").write_text(out);(logs/"active-failure.stderr").write_text(err)
                if process.returncode!=3:raise RuntimeError(("in-flight fault did not fail measurement",process.returncode,out,err))
                with socket.create_connection(("127.0.0.1",service.coordinator_port),timeout=5) as sock:
                    message=[0x52524c5741495431,1,1,0,1000000000]+[0]*11
                    sock.sendall(struct.pack("!16Q",*message));reply=b""
                    while len(reply)<128:
                        block=sock.recv(128-len(reply))
                        if not block:raise RuntimeError("missing poison response")
                        reply+=block
                    if struct.unpack("!16Q",reply)[1]!=4:raise RuntimeError("uncertain data did not poison coordinator")
            finally:
                if process.poll() is None:process.kill();process.wait()
    workloads = ("ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order")
    for policy in ("no-wait", "wait-die", "wound-wait"):
        for cc in ("rdma-record-2pl", "rdma-gap-lock", "rdma-next-key", "rdma-hierarchical", "rdma-predicate"):
            for workload in workloads:
                if args.endurance and (policy == "no-wait" or cc not in ("rdma-next-key", "rdma-hierarchical") or workload != "ycsb-e"): continue
                name = "-".join((cc, workload, policy))
                if args.only and name != args.only: continue
                cfg = json.loads(config.read_text())
                cfg.update(workload=workload, cc=cc, wait_policy=policy, threads=4,
                           duration_seconds=args.endurance or args.seconds)
                path = logs / (name + ".json"); path.write_text(json.dumps(cfg, indent=2))
                with SharedServices(build, path, logs / name, env) as service:
                    # The server reads the same config bytes; only network ports
                    # and client role are supplied separately.
                    flags = ["--workload", workload, "--index", "rdma", "--cc", cc,
                             "--keys", str(cfg["key_count"]), "--txns", str(cfg["transaction_count"]),
                             "--ops", str(cfg["operations_per_transaction"]), "--range", str(cfg["range_length"]),
                             "--threads", "4", "--seed", str(cfg["seed"]), "--order", str(cfg["order"]),
                             "--duration", str(cfg["duration_seconds"]), "--wait-policy", policy,
                             "--data-port", str(service.data_port), "--rdma-port", str(service.lock_port),
                             "--coordinator-port", str(service.coordinator_port), "--format", "csv"]
                    host = "10.0.0.3" if args.cross_host else "127.0.0.1"
                    binary = (remote if args.cross_host else str(build)) + "/rrl_workload_runner"
                    cmd = [binary] + flags + ["--rdma-host", host]
                    if args.cross_host: cmd = ssh + [shlex.join(["env", "RRL_RDMA_GID_INDEX=3"] + cmd)]
                    row = validate(run(cmd, name, timeout=(args.endurance or args.seconds) + 180))
                    assert row["data_backend"] == "shared-rdma" and int(row["data_rdma_operations"]) > 0
                    assert int(row["data_read_bytes"]) > 0 and int(row["data_write_bytes"]) > 0
                    service.check(); cases.append(dict(name=name, committed=row["committed"], retries=row["retries"]))
                    (logs / "progress.json").write_text(json.dumps(cases, indent=2))
    (logs / "validated.json").write_text(json.dumps(dict(status="PASS", cases=cases), indent=2))
    print("PASS;", len(cases), "shared data cases; logs:", logs, flush=True)

if __name__ == "__main__": main()
