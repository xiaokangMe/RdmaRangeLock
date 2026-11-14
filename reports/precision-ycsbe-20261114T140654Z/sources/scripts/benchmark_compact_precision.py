#!/usr/bin/env python3
"""Sequential same-trace YCSB-E generic/full/fast Precision comparison.

Edit CONFIG before a run. Wait for other runner campaigns to finish; retain
raw output, errors, source snapshot, hashes and every individual trial.
"""
import csv,datetime,hashlib,itertools,json,os,random,statistics,subprocess,time
from pathlib import Path

CONFIG = {
    'runner': 'build-precision/rrl_workload_runner',
    'keys': 2_097_152, 'transactions': 5_000, 'ops': 10, 'range': 32,
    'threads': 32, 'seed': 8, 'thetas': [0.0, 0.9, 1.5],
    'policies': ['no-wait', 'wait-die', 'wound-wait'],
    'rounds': 3, 'warmup': 1, 'duration': 3,
    'max_attempts': 1000, 'wait_timeout_ms': 100,
    'timeout': 120, 'max_idle_wait': 1800,
}


def other_campaigns(exclude=()):
    result=[]
    for path in Path('/proc').iterdir():
        if not path.name.isdigit() or int(path.name) in (os.getpid(),*exclude):continue
        try:
            parts=(path/'cmdline').read_bytes().split(b'\0')
            if not parts:continue
            executable=Path(parts[0].decode(errors='replace')).name
            if executable=='rrl_workload_runner' or (executable.startswith('python') and any(
                Path(arg.decode(errors='replace')).name.startswith('benchmark_') for arg in parts[1:])):
                result.append(int(path.name))
        except (FileNotFoundError,ProcessLookupError,PermissionError):pass
    return result


