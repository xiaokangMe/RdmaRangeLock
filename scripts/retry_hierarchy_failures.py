#!/usr/bin/env python3
import csv,json,subprocess,sys,time
from pathlib import Path
root=Path(sys.argv[1]);failures=json.loads((root/'failures.json').read_text())
valid=list(csv.DictReader((root/'results.csv').open()))
remaining=[];recovered=[]
for failure in failures:
 original=failure['name']
 for attempt in range(1,4):
  name=f'{original}-retry{attempt}';command=failure['command'];started=time.monotonic()
  try:
   p=subprocess.run(command,capture_output=True,text=True,timeout=90)
   (root/(name+'.stdout')).write_text(p.stdout);(root/(name+'.stderr')).write_text(p.stderr)
   rec={'name':name,'command':command,'retry_of':original,'retry':attempt,'returncode':p.returncode,
        'wall_seconds':time.monotonic()-started}
   if p.returncode==0:
    rows=list(csv.DictReader(p.stdout.splitlines()));assert len(rows)==1
    row=rows[0]
    for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):assert int(row[field])==0
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    row.update(run=name,round=failure['round'],root_variant=str(failure['implicit_root']),
               padding_variant=str(failure['padded']),metrics_mode='full')
    valid.append(row);recovered.append(rec)
    with (root/'results.csv').open('w',newline='') as f:
     w=csv.DictWriter(f,fieldnames=list(valid[0]));w.writeheader();w.writerows(valid)
    (root/(name+'.json')).write_text(json.dumps(rec,indent=2));break
   rec['error']=p.stderr or f'exit {p.returncode}';(root/(name+'.json')).write_text(json.dumps(rec,indent=2))
  except Exception as e:
   rec={'name':name,'command':command,'retry_of':original,'retry':attempt,'error':str(e)}
   (root/(name+'.json')).write_text(json.dumps(rec,indent=2))
 else:remaining.append(failure)
(root/'retries.json').write_text(json.dumps({'recovered':recovered,'unrecovered':remaining},indent=2))
(root/'failures.json').write_text(json.dumps(remaining,indent=2))
print(f'recovered {len(recovered)} of {len(failures)}; unrecovered {len(remaining)}',flush=True)
raise SystemExit(bool(remaining))
