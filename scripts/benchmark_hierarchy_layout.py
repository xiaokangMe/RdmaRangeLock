#!/usr/bin/env python3
"""Measure node padding and optional metric overhead on width-256 YCSB-E."""
import argparse,csv,json,random,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--runner',default='build-record/rrl_workload_runner')
p.add_argument('--output',required=True);p.add_argument('--rounds',type=int,default=3)
p.add_argument('--threads',default='32,64');p.add_argument('--width',type=int,default=256)
p.add_argument('--keys',type=int,default=100000);p.add_argument('--txns',type=int,default=5000)
p.add_argument('--duration',type=int,default=2);a=p.parse_args()
out=Path(a.output);out.mkdir(parents=True,exist_ok=False);runner=str(Path(a.runner).resolve())
rows=[];failures=[];number=0
variants=[('dense-full',0,1),('padded-full',1,1),('dense-fast',0,0)]
cases=[(policy,int(t)) for policy in ('no-wait','wait-die','wound-wait') for t in a.threads.split(',')]
for repeat in range(1,a.rounds+1):
 order=cases[:];random.Random(3320+repeat).shuffle(order)
 for policy,threads in order:
  for name,padded,metrics in variants:
   number+=1;run_name=f'{number:03d}-r{repeat}-{policy}-t{threads}-{name}'
   cmd=[runner,'--index','v2','--cc','hierarchical','--hierarchy-lock-table','compact',
    '--workload','ycsb-e','--keys',str(a.keys),'--txns',str(a.txns),'--ops','10','--range','32',
    '--zipf','0.99','--seed','7','--threads',str(threads),'--bucket',str(a.width),
    '--wait-policy',policy,'--execution-mode','threads','--max-attempts','1000',
    '--wait-timeout-ms','100','--warmup','1','--duration',str(a.duration),
    '--hierarchy-implicit-root','1','--hierarchy-padded',str(padded),
    '--hierarchy-latency',str(metrics),'--hierarchy-statistics',str(metrics),'--format','csv']
   rec={'name':run_name,'command':cmd,'variant':name,'round':repeat};(out/(run_name+'.json')).write_text(json.dumps(rec,indent=2))
   try:
    q=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
    (out/(run_name+'.stdout')).write_text(q.stdout);(out/(run_name+'.stderr')).write_text(q.stderr)
    rec['returncode']=q.returncode;assert q.returncode==0,q.stderr
    row=next(csv.DictReader(q.stdout.splitlines()));row.update(run=run_name,round=str(repeat),layout_variant=name);rows.append(row)
    for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):assert int(row[field])==0
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    with (out/'results.csv').open('w',newline='') as f:
     w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
   except Exception as e:rec['error']=str(e);failures.append(rec)
   (out/(run_name+'.json')).write_text(json.dumps(rec,indent=2))
  if number%9==0:print(f'{number} runs; failures={len(failures)}',flush=True)
(out/'failures.json').write_text(json.dumps(failures,indent=2));print(f'{len(rows)} runs; failures={len(failures)}',flush=True)
raise SystemExit(bool(failures))
