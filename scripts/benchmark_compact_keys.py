#!/usr/bin/env python3
"""Sequential same-seed YCSB-E comparison; retain commands and raw output."""
import argparse,csv,datetime,hashlib,itertools,json,os,platform,random,subprocess,time
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--runner',default='build-record/rrl_workload_runner')
parser.add_argument('--output',required=True)
parser.add_argument('--rounds',type=int,default=3)
parser.add_argument('--duration',type=int,default=3)
args=parser.parse_args()
out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
runner=str(Path(args.runner).resolve())
metadata={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'platform':platform.platform(),'affinity':sorted(os.sched_getaffinity(0)),
          'loadavg':os.getloadavg(),'arguments':vars(args)}
sources=['CMakeLists.txt','benchmarks/workload_runner.cpp','include/lock/classic_lock_table.h',
         'include/lock/record_lock_table.h','include/txn/policy_workload.h','include/txn/record_workload.h',
         'src/txn/record_workload.cpp','test/classic_lock_table_test.cpp','test/compact_key_runner_test.py',
         'scripts/benchmark_compact_keys.py']
metadata['sha256']={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sources}
metadata['runner_sha256']=hashlib.sha256(Path(runner).read_bytes()).hexdigest()
for name in sources:
    target=out/'sources'/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(Path(name).read_bytes())
for name,command in [('cpu',['lscpu']),('git',['git','rev-parse','HEAD']),
                     ('branch',['git','branch','--show-current']),('diff',['git','diff'])]:
    p=subprocess.run(command,capture_output=True,text=True);(out/(name+'.txt')).write_text(p.stdout+p.stderr)
(out/'metadata.json').write_text(json.dumps(metadata,indent=2))
rows=[];failures=[];number=0
cases=list(itertools.product(('next-key','gap-lock','prior-key'),('no-wait','wait-die','wound-wait'),(1,8,32,64)))
for repeat in range(args.rounds):
    order=cases[:];random.Random(793+repeat).shuffle(order)
    for cc,policy,threads in order:
        for backend in (('generic','compact') if (number//2+repeat)%2==0 else ('compact','generic')):
            number+=1;name=f'{number:03d}-r{repeat+1}-{cc}-{policy}-t{threads}-{backend}'
            command=[runner,'--index','v2','--cc',cc,'--key-lock-table',backend,
                     '--workload','ycsb-e','--keys','100000','--txns','5000','--ops','10',
                     '--range','32','--zipf','0.99','--seed','7','--threads',str(threads),
                     '--wait-policy',policy,'--execution-mode','threads','--max-attempts','1000',
                     '--wait-timeout-ms','100','--warmup','1','--duration',str(args.duration),'--format','csv']
            started=time.monotonic()
            record={'name':name,'command':command,'round':repeat+1}
            try:
                p=subprocess.run(command,capture_output=True,text=True,timeout=90)
                (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
                record.update(returncode=p.returncode,wall_seconds=time.monotonic()-started)
                assert p.returncode==0,p.stderr
                parsed=list(csv.DictReader(p.stdout.splitlines()));assert len(parsed)==1
                row=parsed[0];row.update(run=name,round=str(repeat+1),metrics_mode='full')
                for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):
                    assert int(row[field])==0,(field,row[field])
                assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
                rows.append(row)
                with (out/'results.csv').open('w',newline='') as f:
                    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
            except Exception as e:
                record['error']=str(e);failures.append(record)
                if isinstance(e,subprocess.TimeoutExpired):
                    for stream in ('stdout','stderr'):
                        data=getattr(e,stream) or b''
                        (out/(name+'.'+stream)).write_bytes(data.encode() if isinstance(data,str) else data)
            (out/(name+'.json')).write_text(json.dumps(record,indent=2))
            if number%6==0 or 'error' in record:
                print(f'{number}/{args.rounds*len(cases)*2} completed; failures={len(failures)}; last={name}',flush=True)
(out/'failures.json').write_text(json.dumps(failures,indent=2))
print(f'Finished: {len(rows)} valid results; {len(failures)} failures; {out}',flush=True)
raise SystemExit(bool(failures))
