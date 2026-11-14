import csv,json,subprocess,sys,tempfile
from pathlib import Path
runner=sys.argv[1]
def run(backend,cc,policy='no-wait',threads=1,extra=(),ok=True):
    cmd=[runner,'--index','v2','--cc',cc,'--key-lock-table',backend,
        '--workload','ycsb-e','--keys','128','--txns','128','--ops','10','--range','16',
        '--threads',str(threads),'--seed','7','--wait-policy',policy,
        '--execution-mode','threads','--max-attempts','1000','--format','csv']
    options=dict(zip(cmd[1::2],cmd[2::2]));options.update(zip(extra[::2],extra[1::2]))
    cmd=[runner,*[v for pair in options.items() for v in pair]]
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
    if not ok:
        assert p.returncode!=0,(cmd,p.stdout);return
    assert p.returncode==0,(cmd,p.stderr)
    lines=list(csv.reader(p.stdout.splitlines()));assert len(lines)==2 and len(lines[0])==len(lines[1])
    row=dict(zip(*lines))
    assert row['key_lock_table']==backend if backend!='auto' else row['key_lock_table'] in ('compact','generic')
    for name in ('index_errors','lock_errors','lock_release_failures','failed_transactions'):
        assert row[name]=='0',(cmd,name,row[name])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    return row
for cc in ('next-key','gap-lock','prior-key'):
    for policy in ('no-wait','wait-die','wound-wait'):
        generic=run('generic',cc,policy);compact=run('compact',cc,policy)
        for name in ('committed','checksum','rows_found','inserts','range_reads'):
            assert generic[name]==compact[name],(cc,policy,name,generic[name],compact[name])
        for threads in (4,64):run('compact',cc,policy,threads)
        run('compact',cc,policy,2,('--execution-mode','coroutines','--coroutines','2'))
    run('compact',cc,threads=65,ok=False)
    assert run('auto',cc,threads=65,extra=('--max-attempts','1','--keys','65536','--range','1'))['key_lock_table']=='generic'
    row=run('compact',cc,threads=2,extra=('--warmup','1'))
    assert int(row['warmup_committed'])>0 and int(row['logical_started'])==128
    row=run('compact',cc,threads=2,extra=('--duration','1'))
    assert int(row['elapsed_ns'])>=1000000000
    run('compact',cc,extra=('--lock-budget','1'))
    row=run('compact',cc,extra=('--key-latency','0','--key-statistics','0'))
    assert int(row['committed'])==128 and int(row['successful_attempt_count'])==0 and int(row['rows_found'])==0
run('compact','record-2pl',ok=False)
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'histogram.json';row=run('compact','next-key',extra=('--latency-histogram',str(path)))
    assert json.loads(path.read_text())['histograms']['logical_completion']['count']==int(row['committed'])
print('YCSB-E: generic equivalence; three protocols/policies; 64 threads; coroutines; warmup; duration; cleanup; limits OK')