def main():
    root=Path(__file__).resolve().parents[1];os.chdir(root)
    runner=(root/CONFIG['runner']).resolve()
    out=root/'reports'/('precision-ycsbe-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir(parents=True,exist_ok=False)
    print('output='+str(out),flush=True)
    start=time.monotonic()
    while other_campaigns():
        if time.monotonic()-start>CONFIG['max_idle_wait']:raise RuntimeError('other campaign still running')
        print('waiting for other campaigns '+str(other_campaigns()),flush=True);time.sleep(10)
    files=['CMakeLists.txt','benchmarks/workload_runner.cpp','include/txn/policy_workload.h',
           'include/txn/record_workload.h','include/lock/record_lock_table.h',
           'src/txn/precision_workload.cpp','src/txn/compact_precision_plan.h',
           'test/compact_precision_test.cpp','test/compact_precision_runner_test.py',
           'scripts/benchmark_compact_precision.py']
    metadata={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'config':CONFIG,
              'affinity':sorted(os.sched_getaffinity(0)),'loadavg':os.getloadavg(),
              'runner_sha256':hashlib.sha256(runner.read_bytes()).hexdigest(),
              'sha256':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}}
    for f in files:
        target=out/'sources'/f;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((root/f).read_bytes())
    for name,cmd in [('cpu',['lscpu']),('head',['git','rev-parse','HEAD']),('branch',['git','branch','--show-current']),('diff',['git','diff'])]:
        p=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'.txt')).write_text(p.stdout+p.stderr)
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    rows=[];failures=[];number=0
    cases=list(itertools.product(CONFIG['thetas'],CONFIG['policies']))
    for repeat in range(1,CONFIG['rounds']+1):
        order=cases[:];random.Random(20261114+repeat).shuffle(order)
        for theta,policy in order:
            variants=[('generic','full'),('compact','full'),('compact','fast')]
            if repeat%2==0:variants.reverse()
            for backend,metrics in variants:
                number+=1;name=f'{number:03d}-r{repeat}-z{theta:g}-{policy}-{backend}-{metrics}'
                while other_campaigns():
                    print('waiting before '+name,flush=True);time.sleep(10)
                command=[str(runner),'--index','v2','--cc','precision-lock','--precision-lock-table',backend,
                    '--workload','ycsb-e','--keys',str(CONFIG['keys']),'--txns',str(CONFIG['transactions']),
                    '--ops',str(CONFIG['ops']),'--range',str(CONFIG['range']),'--threads',str(CONFIG['threads']),
                    '--seed',str(CONFIG['seed']),'--zipf',str(theta),'--wait-policy',policy,
                    '--execution-mode','threads','--max-attempts',str(CONFIG['max_attempts']),
                    '--wait-timeout-ms',str(CONFIG['wait_timeout_ms']),'--warmup',str(CONFIG['warmup']),
                    '--duration',str(CONFIG['duration']),'--format','csv']
                if metrics=='fast':command+=['--precision-latency','0','--precision-statistics','0']
                rec={'name':name,'command':command,'round':repeat,'backend':backend,'metrics':metrics,'theta':theta}
                try:
                    started=time.monotonic();overlap=[]
                    with (out/(name+'.stdout')).open('w') as stdout,(out/(name+'.stderr')).open('w') as stderr:
                        process=subprocess.Popen(command,stdout=stdout,stderr=stderr)
                        while process.poll() is None:
                            overlap.extend(other_campaigns((process.pid,)))
                            if time.monotonic()-started>CONFIG['timeout']:
                                process.kill();process.wait();raise RuntimeError('runner timeout')
                            time.sleep(.2)
                    rec.update(returncode=process.returncode,wall_seconds=time.monotonic()-started,overlap=sorted(set(overlap)))
                    if overlap:raise RuntimeError('another campaign overlapped; trial invalid')
                    if process.returncode:raise RuntimeError((out/(name+'.stderr')).read_text())
                    parsed=list(csv.DictReader((out/(name+'.stdout')).read_text().splitlines()));assert len(parsed)==1
                    row=parsed[0]
                    for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):assert int(row[field])==0,(field,row[field])
                    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
                    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
                    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
                    row.update(run=name,round=repeat,backend=backend,metrics=metrics,theta=theta);rows.append(row)
                except Exception as error:rec['error']=str(error);failures.append(rec)
                (out/(name+'.json')).write_text(json.dumps(rec,indent=2))
                if rows:
                    with (out/'results.csv').open('w',newline='') as f:
                        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
                (out/'failures.json').write_text(json.dumps(failures,indent=2))
                print(f'{number}/{len(cases)*3*CONFIG["rounds"]} {name} failures={len(failures)}',flush=True)
    metadata.update(completed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),valid_trials=len(rows),failures=len(failures))
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    lines=['# Precision CAS YCSB-E comparison','','Each cell is the median of three independent runs; all raw runs retained.',
           f'CONFIG: `{CONFIG}`','','| theta | policy | generic/full TPS | compact/full TPS | compact/fast TPS | full speedup |',
           '|---:|---|---:|---:|---:|---:|']
    for theta,policy in cases:
        cells=[]
        for backend,metrics in [('generic','full'),('compact','full'),('compact','fast')]:
            sample=[float(row['throughput_committed_txn_s']) for row in rows if row['theta']==theta and row['wait_policy']==policy and row['backend']==backend and row['metrics']==metrics]
            cells.append(statistics.median(sample) if len(sample)==CONFIG['rounds'] else float('nan'))
        lines.append(f'| {theta:g} | {policy} | {cells[0]:,.1f} | {cells[1]:,.1f} | {cells[2]:,.1f} | {cells[1]/cells[0]:.2f}x |')
    lines+=['','This is a fixed-admission specialization: only possible Lu keys have CAS words. The admitted write universe and per-operation predicate mapping are compiled before measurement. It is not an unrestricted online predicate service.',
            'Fast mode disables optional latency/operation/checksum metrics; commits, aborts, retries, unfinished and errors are still counted.',
            'The finite trace is repeated; warmup may turn inserts into overwrites. No result claims continuous keyspace growth.',
            f'Failures: {len(failures)}. See results.csv for per-trial retries, wait, retry_exhausted and logical_unfinished.','']
    (out/'summary.md').write_text('\n'.join(lines))
    print('COMPLETE '+str(out),flush=True)
    return bool(failures)

if __name__=='__main__':raise SystemExit(main())
