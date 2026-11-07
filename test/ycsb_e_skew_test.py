"""Small local / real RDMA workload checks, not a performance sweep."""
import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/"scripts"))
from shared_rdma_services import SharedServices
from run_shared_matrices import validate_row,validate_workload_parameters,cases,config_fingerprint,lock_service_flags
from make_ycsb_e_skew_configs import make_configs,DEFAULT_THETAS
exe=Path(sys.argv[1]).resolve();build=exe.parent
base=json.loads((root/"experiments/configs/ycsb_e_skew_correctness.json").read_text())
expected=dict(keys=128,transactions=24,operations=2,ycsb_unique_keys=1,zipf=.9)
protocols=("record-2pl","gap-lock","next-key","prior-key","hierarchical","predicate","exact-range")
def run(cfg,path,extra=()):
    path.write_text(json.dumps(cfg))
    output=subprocess.run([str(exe),"--config",str(path),"--format","csv",*extra],
                          check=True,capture_output=True,text=True,timeout=45).stdout
    row=validate_row(output,0,cfg.get("index")=="rdma")
    observed_config=dict(expected,zipf=cfg["zipf"],
        request_distribution=cfg.get("request_distribution",cfg.get("requestdistribution","auto")),
        hotspot_data_fraction=cfg.get("hotspot_data_fraction",cfg.get("hotspotdatafraction",.2)),
        hotspot_opn_fraction=cfg.get("hotspot_opn_fraction",cfg.get("hotspotopnfraction",.8)))
    validate_workload_parameters(row,observed_config,8,8)
    assert row["execution_mode"]=="threads" and row["coroutines"]=="1" and row["coroutine_switches_total"]=="0"
    assert row["workload"]=="ycsb-e-skew" and int(row["committed"])>0 and int(row["range_reads"])>0
    return row

with tempfile.TemporaryDirectory(prefix="rrl-e-skew-") as directory:
    d=Path(directory);path=d/"case.json"
    for protocol in protocols:
        row=run(dict(base,cc=protocol),path)
    cfg=dict(base,cc="next-key",zipf=0,request_distribution="uniform")
    uniform=run(cfg,path,["--zipf","0"])
    assert uniform["trace_key_distribution"]=="uniform"
    for theta in (-1,float("inf"),float("nan")):
        r=subprocess.run([str(exe),"--workload","ycsb-e-skew","--zipf",str(theta)],capture_output=True,text=True,timeout=10)
        assert r.returncode!=0,"invalid theta accepted"
    alias={k:v for k,v in base.items() if k not in ("request_distribution","hotspot_data_fraction","hotspot_opn_fraction")}
    alias.update(requestdistribution="hotspot",hotspotdatafraction=.1,hotspotopnfraction=.9)
    alias_row=run(alias,path)
    assert alias_row["trace_key_distribution"]=="hotspot"
    for theta in (0,.9,.99):
        run(dict(base,request_distribution="zipfian",zipf=theta),path)
    bad=dict(row,trace_zipf_theta="0.4")
    try:validate_workload_parameters(bad,dict(expected,request_distribution="hotspot"),8,8)
    except ValueError:pass
    else:raise AssertionError("different theta accepted into campaign")
    for field in ("trace_key_distribution","trace_seed","trace_range_length","trace_key_count","trace_hotspot_data_fraction","trace_hotspot_opn_fraction"):
        bad=dict(row);bad[field]="other" if field=="trace_key_distribution" else "999"
        try:validate_workload_parameters(bad,dict(expected,request_distribution="hotspot"),8,8)
        except ValueError:pass
        else:raise AssertionError("mixed trace geometry accepted: "+field)
    bad={k:v for k,v in row.items() if not k.startswith("trace_")}
    try:validate_workload_parameters(bad)
    except ValueError:pass
    else:raise AssertionError("untraceable skew result accepted")
    source=json.loads((root/"experiments/configs/formal_native_record_gap_capacity_2m_10s.json").read_text())
    configs=make_configs(source)
    assert configs[0]["request_distribution"]=="uniform"
    assert {(c["hotspot_opn_fraction"],c["hotspot_data_fraction"]) for c in configs[1:]}=={(.8,.2),(.8,.1),(.9,.2),(.9,.1)}
    zipf_configs=make_configs(source,distribution="zipfian")
    assert tuple(c["zipf"] for c in zipf_configs)==DEFAULT_THETAS
    assert len({config_fingerprint(c) for c in configs})==5
    for c in configs:
        assert c["workloads"]==["ycsb-e-skew"] and c["keys"]==2097152
        assert c["expected_local_cases"]==24 and c["expected_cross_host_cases"]==21
        assert c["threads"]==[32] and c["execution_mode"]=="threads" and c["coroutines"]==1
        assert {point[1] for point in cases(c,"local")}=={32}
    for thetas in ([0,0],[-1],[float("nan")],[float("inf")]):
        try:make_configs(source,thetas,distribution="zipfian")
        except ValueError:pass
        else:raise AssertionError("invalid skew plan accepted")
    print("E-skew local protocols, trace identity, validation and generation-only planner: OK",flush=True)
    if not os.environ.get("RRL_TEST_RDMA_PORT"):sys.exit(77)
    env=dict(os.environ)
    for protocol in protocols:
        cfg=dict(base,index="rdma",cc="rdma-"+protocol,native_object_locks=int(protocol=="record-2pl"))
        path.write_text(json.dumps(cfg))
        with SharedServices(build,path,d/protocol,env,native=True) as service:
            extra=["--rdma-host","127.0.0.1","--rdma-port",str(service.lock_port),
                   "--data-port",str(service.data_port),"--txn-state-port",str(service.txn_state_port),
                   *lock_service_flags(cfg)]
            row=run(cfg,path,extra)
            assert row["coordination"]=="native-rdma-state" and int(row["data_rdma_operations"])>0
            service.check()
print("E-skew seven native protocols / remote index+data+lock path: OK")
