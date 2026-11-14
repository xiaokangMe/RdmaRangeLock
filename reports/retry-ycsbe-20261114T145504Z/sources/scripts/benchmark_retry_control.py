#!/usr/bin/env python3
"""Edit CONFIG to compare unchanged retries with adaptive pacing on YCSB-E."""
import csv,datetime,hashlib,itertools,json,os,random,statistics,subprocess,time
from pathlib import Path

CONFIG={
    'runner':'build-precision/rrl_workload_runner',
    'algorithms':['record-2pl','next-key','gap-lock','prior-key','hierarchical','exact-range','precision-lock'],
    'policies':['no-wait','wait-die','wound-wait'],
    'thetas':[0.0,1.5], 'retry_modes':['off','adaptive'], 'rounds':3,
    'keys':2_097_152,'transactions':5000,'ops':10,'range':32,'threads':32,
    'seed':8,'warmup':1,'duration':2,'max_attempts':1000,'wait_timeout_ms':100,
    'hierarchy_bucket':64,'timeout':90,
}


def main():
    root=Path(__file__).resolve().parents[1];os.chdir(root)
    config=dict(CONFIG)
    # A separate quick probe is useful before launching the whole campaign.
    if os.environ.get('RRL_RETRY_PROBE')=='1':
        config.update(algorithms=['precision-lock','hierarchical','record-2pl'],thetas=[1.5],rounds=1,warmup=0,duration=1)
    runner=(root/config['runner']).resolve()
    out=root/'reports'/('retry-ycsbe-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir(parents=True,exist_ok=False)
    files=['include/txn/retry_backoff.h','include/txn/policy_workload.h','src/txn/record_workload.cpp','src/txn/precision_workload.cpp','benchmarks/workload_runner.cpp','scripts/benchmark_retry_control.py']
    meta={'config':config,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'affinity':sorted(os.sched_getaffinity(0)),
          'runner_sha256':hashlib.sha256(runner.read_bytes()).hexdigest(),'source_sha256':{f:hashlib.sha256(Path(f).read_bytes()).hexdigest() for f in files}}
    for f in files:
        dst=out/'sources'/f;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(Path(f).read_bytes())
    for name,cmd in [('cpu',['lscpu']),('head',['git','rev-parse','HEAD']),('branch',['git','branch','--show-current']),('diff',['git','diff'])]:
        p=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'.txt')).write_text(p.stdout+p.stderr)
    (out/'metadata.json').write_text(json.dumps(meta,indent=2))
    rows=[];failures=[];number=0
    cases=list(itertools.product(config['algorithms'],config['policies'],config['thetas']))
    total=len(cases)*len(config['retry_modes'])*config['rounds']
    print('output='+str(out)+' trials='+str(total),flush=True)
    for repeat in range(1,config['rounds']+1):
        order=cases[:];random.Random(707+repeat).shuffle(order)
        for cc,policy,theta in order:
            modes=config['retry_modes'][:] if repeat%2 else list(reversed(config['retry_modes']))
            for mode in modes:
                number+=1;name=f'{number:03d}-r{repeat}-{cc}-{policy}-z{theta:g}-{mode}'
                backend='record' if cc=='record-2pl' else 'key' if cc in ['next-key','gap-lock','prior-key'] else 'hierarchy' if cc=='hierarchical' else 'precision' if cc=='precision-lock' else 'exact'
                cmd=[str(runner),'--index','v2','--cc',cc,f'--{backend}-lock-table','compact',
                     '--workload','ycsb-e','--keys',str(config['keys']),'--txns',str(config['transactions']),
                     '--ops',str(config['ops']),'--range',str(config['range']),'--threads',str(config['threads']),
                     '--zipf',str(theta),'--seed',str(config['seed']),'--wait-policy',policy,
                     '--execution-mode','threads','--max-attempts',str(config['max_attempts']),
                     '--wait-timeout-ms',str(config['wait_timeout_ms']),'--warmup',str(config['warmup']),
                     '--duration',str(config['duration']),'--retry-backoff',mode,'--format','csv']
                if cc=='hierarchical':cmd+=['--bucket',str(config['hierarchy_bucket']),'--hierarchy-implicit-root','1']
                rec={'command':cmd,'name':name,'round':repeat,'retry_mode':mode}
                try:
                    start=time.monotonic();p=subprocess.run(cmd,capture_output=True,text=True,timeout=config['timeout'])
                    rec.update(returncode=p.returncode,wall_seconds=time.monotonic()-start)
                    (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
                    assert p.returncode==0,p.stderr
                    parsed=list(csv.DictReader(p.stdout.splitlines()));assert len(parsed)==1
                    row=parsed[0];assert row['retry_backoff']==mode
                    for f in ['lock_errors','index_errors','lock_release_failures','failed_transactions']:assert int(row[f])==0,(f,row[f])
                    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
                    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
                    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
                    row.update(run=name,round=repeat,theta=theta,retry_mode=mode);rows.append(row)
                    print(f"{number}/{total} {cc} {policy} z={theta:g} {mode}: TPS={row['throughput_committed_txn_s']} exhausted={row['retry_exhausted']}",flush=True)
                except Exception as e:
                    rec['error']=str(e);failures.append(rec);print(f'{number}/{total} FAIL {name}: {e}',flush=True)
                (out/(name+'.json')).write_text(json.dumps(rec,indent=2))
                if rows:
                    with (out/'results.csv').open('w',newline='') as f:
                        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
                (out/'failures.json').write_text(json.dumps(failures,indent=2))
    meta.update(finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),passed=len(rows),failures=len(failures))
    (out/'metadata.json').write_text(json.dumps(meta,indent=2))
    print('COMPLETE '+str(out),flush=True)
    return bool(failures)

if __name__=='__main__':raise SystemExit(main())
