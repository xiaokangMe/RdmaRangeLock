#!/usr/bin/env python3
"""YCSB-E generic vs five-mode CAS hierarchy, retaining all raw trials."""
import argparse,csv,datetime,hashlib,itertools,json,os,platform,random,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--runner',default='build-record/rrl_workload_runner')
p.add_argument('--output',required=True);p.add_argument('--rounds',type=int,default=3)
p.add_argument('--warmup',type=int,default=1);p.add_argument('--duration',type=int,default=2)
p.add_argument('--threads',default='1,8,32,64');p.add_argument('--widths',default='16,64,256')
p.add_argument('--keys',type=int,default=100000);p.add_argument('--txns',type=int,default=5000)
p.add_argument('--ops',type=int,default=10);p.add_argument('--range',type=int,default=32)
p.add_argument('--zipf',type=float,default=.99);p.add_argument('--policies',default='no-wait,wait-die,wound-wait')
p.add_argument('--sensitivity',action='store_true');a=p.parse_args()
out=Path(a.output);out.mkdir(parents=True,exist_ok=False);runner=str(Path(a.runner).resolve())
threads=[int(x) for x in a.threads.split(',')];widths=[int(x) for x in a.widths.split(',')]
policies=a.policies.split(',');rows=[];failures=[];number=0
files=['CMakeLists.txt','benchmarks/workload_runner.cpp','include/lock/hierarchy_lock_table.h',
 'include/txn/policy_workload.h','include/txn/record_workload.h','src/txn/hierarchical_plan.h',
 'src/txn/compact_hierarchy_plan.h','src/txn/record_workload.cpp','test/hierarchy_lock_table_test.cpp',
 'test/compact_hierarchy_runner_test.py','scripts/benchmark_compact_hierarchy.py']
meta={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'platform':platform.platform(),
 'affinity':sorted(os.sched_getaffinity(0)),'loadavg':os.getloadavg(),'arguments':vars(a),
 'runner_sha256':hashlib.sha256(Path(runner).read_bytes()).hexdigest(),
 'sha256':{f:hashlib.sha256(Path(f).read_bytes()).hexdigest() for f in files}}
for f in files:
 q=out/'sources'/f;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(Path(f).read_bytes())
for name,cmd in [('cpu',['lscpu']),('git',['git','rev-parse','HEAD']),('branch',['git','branch','--show-current'])]:
 q=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'.txt')).write_text(q.stdout+q.stderr)
(out/'metadata.json').write_text(json.dumps(meta,indent=2))
def run(case,repeat,backend,implicit=0,padded=0,metrics=1):
 global number
 number+=1;cc,policy,nthreads,width=case
 name=f'{number:03d}-r{repeat}-{cc}-{policy}-t{nthreads}-w{width}-{backend}-root{implicit}-pad{padded}-m{metrics}'
 cmd=[runner,'--index','v2','--cc','hierarchical','--hierarchy-lock-table',backend,
  '--workload','ycsb-e','--keys',str(a.keys),'--txns',str(a.txns),'--ops',str(a.ops),
  '--range',str(a.range),'--zipf',str(a.zipf),'--seed','7','--threads',str(nthreads),
  '--bucket',str(width),'--wait-policy',policy,'--execution-mode','threads',
  '--max-attempts','1000','--wait-timeout-ms','100','--warmup',str(a.warmup),
  '--duration',str(a.duration),'--hierarchy-implicit-root',str(implicit),
  '--hierarchy-padded',str(padded),'--hierarchy-latency',str(metrics),
  '--hierarchy-statistics',str(metrics),'--format','csv']
 rec={'name':name,'command':cmd,'round':repeat,'backend':backend,'implicit_root':implicit,'padded':padded,'metrics':metrics}
 (out/(name+'.json')).write_text(json.dumps(rec,indent=2))
 started=time.monotonic()
 try:
  result=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
  (out/(name+'.stdout')).write_text(result.stdout);(out/(name+'.stderr')).write_text(result.stderr)
  rec['wall_seconds']=time.monotonic()-started;rec['returncode']=result.returncode
  assert result.returncode==0,result.stderr
  parsed=list(csv.DictReader(result.stdout.splitlines()));assert len(parsed)==1
  row=parsed[0];row.update(run=name,round=str(repeat),root_variant=str(implicit),padding_variant=str(padded),metrics_mode='full' if metrics else 'fast')
  for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):assert int(row[field])==0,(field,row[field])
  assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
  assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
  assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
  rows.append(row)
  with (out/'results.csv').open('w',newline='') as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 except Exception as error:
  rec['error']=str(error);failures.append(rec)
  if isinstance(error,subprocess.TimeoutExpired):
   for stream in ('stdout','stderr'):
    data=getattr(error,stream) or b'';(out/(name+'.'+stream)).write_bytes(data.encode() if isinstance(data,str) else data)
 (out/(name+'.json')).write_text(json.dumps(rec,indent=2))
 if number%6==0 or 'error' in rec:print(f'{number} runs; failures={len(failures)}; last={name}',flush=True)

cases=list(itertools.product(('hierarchical',),policies,threads,widths))
for repeat in range(1,a.rounds+1):
 order=cases[:];random.Random(6100+repeat).shuffle(order)
 for case in order:
  for backend in (('generic','compact') if repeat%2 else ('compact','generic')):run(case,repeat,backend)
if a.sensitivity:
 selected=list(itertools.product(('hierarchical',),policies,(8,32,64),(256,)))
 for repeat in range(1,4):
  order=selected[:];random.Random(881+repeat).shuffle(order)
  for case in order:
   for variant in ((0,0),(1,0),(1,1)):
    for backend in ('compact',):run(case,repeat,backend,*variant)
(out/'failures.json').write_text(json.dumps(failures,indent=2))
print(f'Finished {len(rows)} trials; {len(failures)} failures; {out}',flush=True)
raise SystemExit(bool(failures))
