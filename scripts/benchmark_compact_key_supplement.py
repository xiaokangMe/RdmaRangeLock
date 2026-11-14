#!/usr/bin/env python3
"""Cold membership-changing traces and optional metric-overhead comparison."""
import argparse,csv,json,random,subprocess
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--runner',default='build-record/rrl_workload_runner')
parser.add_argument('--output',required=True);args=parser.parse_args()
out=Path(args.output);out.mkdir(parents=True,exist_ok=False);rows=[];number=0
def run(scenario,cc,policy,backend,repeat,extra):
    global number
    number+=1;name=f'{number:03d}-{scenario}-{cc}-{policy}-{backend}-r{repeat}'
    command=[str(Path(args.runner).resolve()),'--index','v2','--cc',cc,'--key-lock-table',backend,
        '--workload','ycsb-e','--keys','100000','--txns','5000','--ops','10','--range','32',
        '--seed','7','--threads','32','--wait-policy',policy,'--execution-mode','threads',
        '--max-attempts','100','--wait-timeout-ms','100','--format','csv',*extra]
    (out/(name+'.json')).write_text(json.dumps(command,indent=2))
    p=subprocess.run(command,capture_output=True,text=True,timeout=90)
    (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
    assert p.returncode==0,(command,p.stderr)
    parsed=list(csv.DictReader(p.stdout.splitlines()));assert len(parsed)==1
    row=parsed[0];row.update(run=name,round=str(repeat),scenario=scenario)
    assert all(int(row[f])==0 for f in ('index_errors','lock_errors','lock_release_failures','failed_transactions'))
    rows.append(row)
    with (out/'results.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    if number%6==0:print(f'{number} supplementary cases complete',flush=True)
for repeat in range(1,4):
    cases=[(cc,policy) for cc in ('next-key','gap-lock','prior-key') for policy in ('no-wait','wait-die','wound-wait')]
    random.Random(83+repeat).shuffle(cases)
    for cc,policy in cases:
        for backend in (('generic','compact') if repeat%2 else ('compact','generic')):
            run('cold-uniform',cc,policy,backend,repeat,['--zipf','0'])
    for cc in ('next-key','gap-lock','prior-key'):
        for mode in (('full','fast') if repeat%2 else ('fast','full')):
            run('metrics-'+mode,cc,'no-wait','compact',repeat,
                ['--zipf','0.99','--warmup','1','--duration','3',
                 '--key-latency',str(int(mode=='full')),'--key-statistics',str(int(mode=='full'))])
print(f'Finished {number} supplementary cases',flush=True)
