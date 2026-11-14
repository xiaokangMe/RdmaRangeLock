"""YCSB-E coverage of compact Precision admission, counters, metrics and fallback."""
import csv,json,subprocess,sys,tempfile
from pathlib import Path
runner=sys.argv[1]
def run(backend,policy='no-wait',threads=1,extra=(),ok=True):
    cmd=[runner,'--index','v2','--cc','precision-lock','--precision-lock-table',backend,
        '--workload','ycsb-e','--keys','128','--txns','128','--ops','10','--range','16',
        '--threads',str(threads),'--seed','7','--wait-policy',policy,
        '--execution-mode','threads','--max-attempts','1000','--format','csv']
    opts=dict(zip(cmd[1::2],cmd[2::2]));opts.update(zip(extra[::2],extra[1::2]))
    cmd=[runner,*[v for pair in opts.items() for v in pair]]
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
    if not ok:
        assert result.returncode!=0,(cmd,result.stdout);return
    assert result.returncode==0,(cmd,result.stderr)
    lines=list(csv.reader(result.stdout.splitlines()));assert len(lines)==2 and len(lines[0])==len(lines[1])
    row=dict(zip(*lines));assert row['precision_lock_table'] in (('compact','generic') if backend=='auto' else (backend,))
    for field in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):
        assert row[field]=='0',(cmd,field,row[field])
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    return row
for policy in ('no-wait','wait-die','wound-wait'):
    generic=run('generic',policy);compact=run('compact',policy)
    for field in ('committed','checksum','rows_found','inserts','range_reads'):
        assert generic[field]==compact[field],(policy,field,generic[field],compact[field])
    assert int(compact['record_lock_bytes'])==0 and int(compact['rows_rechecked'])==0
    for threads in (4,32,63):run('compact',policy,threads)
    run('compact',policy,2,('--execution-mode','coroutines','--coroutines','2'))
run('compact',threads=64,ok=False)
assert run('auto',threads=64,extra=('--max-attempts','1','--keys','65536','--range','1'))['precision_lock_table']=='generic'
run('compact',extra=('--cc','next-key'),ok=False)
run('generic',extra=('--precision-latency','0'),ok=False)
row=run('compact',threads=2,extra=('--warmup','1'));assert int(row['warmup_committed'])>0 and int(row['logical_started'])==128
row=run('compact',threads=2,extra=('--duration','1'));assert int(row['elapsed_ns'])>=1_000_000_000
run('compact',extra=('--lock-budget','1'))
row=run('compact',extra=('--precision-latency','0','--precision-statistics','0'))
assert int(row['committed'])==128 and int(row['successful_attempt_count'])==0 and int(row['rows_found'])==0
with tempfile.TemporaryDirectory() as d:
    histogram=Path(d)/'histogram.json';row=run('compact',extra=('--latency-histogram',str(histogram)))
    assert json.loads(histogram.read_text())['histograms']['logical_completion']['count']==int(row['committed'])
    config=Path(d)/'case.json';config.write_text(json.dumps(dict(index='v2',cc='precision-lock',workload='ycsb-e',keys=128,
        transactions=128,operations=10,range_length=16,threads=4,execution_mode='threads',precision_lock_table='compact',precision_statistics=0,precision_latency=0)))
    result=subprocess.run([runner,'--config',str(config),'--format','csv'],capture_output=True,text=True,check=True)
    assert next(csv.DictReader(result.stdout.splitlines()))['precision_lock_table']=='compact'
print('compact Precision YCSB-E: reference equivalence, three policies, contexts, coroutine, warmup, metrics, budgets and fallback OK')
