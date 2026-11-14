#!/usr/bin/env python3
"""Paired old/new binaries on the same trace, before the final classic rerun."""
import csv,datetime,hashlib,itertools,json,random,subprocess,time
from pathlib import Path

CONFIG = dict(old_runner='/tmp/rrl-before-scan-reuse',new_runner='build-precision/rrl_workload_runner',
              algorithms=['next-key','gap-lock','prior-key'], policies=['no-wait','wait-die','wound-wait'],
              thetas=[0,1.5], rounds=2, keys=2_097_152, threads=32, transactions=5000,
              ops=10,range=32,seed=8,warmup=1,duration=2,max_attempts=1000)

def main():
    root=Path(__file__).resolve().parents[1]
    out=root/'reports'/('scan-reuse-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
    meta={'config':CONFIG,'binaries':{k:hashlib.sha256((root/CONFIG[k]).read_bytes()).hexdigest() for k in ['old_runner','new_runner']}}
    (out/'metadata.json').write_text(json.dumps(meta,indent=2));rows=[]
    cases=list(itertools.product(CONFIG['algorithms'],CONFIG['policies'],CONFIG['thetas']))
    print('output='+str(out),flush=True)
    for repeat in range(CONFIG['rounds']):
        random.Random(123+repeat).shuffle(cases)
        for cc,policy,z in cases:
            for version in (['old','new'] if repeat%2==0 else ['new','old']):
                name=f'{len(rows)+1:03d}-r{repeat+1}-{cc}-{policy}-z{z:g}-{version}'
                cmd=[str(root/CONFIG[version+'_runner']),'--index','v2','--cc',cc,'--key-lock-table','compact',
                     '--key-latency','0','--key-statistics','0','--workload','ycsb-e','--execution-mode','threads',
                     '--retry-backoff','adaptive','--wait-policy',policy,'--zipf',str(z),'--format','csv']
                for key,arg in [('keys','keys'),('threads','threads'),('transactions','txns'),('ops','ops'),('range','range'),
                                ('seed','seed'),('warmup','warmup'),('duration','duration'),('max_attempts','max-attempts')]:
                    cmd+=['--'+arg,str(CONFIG[key])]
                start=time.monotonic();p=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
                (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
                (out/(name+'.json')).write_text(json.dumps(dict(command=cmd,returncode=p.returncode,wall_seconds=time.monotonic()-start),indent=2))
                assert p.returncode==0,p.stderr
                row=next(csv.DictReader(p.stdout.splitlines()))
                for f in ['lock_errors','index_errors','lock_release_failures','failed_transactions']:assert int(row[f])==0,(f,row[f])
                row.update(theta=z,version=version,repeat=repeat+1);rows.append(row)
                with (out/'results.csv').open('w',newline='') as f:
                    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
                print(f"{len(rows)}/{len(cases)*2*CONFIG['rounds']} {name}: TPS={row['throughput_committed_txn_s']} exhausted={row['retry_exhausted']}",flush=True)
    import statistics
    lines=['# Reusing held classic scan protection','',f"Paired binaries; alternating old/new order; {CONFIG['rounds']} trial(s)/configuration; optional metrics disabled.",
           '', '| Algorithm | Policy | Theta | Old median TPS | New median TPS | Ratio |','|---|---|---:|---:|---:|---:|']
    for cc,policy,z in itertools.product(CONFIG['algorithms'],CONFIG['policies'],CONFIG['thetas']):
        a,b=[statistics.median(float(r['throughput_committed_txn_s']) for r in rows if r['cc']==cc and r['wait_policy']==policy and float(r['theta'])==z and r['version']==v) for v in ['old','new']]
        lines.append(f'| {cc} | {policy} | {z:g} | {a:,.1f} | {b:,.1f} | {b/a:.3f}x |')
    (out/'summary.md').write_text('\n'.join(lines)+'\n')
    print('COMPLETE '+str(out),flush=True)

if __name__=='__main__':main()
