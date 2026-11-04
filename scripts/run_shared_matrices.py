#!/usr/bin/env python3
"""Local / shared-data RoCE matrices. --smoke exercises the SAME case executor."""
import argparse
import csv
import datetime
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import shlex
import signal
import shutil
import subprocess
from shared_rdma_services import SharedServices

ERRORS = ("lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
          "rdma_timeouts", "rdma_completion_errors", "rdma_count_errors", "rdma_local_errors",
          "rdma_gate_transport_errors")


def cases(config, mode):
    for workload in config["workloads"]:
        for length in config["ranges"] if workload == "ycsb-e" else [0]:
            for cc in config["rdma_ccs"] if mode == "cross-host" else config["local_ccs"]:
                for bucket in config["buckets"] if cc in ("hierarchical", "rdma-hierarchical") else [0]:
                    for threads, run, policy in itertools.product(config["threads"], range(config["runs"]), config["wait_policies"]):
                        # Unlocked baseline has no conflict policy; run once.
                        if cc == "none" and policy != config["wait_policies"][0]: continue
                        yield workload, length, cc, bucket, threads, run, policy


def validate_row(output, seconds, shared, expected_coordination=None):
    reader = csv.DictReader(io.StringIO(output)); rows = list(reader)
    if len(rows) != 1 or len(reader.fieldnames) != len(set(reader.fieldnames)) or None in rows[0] or None in rows[0].values():
        raise RuntimeError("invalid runner CSV schema")
    row = rows[0]
    if any(int(row[k]) for k in ERRORS): raise RuntimeError("hard error in measurement")
    if int(row["lock_capacity_aborts"]) or int(row["lock_gate_aborts"]):
        raise RuntimeError("capacity/gate saturation; formal case invalid")
    if int(row["transactions"]) <= 0: raise RuntimeError("no transaction attempts")
    if int(row["elapsed_ns"]) < seconds * 1000000000: raise RuntimeError("short measurement window")
    if "metrics_schema" in row:
        # Zero commits under no-wait contention is an observable protocol
        # outcome, not a malformed CSV. Keep the strict accounting checks.
        n=lambda k:int(row[k])
        if n("metrics_schema")!=2 or n("transactions")!=n("committed")+n("aborted")+n("failed_transactions"):
            raise RuntimeError("attempt accounting mismatch")
        if n("logical_started")!=n("committed")+n("logical_unfinished")+n("logical_failed") or n("transactions")!=n("logical_started")+n("retries"):
            raise RuntimeError("logical transaction accounting mismatch")
        if n("retries")!=n("completed_retries")+n("unfinished_retries"):
            raise RuntimeError("retry accounting mismatch")
        for name in ("successful_attempt","logical_completion"):
            if n(name+"_count")!=n("committed"):raise RuntimeError("latency population mismatch")
        for name in ("successful_attempt","logical_completion","lock_wait","lock_call","release"):
            q=[n(name+"_"+f+"_ns") for f in ("p50","p95","p99","p999","max")]
            if q!=sorted(q):raise RuntimeError("quantile order mismatch")
        if row["wait_policy"]=="no-wait" and (n("lock_wait_count") or n("lock_wait_total_ns")):
            raise RuntimeError("no-wait unexpectedly blocked on transaction locks")
    if shared:
        if row["index"] != "rdma" or row["data_backend"] != "shared-rdma":
            raise RuntimeError("shared data path absent; refuse old lock-only result")
        # An all-abort case reads nothing: zero data traffic is a legitimate
        # outcome of the protocol, not a broken data path. Only require traffic
        # from cases that actually committed work.
        if int(row["committed"]) > 0 and (int(row["data_rdma_operations"]) <= 0 or int(row["data_read_bytes"]) <= 0):
            raise RuntimeError("shared data path idle while transactions committed")
        if row["coordination"] not in ("tcp-control+rdma-slots","native-rdma-state"):
            raise RuntimeError("wrong coordination path")
        if expected_coordination and row["coordination"]!=expected_coordination:
            raise RuntimeError("coordination differs from campaign configuration")
    elif row["data_backend"] != "local": raise RuntimeError("local matrix unexpectedly uses RDMA data")
    row["progress_status"]="commits" if int(row["committed"]) else "no-commits"
    return row


