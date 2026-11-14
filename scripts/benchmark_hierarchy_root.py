#!/usr/bin/env python3
"""Measure removing the shared intention-only root word."""
import argparse,csv,json,random,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--runner',default='build-record/rrl_workload_runner')
p.add_argument('--output',required=True);p.add_argument('--rounds',type=int,default=3)
p.add_argument('--threads',default='1,8,32,64');p.add_argument('--width',type=int,default=256)
p.add_argument('--keys',type=int,default=100000);p.add_argument('--txns',type=int,default=5000)
p.add_argument('--duration',type=int,default=2);a=p.parse_args()
out=Path(a.output);out.mkdir(parents=True,exist_ok=False);runner=str(Path(a.runner).resolve())
rows=[];failures=[];number=0
cases=[('no-wait',int(t)) for t in a.threads.split(',')]
cases=[(policy,threads,a.width) for policy in ('no-wait','wait-die','wound-wait') for threads in map(int,a.threads.split(','))]
for repeat in range(1,a.rounds+1):
 order=cases[:];random.Random(2250+repeat).shuffle(order)
 for policy,threads,width in order:
  for root in (0,1):
   number+=1;name=f'{number:03d}-r{repeat}-{policy}-t{threads}-root{root}'
   cmd=[runner,'--index','v2','--cc','hierarchical','--hierarchy-lock-table','compact',
    '--workload','ycsb-e','--keys',str(a.keys),'--txns',str(a.txns),'--ops','10','--range','32',
    '--zipf','0.99','--seed','7','--threads',str(threads),'--bucket',str(width),
    '--wait-policy',policy,'--execution-mode','threads','--max-attempts','1000',
    '--wait-timeout-ms','100','--warmup','1','--duration',str(a.duration),
    '--hierarchy-implicit-root',str(root),'--hierarchy-latency','1','--hierarchy-statistics','1','--format','csv']
   rec={'name':name,'command':cmd,'round':repeat,'implicit_root':root};started=time.monotonic()
   try:
    q=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
    (out/(name+'.stdout')).write_text(q.stdout);(out/(name+'.stderr')).write_text(q.stderr)
    rec.update(returncode=q.returncode,wall_seconds=time.monotonic()-started);assert q.returncode==0,q.stderr
    parsed=list(csv.DictReader(q.stdout.splitlines()));assert len(parsed)==1
    row=parsed[0];row.update(run=name,round=str(repeat),root_variant=str(root));rows.append(row)
    for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):assert int(row[field])==0,(field,row[field])
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    with (out/'results.csv').open('w',newline='') as f:
     w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
   except Exception as error:rec['error']=str(error);failures.append(rec)
   (out/(name+'.json')).write_text(json.dumps(rec,indent=2))
  if number%6==0:print(f'{number} runs; failures={len(failures)}',flush=True)
(out/'failures.json').write_text(json.dumps(failures,indent=2))
print(f'Finished {len(rows)} runs; failures={len(failures)}',flush=True);raise SystemExit(bool(failures))
