import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

runner=sys.argv[1]
def run(*args,ok=True):
    options={'--index':'v2','--cc':'record-2pl','--keys':'128','--txns':'128',
             '--ops':'4','--threads':'2','--format':'csv'}
    options.update(zip(map(str,args[::2]),map(str,args[1::2])))
    result=subprocess.run([runner,*[value for item in options.items() for value in item]],
        text=True,capture_output=True,timeout=20)
    if not ok:
        assert result.returncode!=0,result.stdout
        return
    assert result.returncode==0,result.stderr
    lines=list(csv.reader(result.stdout.splitlines()))
    assert len(lines)==2 and len(lines[0])==len(lines[1]),result.stdout
    row=dict(zip(*lines))
    for name in ['lock_errors','index_errors','lock_release_failures','failed_transactions']:
        assert row[name]=='0',(name,row)
    assert int(row['committed'])>0
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    return row

for policy in ['no-wait','wait-die','wound-wait']:
    for mode in ['threads','coroutines']:
        row=run('--wait-policy',policy,'--execution-mode',mode,
            '--coroutines',2 if mode=='coroutines' else 1)
        assert row['record_lock_table']=='compact'
        assert row['coordination']=='compact-record-state'
        assert row['record_lock_bytes']=='1024'
        assert int(row['successful_attempt_count'])==int(row['committed'])
        assert int(row['active_contexts'])==(4 if mode=='coroutines' else 2)
        assert int(row['coroutine_switches_total'])>0 if mode=='coroutines' else row['coroutine_switches_total']=='0'

for policy in ['no-wait','wait-die','wound-wait']:
    row=run('--wait-policy',policy,'--execution-mode','threads','--record-latency',0)
    assert row['successful_attempt_count']=='0' and row['release_count']=='0'

for workload in ['ycsb-a','ycsb-b','ycsb-e','tpcc-payment','tpcc-new-order']:
    args=['--workload',workload,'--execution-mode','threads','--wait-policy','no-wait','--threads','1']
    baseline=run(*args,'--record-lock-table','generic')
    compact=run(*args,'--record-lock-table','compact')
    for name in ['committed','checksum','rows_found','reads','updates','inserts','range_reads','rows_scanned','rows_rechecked']:
        assert compact[name]==baseline[name],(workload,name,baseline[name],compact[name])

row=run('--execution-mode','threads','--record-latency',0)
assert row['record_latency']=='0' and row['successful_attempt_count']=='0' and row['release_count']=='0'
run('--record-lock-table','generic','--record-latency',0,ok=False)
row=run('--record-lock-table','compact','--threads',64,'--execution-mode','threads')
assert row['record_lock_table']=='compact' and int(row['active_contexts'])==64
row=run('--threads',64,'--execution-mode','threads')
assert row['record_lock_table']=='compact'
row=run('--execution-mode','threads','--wait-policy','no-wait','--warmup',1)
assert int(row['warmup_committed'])>0 and row['logical_started']=='128'
row=run('--execution-mode','threads','--wait-policy','no-wait','--duration',1,'--record-latency',0)
assert int(row['elapsed_ns'])>=1000000000
row=run('--execution-mode','threads','--wait-policy','no-wait','--record-latency',0,'--record-statistics',0)
assert row['record_statistics']=='0' and row['lock_acquires']=='0' and row['successful_attempt_count']=='0'
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'histogram.json'
    row=run('--execution-mode','threads','--latency-histogram',path)
    data=json.loads(path.read_text())
    assert data['histograms']['successful_attempt']['count']==int(row['committed'])
print('Compact record runner: schema, policies, coroutines, serial equivalence, warmup, profile and fallback OK')
