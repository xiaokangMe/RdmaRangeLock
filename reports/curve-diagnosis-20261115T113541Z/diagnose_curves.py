import csv,json,datetime,random,subprocess,time
from pathlib import Path
root=Path('/home/dell/RdmaRangeLock-record2pl-20261112')
out=root/'reports'/('curve-diagnosis-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
out.mkdir();print(out,flush=True)
geometry=subprocess.run(['/tmp/analyze_ycsbe_geometry'],capture_output=True,text=True,check=True)
(out/'geometry.csv').write_text(geometry.stdout)
(out/'geometry_source.cpp').write_bytes(Path('/tmp/analyze_ycsbe_geometry.cpp').read_bytes())
rows=[]
for rep in range(2):
 for z in ([0,.1] if rep==0 else [.1,0]):
  name=f'record-r{rep+1}-z{z:g}'
  cmd=[str(root/'build-precision/rrl_workload_runner'),'--index','v2','--cc','record-2pl','--record-lock-table','compact',
       '--record-latency','0','--record-statistics','0','--workload','ycsb-e','--keys','2097152','--txns','5000','--ops','10',
       '--range','32','--threads','32','--seed','8','--zipf',str(z),'--wait-policy','no-wait','--execution-mode','threads',
       '--max-attempts','1000','--warmup','10','--duration','10','--retry-backoff','adaptive','--format','csv']
  p=subprocess.run(cmd,capture_output=True,text=True,timeout=180)
  (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
  (out/(name+'.json')).write_text(json.dumps(dict(command=cmd,returncode=p.returncode),indent=2))
  assert p.returncode==0,p.stderr
  r=next(csv.DictReader(p.stdout.splitlines()));r.update(repeat=rep+1,theta=z);rows.append(r)
  print(name,r['throughput_committed_txn_s'],'exhausted',r['retry_exhausted'],flush=True)
with (out/'record_repeats.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print('COMPLETE '+str(out),flush=True)