def config_from_environment():
    def ls(name, default, cast=str): return [cast(v) for v in os.environ.get("RRL_" + name, default).split(",")]
    def number(name, default): return int(os.environ.get("RRL_" + name, default))
    runs = number("RUNS", "3")
    return dict(duration_seconds=number("DURATION", "10"), runs=runs, seeds=list(range(8, 8+runs)),
                keys=number("KEYS", "10000"), transactions=number("TRANSACTIONS", "5000"),
                operations=number("OPERATIONS", "10"), zipf=float(os.environ.get("RRL_ZIPF", "0.99")),
                tpcc_items=number("TPCC_ITEMS", "5"), threads=ls("THREADS", "1,2,4,8,16,32", int),
                workloads=ls("WORKLOADS", "ycsb-a,ycsb-e,tpcc-payment,tpcc-new-order"),
                ranges=ls("RANGE_LENGTHS", "8,32,128", int), buckets=ls("BUCKETS", "16,64,256", int),
                local_ccs=ls("LOCAL_CCS", "none,record-2pl,gap-lock,next-key,hierarchical"),
                rdma_ccs=ls("RDMA_CCS", "rdma-next-key,rdma-hierarchical"),
                wait_policies=ls("WAIT_POLICIES", "no-wait"), max_attempts=number("MAX_ATTEMPTS", "1000"),
                order=number("TREE_ORDER", "128"), tree_pages=number("TREE_PAGES", "8192"),
                tree_records=number("TREE_RECORDS", "262144"), gid_index=number("RDMA_GID_INDEX", "3"),
                server_roce=os.environ.get("RRL_SERVER_ROCE", "10.0.0.3"),
                rdma_connections=os.environ.get("RRL_RDMA_CONNECTIONS", "auto"),
                wait_timeout_ms=number("WAIT_TIMEOUT_MS","5000"), tree_latch_timeout_ms=number("TREE_LATCH_TIMEOUT_MS","10000"),
                lock_budget=number("LOCK_BUDGET","16384"), local_gate=number("LOCAL_GATE","1"),
                gate_retry=number("GATE_RETRY","64"), profile=0, slot_lookup=os.environ.get("RRL_SLOT_LOOKUP","linear"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="experiments/configs/formal_shared_10s.json")
    parser.add_argument("--mode", choices=("local", "cross-host", "both"), default="both")
    parser.add_argument("--build", default="build-shared")
    parser.add_argument("--smoke", action="store_true", help="one second, one repetition; tagged non-formal")
    parser.add_argument("--environment-config", action="store_true")
    parser.add_argument("--validation-seconds", type=int, default=1)
    parser.add_argument("--keep-geometry", action="store_true", help="smoke timing with configured key/trace/thread/range geometry")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--resume", default=None,
                        help="existing result CSV to finish; cases already present are skipped")
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise SystemExit(128+signum)
    for sig in (signal.SIGTERM,signal.SIGINT):signal.signal(sig,interrupted)
    root = Path(__file__).resolve().parent.parent; os.chdir(root)
    config = config_from_environment() if args.environment_config else json.loads((root / args.config).read_text())
    config.setdefault("coordination_backend","native-rdma-state")
    config.setdefault("coroutines",1);config.setdefault("coroutine_stack_bytes",262144);config.setdefault("warmup_seconds",10)
    if config["coordination_backend"] not in ("native-rdma-state","tcp-control+rdma-slots"):
        raise SystemExit("invalid coordination backend")
    if not 1<=config["coroutines"]<=4096 or not 65536<=config["coroutine_stack_bytes"]<=16777216 or not 0<=config["warmup_seconds"]<=600000:
        raise SystemExit("runtime geometry bounds")
    if config["coordination_backend"]=="tcp-control+rdma-slots" and config["coroutines"]!=1:
        raise SystemExit("TCP compatibility path requires one coroutine per worker")
    config.setdefault("wait_policies", ["no-wait"])
    config.setdefault("max_attempts", 1000)
    config.setdefault("order", 128); config.setdefault("tree_pages", 8192); config.setdefault("tree_records", 262144)
    config.setdefault("wait_timeout_ms",5000);config.setdefault("tree_latch_timeout_ms",10000)
    config.setdefault("lock_budget",16384);config.setdefault("local_gate",1);config.setdefault("gate_retry",64)
    config.setdefault("profile",0);config.setdefault("slot_lookup","linear")
    if not 1<=config["wait_timeout_ms"]<=600000 or not 1<=config["tree_latch_timeout_ms"]<=600000 or not 1<=config["lock_budget"]<=1048576:
        raise SystemExit("invalid wait/latch timeout or metadata lock budget")
    if config["profile"]!=0 or config["slot_lookup"] not in ("linear","index") or config["local_gate"] not in (0,1):
        raise SystemExit("formal profiling must be off; invalid lookup or local gate")
    config.setdefault("rdma_connections", "auto")
    config.setdefault("gid_index", 3); config.setdefault("server_roce", "10.0.0.3")
    if not 1<=args.validation_seconds<=600:raise SystemExit("validation seconds must be 1..600")
    if args.smoke:
        config.update(duration_seconds=args.validation_seconds, runs=1, seeds=[config["seeds"][0]],
                      warmup_seconds=min(config["warmup_seconds"],args.validation_seconds))
        if not args.keep_geometry:
            config.update(keys=min(config["keys"], 128), transactions=min(config["transactions"], 200),
                          operations=min(config["operations"], 4), threads=[1, 4], ranges=[8], buckets=[16], order=16)
    elif args.no_build:
        raise SystemExit("formal runs must build from the recorded committed source")
    elif config["duration_seconds"] < 10 or config["runs"] < 3:
        raise SystemExit("formal matrices require >=10 seconds and >=3 repetitions; use --smoke for validation")
    if max(config["threads"])*config["coroutines"]>4096:
        raise SystemExit("active execution contexts exceed runtime/state capacity")
    if str(config["rdma_connections"])!="auto" and (not str(config["rdma_connections"]).isdigit() or int(config["rdma_connections"])<1):
        raise SystemExit("rdma_connections must be auto or positive integer")
    if len(config["seeds"]) != config["runs"] or len(set(config["seeds"])) != config["runs"]:
        raise SystemExit("one distinct seed per repetition is required")
    if set(config["wait_policies"]) - {"no-wait", "wait-die", "wound-wait"}: raise SystemExit("invalid wait policy")
    if not config["wait_policies"] or not config["workloads"] or not config["threads"] or not config["ranges"] or not config["buckets"]: raise SystemExit("empty dimensions")
    if set(config["local_ccs"]) - {"none", "record-2pl", "gap-lock", "next-key", "hierarchical", "predicate"}: raise SystemExit("invalid local protocols")
    if set(config["rdma_ccs"]) - {"rdma-record-2pl", "rdma-gap-lock", "rdma-next-key", "rdma-hierarchical", "rdma-predicate"}: raise SystemExit("invalid RDMA protocols")
    if subprocess.check_output(["git", "diff", "HEAD", "--"], text=True).strip() and not args.smoke:
        raise SystemExit("commit source/config before formal run")
    untracked = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "include", "src", "benchmarks", "test", "experiments/configs"], text=True)
    if untracked.strip() and not args.smoke: raise SystemExit("uncommitted source/config files")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    kind = "smoke-not-formal" if args.smoke else "formal-shared-data"
    logs = root / "experiments/logs" / (kind + "-" + stamp); logs.mkdir(parents=True)
    results = root / "experiments/results"; results.mkdir(exist_ok=True)
    build = (root / args.build).resolve()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    tracked = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], text=True).split("\0")
    manifest = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in tracked if p and (root / p).is_file()}
    (logs / "source-sha256.json").write_text(json.dumps(manifest, indent=2))
    (logs / "config.json").write_text(json.dumps(config, indent=2))
    env = {k: v for k, v in os.environ.items() if not k.startswith("RRL_")}
    env.update(RRL_RDMA_GID_INDEX=str(config["gid_index"]), RRL_SLOT_LOOKUP=config["slot_lookup"], RRL_RDMA_PROFILE="0",
               RRL_LOCAL_GATE=str(config["local_gate"]),RRL_GATE_RETRY=str(config["gate_retry"]))
    if not args.no_build:
        with (logs / "build.log").open("w") as log:
            subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"], check=True, stdout=log, stderr=log)
            subprocess.run(["cmake", "--build", str(build), "-j4"], check=True, stdout=log, stderr=log)
    binaries = ("rrl_workload_runner", "rrl_rdma_data_server", "rrl_rdma_lock_server", "rrl_wait_coordinator", "rrl_rdma_txn_server")
    if not args.smoke and any(hashlib.sha256((root/p).read_bytes()).hexdigest()!=digest for p,digest in manifest.items()):
        raise SystemExit("source changed during build; discard this build")
    original_build=build
    pinned=logs/"binaries";pinned.mkdir()
    for name in binaries:shutil.copy2(build/name,pinned/name)
    build=pinned
    binary_hashes = {n: hashlib.sha256((build / n).read_bytes()).hexdigest() for n in binaries}
    state = dict(status="starting", kind=kind, source_commit=commit, config=config, log_dir=str(logs), no_progress_cases=0,
                 binary_sha256=binary_hashes, pid=os.getpid(), local_completed=0, cross_host_completed=0, failed_cases=0,
                 preflight_completed=0, error=None, result_files={})
    state_file = results / (kind + "-" + stamp + ".json")
    def publish(status):
        state["status"] = status; state["updated"] = datetime.datetime.now().isoformat()
        text = json.dumps(state, indent=2)
        temp = logs / "state.tmp"; temp.write_text(text); temp.replace(state_file)
        (Path("/tmp/rrl-shared-campaign-state.json")).write_text(text)
        print(status, state["local_completed"], state["cross_host_completed"], flush=True)
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-i",
           os.environ.get("RRL_SSH_KEY", str(Path.home() / ".ssh/rdmarangelock_deploy")),
           os.environ.get("RRL_REMOTE_HOST", "dell@10.102.35.67")]
    remote_dir = None
    modes = ["local", "cross-host"] if args.mode == "both" else [args.mode]
    def command(cmd, name, remote=False, timeout=900):
        actual = ssh + [shlex.join(["env"] + [k+"="+v for k,v in env.items() if k.startswith("RRL_")] + cmd)] if remote else cmd
        (logs / (name + ".command.json")).write_text(json.dumps(actual))
        proc = subprocess.run(actual, capture_output=True, text=True, timeout=timeout, env=env)
        (logs / (name + ".stdout")).write_text(proc.stdout)
        (logs / (name + ".stderr")).write_text(proc.stderr)
        if proc.returncode: raise RuntimeError((name, proc.returncode, proc.stderr[-2000:]))
        return proc.stdout
    def execute(mode, point, label, duration=None):
        workload,length,cc,bucket,threads,run,policy = point
        if not args.smoke and subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip()!=commit:
            raise RuntimeError("HEAD changed during campaign; stop before another case")
        duration = config["duration_seconds"] if duration is None else duration
        cfg = dict(workload=workload, index="rdma", cc=cc, key_count=config["keys"],
                   transaction_count=config["transactions"], operations_per_transaction=config["operations"],
                   range_length=length, threads=threads, seed=config["seeds"][run], zipf=config["zipf"],
                   tpcc_items_per_order=config["tpcc_items"], order=config["order"],
                   tree_pages=config["tree_pages"], tree_records=config["tree_records"],
                   wait_policy=policy, max_attempts=config["max_attempts"], duration_seconds=duration,
                   coroutines=config["coroutines"],coroutine_stack_bytes=config["coroutine_stack_bytes"],warmup_seconds=config["warmup_seconds"],
                   wait_timeout_ms=config["wait_timeout_ms"], tree_latch_timeout_ms=config["tree_latch_timeout_ms"], lock_budget=config["lock_budget"])
        path = logs / (label + ".json"); path.write_text(json.dumps(cfg, indent=2))
        flags = ["--index", "v2", "--cc", cc, "--workload", workload, "--keys", str(config["keys"]),
                 "--txns", str(config["transactions"]), "--ops", str(config["operations"]), "--range", str(length),
                 "--threads", str(threads), "--seed", str(cfg["seed"]), "--zipf", str(config["zipf"]),
                 "--items", str(config["tpcc_items"]), "--order", str(config["order"]),
                 "--duration", str(duration), "--format", "csv",
                 "--coroutines",str(config["coroutines"]),"--coroutine-stack-bytes",str(config["coroutine_stack_bytes"]),
                 "--warmup",str(config["warmup_seconds"])]
        if bucket: flags += ["--bucket", str(bucket)]
        if mode=="cross-host" and str(config["rdma_connections"])!="auto":
            flags += ["--rdma-connections", str(config["rdma_connections"])]
        if cc != "none": flags += ["--wait-policy", policy, "--max-attempts", str(config["max_attempts"]),
                                    "--wait-timeout-ms",str(config["wait_timeout_ms"]),"--lock-budget",str(config["lock_budget"])]
        if mode == "cross-host":
            flags[1] = "rdma"
            with SharedServices(build, path, logs / label, env, native=config["coordination_backend"]=="native-rdma-state") as service:
                flags += ["--tree-latch-timeout-ms",str(config["tree_latch_timeout_ms"]),"--data-port", str(service.data_port), "--rdma-port", str(service.lock_port),
                          ("--txn-state-port" if config["coordination_backend"]=="native-rdma-state" else "--coordinator-port"),
                          str(service.txn_state_port if config["coordination_backend"]=="native-rdma-state" else service.coordinator_port), "--rdma-host", config["server_roce"]]
                output = command([remote_dir + "/rrl_workload_runner"] + flags, label, True, duration+config["warmup_seconds"]+180)
                service.check()
        else: output = command([str(build / "rrl_workload_runner")] + flags, label, timeout=duration+config["warmup_seconds"]+180)
        row = validate_row(output, duration, mode == "cross-host", config["coordination_backend"] if mode=="cross-host" else None)
        if cc=="none":row["wait_policy"]="none"
        return dict(source_commit=commit, result_kind=kind, transport="rdma-cross-host-shared" if mode=="cross-host" else "local-no-rdma",
                    run=run+1, seed=cfg["seed"], range=length, bucket=bucket, zipf=config["zipf"], **row)
    try:
        if not args.smoke:
            publish("correctness")
            checks=["python3",str(root/"scripts/check_shared_rdma.py"),"--build",str(original_build)]
            if "cross-host" in modes:checks.append("--cross-host")
            command(checks,"correctness",timeout=900)
            native_checks=["python3",str(root/"scripts/check_native_runtime.py"),"--build",str(original_build)]
            if "cross-host" in modes:native_checks.append("--cross-host")
            command(native_checks,"native-correctness",timeout=900)
            if any(hashlib.sha256((original_build/n).read_bytes()).hexdigest()!=binary_hashes[n] for n in binaries):
                raise RuntimeError("build outputs changed during correctness gate; discard campaign")
        if "cross-host" in modes:
            remote_dir = subprocess.check_output(ssh + ["mktemp -d /tmp/rrl-shared-matrix.XXXXXX"], text=True).strip()
            if not remote_dir.startswith("/tmp/rrl-shared-matrix."): raise RuntimeError("unsafe remote directory")
            subprocess.run(["scp"] + ssh[1:-1] + [str(build / "rrl_workload_runner"), "{}:{}/".format(ssh[-1],remote_dir)], check=True)
            remote_hash = subprocess.check_output(ssh + ["sha256sum " + shlex.quote(remote_dir+"/rrl_workload_runner")], text=True).split()[0]
            if remote_hash != binary_hashes["rrl_workload_runner"]: raise RuntimeError("remote binary mismatch")
            state["remote_dir"] = remote_dir
            # Always preflight worst configured scan/thread/operation geometry
            # on the actual shared-data path, with fresh services per point.
            publish("capacity-preflight")
            preflight=[]
            for cc in config["rdma_ccs"]:
                for bucket in config["buckets"] if cc=="rdma-hierarchical" else [0]:
                    for run,policy in itertools.product(range(config["runs"]),config["wait_policies"]):
                        preflight.append(("ycsb-e",max(config["ranges"]),cc,bucket,max(config["threads"]),run,policy))
            state["preflight_expected"]=len(preflight)
            for i,point in enumerate(preflight):
                row=execute("cross-host",point,"preflight-"+str(i),duration=args.validation_seconds if args.smoke else 10)
                state.setdefault("preflight_rows",[]).append(row)
                state["preflight_completed"] += 1;publish("capacity-preflight")
        if args.preflight_only:
            publish("preflight-complete");return
        for mode in modes:
            rows=[]; outpath=results/(kind+"-"+mode+"-"+stamp+".csv")
            done=set()
            if args.resume:
                source=Path(args.resume)
                if not source.is_file(): raise SystemExit("resume file missing: "+str(source))
                previous=list(csv.DictReader(source.open(newline="")))
                wanted={"local":"local-no-rdma","cross-host":"rdma-cross-host-shared"}[mode]
                wrong=[r for r in previous if r.get("transport")!=wanted]
                if wrong: raise SystemExit("resume file holds %d %s rows, not %s"
                                           % (len(wrong),wrong[0].get("transport"),mode))
                for row in previous:
                    if row.get("source_commit")!=commit or row.get("result_kind")!=kind:
                        raise SystemExit("resume source/kind differs from this campaign")
                    expected_backend=config["coordination_backend"] if mode=="cross-host" else "local"
                    if row.get("coordination")!=expected_backend or int(row.get("coroutines",1))!=config["coroutines"] or int(row.get("warmup_seconds",0))!=config["warmup_seconds"]:
                        raise SystemExit("resume runtime/backend differs from this campaign")
                rows=list(previous)
                done={(r["workload"],str(r["range"]),r["cc"],str(r["bucket"]),
                       str(r["threads"]),r["wait_policy"],str(r["seed"])) for r in previous}
                print("resume: keeping %d rows from %s" % (len(rows),source),flush=True)
            state["result_files"][mode]=str(outpath)
            allpoints=list(cases(config,mode));state[mode+"_expected"]=len(allpoints)
            for index,point in enumerate(allpoints):
                label=mode+"-"+str(index)+"-"+point[0]+"-"+point[2]
                key=(point[0],str(point[1]),point[2],str(point[3]),str(point[4]),
                     point[6],str(config["seeds"][point[5]]))
                if key in done:
                    state["local_completed" if mode=="local" else "cross_host_completed"] += 1
                    publish(mode);continue
                state["current_case"]=dict(mode=mode,index=index,point=point);publish(mode)
                rows.append(execute(mode,point,label))
                if rows[-1]["progress_status"]=="no-commits":state["no_progress_cases"]+=1
                fields=list(dict.fromkeys(k for row in rows for k in row))
                temp=logs/"matrix.tmp"
                with temp.open("w",newline="") as f:
                    writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
                temp.replace(outpath)
                state["local_completed" if mode=="local" else "cross_host_completed"] += 1
            publish(mode+"-complete")
        state["current_case"]=None;publish("complete")
    except BaseException as e:
        state["failed_cases"] += 1;state["error"]=repr(e);publish("failed");raise

if __name__ == "__main__": main()
