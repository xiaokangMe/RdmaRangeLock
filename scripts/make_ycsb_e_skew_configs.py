#!/usr/bin/env python3
"""Generate isolated E-skew campaigns only; never build, connect or run a workload."""
import argparse
import copy
import itertools
import json
import math
from pathlib import Path
from run_shared_matrices import cases,validate_hotspot

DEFAULT_THETAS=(0.,.4,.7,.9)

def make_configs(base, thetas=None, threads=(32,), ranges=(32,), bucket=None, wait_policy=None,
                 distribution="hotspot", data_fractions=(.2,.1), opn_fractions=(.8,.9), buckets=None):
    if bucket is not None and buckets is not None:raise ValueError("use bucket or buckets, not both")
    widths=list(buckets) if buckets is not None else [bucket] if bucket is not None else [16,64,256]
    if not threads or not ranges or not widths or any(type(v) is not int or v<1 for v in (*threads,*ranges,*widths)) or len(set(widths))!=len(widths):
        raise ValueError("threads/ranges/buckets must be positive; bucket widths must be distinct")
    if wait_policy is not None and wait_policy not in ("no-wait","wait-die","wound-wait"):raise ValueError("invalid wait policy")
    if distribution=="zipfian":
        thetas=DEFAULT_THETAS if thetas is None else thetas
        if not thetas or any(isinstance(t,bool) or not isinstance(t,(int,float)) or not math.isfinite(t) or t<0 for t in thetas) or len(set(thetas))!=len(thetas):
            raise ValueError("theta values must be distinct, finite and nonnegative")
        profiles=[("theta-"+format(t,".12g"),"zipfian",t,.2,.8) for t in thetas]
    elif distribution=="hotspot":
        if thetas is not None:raise ValueError("theta sweep requires --distribution zipfian")
        if not data_fractions or not opn_fractions or len(set(data_fractions))!=len(data_fractions) or len(set(opn_fractions))!=len(opn_fractions):
            raise ValueError("hotspot fractions must be nonempty and distinct")
        profiles=[("uniform","uniform",0.,.2,.8)]
        for data,opn in itertools.product(data_fractions,opn_fractions):
            validate_hotspot(dict(hotspot_data_fraction=data,hotspot_opn_fraction=opn))
            if not 0<data<1:raise ValueError("sweep requires both hot and cold data sets")
            profiles.append(("hot-ops-"+format(opn,".12g")+"-data-"+format(data,".12g"),"hotspot",0.,data,opn))
    else:raise ValueError("distribution must be hotspot or zipfian")
    result=[]
    for tag,requested,theta,data,opn in profiles:
        config=copy.deepcopy(base)
        config.update(name="ycsb-e-skew-"+tag,workloads=["ycsb-e-skew"],zipf=theta,
                      request_distribution=requested,hotspot_data_fraction=data,hotspot_opn_fraction=opn,
                      threads=list(threads),ranges=list(ranges),buckets=widths,
                      runs=1,seeds=[8],repetition_policy="single-run",
                      wait_policies=[wait_policy] if wait_policy is not None else ["no-wait","wait-die","wound-wait"],
                      hierarchy_layout="bucket",hierarchy_behavior="strict",
                      hierarchy_escalation_threshold=0,hierarchy_deescalate_after=0,
                      coordination_backend="native-rdma-state",native_object_locks=1,ycsb_unique_keys=1,
                      execution_mode="threads",coroutines=1,
                      local_ccs=["none","record-2pl","gap-lock","next-key","prior-key","hierarchical","exact-range"],
                      rdma_ccs=["rdma-record-2pl","rdma-gap-lock","rdma-next-key","rdma-prior-key","rdma-hierarchical","rdma-exact-range"])
        config["expected_local_cases"]=len(list(cases(config,"local")))
        config["expected_cross_host_cases"]=len(list(cases(config,"cross-host")))
        result.append(config)
    return result

def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base",type=Path,default=root/"experiments/configs/formal_native_record_gap_capacity_2m_10s.json")
    parser.add_argument("--out",required=True,type=Path)
    parser.add_argument("--distribution",choices=["hotspot","zipfian"],default="hotspot")
    parser.add_argument("--thetas",nargs="+",type=float)
    parser.add_argument("--hotspot-data-fractions",nargs="+",type=float,default=[.2,.1])
    parser.add_argument("--hotspot-opn-fractions",nargs="+",type=float,default=[.8,.9])
    parser.add_argument("--threads",nargs="+",type=int,default=[32])
    parser.add_argument("--ranges",nargs="+",type=int,default=[32])
    geometry=parser.add_mutually_exclusive_group()
    geometry.add_argument("--bucket",type=int,help="optional single bucket width")
    geometry.add_argument("--buckets",nargs="+",type=int,help="default: 16 64 256")
    parser.add_argument("--wait-policy",choices=["no-wait","wait-die","wound-wait"],default=None,help="optional single-policy override; default: all three")
    args=parser.parse_args()
    configs=make_configs(json.loads(args.base.read_text()),args.thetas,args.threads,args.ranges,args.bucket,args.wait_policy,
                         args.distribution,args.hotspot_data_fractions,args.hotspot_opn_fractions,args.buckets)
    names=[c["name"].replace(".","p")+".json" for c in configs]
    if len(set(names))!=len(names):raise SystemExit("profile names collide")
    if any((args.out/name).exists() for name in names):raise SystemExit("refuse to overwrite existing configs")
    args.out.mkdir(parents=True,exist_ok=True)
    for name,c in zip(names,configs):
        path=args.out/name;path.write_text(json.dumps(c,indent=2)+"\n")
        print(str(path)+": local="+str(c["expected_local_cases"])+", RDMA="+str(c["expected_cross_host_cases"]))
    print("Configurations generated only. No workload was started.")

if __name__=="__main__":main()
