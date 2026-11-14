import csv,json,subprocess,sys,tempfile
from pathlib import Path
runner=sys.argv[1]
def run(backend,policy='no-wait',threads=1,extra=(),ok=True):
    options={'--index':'v2','--cc':'hierarchical','--hierarchy-lock-table':backend,
        '--workload':'ycsb-e','--keys':'256','--txns':'128','--ops':'10','--range':'16',
        '--bucket':'16','--threads':str(threads),'--seed':'7','--wait-policy':policy,
        '--execution-mode':'threads','--max-attempts':'100','--wait-timeout-ms':'100','--format':'csv'}
    options.update(zip(extra[::2],extra[1::2]));cmd=[runner,*[v for pair in options.items() for v in pair]]
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
    if not ok:assert p.returncode!=0,(cmd,p.stdout);return
    assert p.returncode==0,(cmd,p.stderr)
    parsed=list(csv.reader(p.stdout.splitlines()));assert len(parsed)==2 and len(parsed[0])==len(parsed[1])
    row=dict(zip(*parsed))
    assert all(row[f]=='0' for f in ('index_errors','lock_errors','lock_release_failures','failed_transactions')),(cmd,row)
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    return row
for policy in ('no-wait','wait-die','wound-wait'):
    baseline=run('generic',policy)
    auto=run('auto',policy)
    assert auto['hierarchy_lock_table']=='compact'
    assert auto['hierarchy_implicit_root']==('0' if policy=='wound-wait' else '1')
    assert auto['hierarchy_padded']==('0' if policy=='wound-wait' else '1')
    for root in ('0','1'):
        for padded in ('0','1'):
            row=run('compact',policy,extra=('--hierarchy-implicit-root',root,'--hierarchy-padded',padded))
            for field in ('committed','checksum','rows_found','range_reads','inserts'):
                assert row[field]==baseline[field],(policy,root,padded,field,row[field],baseline[field])
    for threads in (4,64):run('compact',policy,threads)
    run('compact',policy,2,('--execution-mode','coroutines','--coroutines','2'))
    fast=run('compact',policy,extra=('--hierarchy-latency','0','--hierarchy-statistics','0'))
    assert int(fast['committed'])==128 and fast['checksum']=='0' and fast['successful_attempt_count']=='0'
run('compact',threads=2,extra=('--warmup','1'))
row=run('compact',threads=2,extra=('--duration','1'));assert int(row['elapsed_ns'])>=1000000000
run('compact',extra=('--lock-budget','1'))
run('compact',threads=65,ok=False)
assert run('auto',threads=65,extra=('--keys','65536','--max-attempts','1'))['hierarchy_lock_table']=='generic'
for extra in (('--cc','record-2pl'),('--hierarchy-escalation-threshold','2'),('--hierarchy-deescalate-after','2')):
    run('compact',extra=extra,ok=False)
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'latency.json';run('compact',extra=('--latency-histogram',str(path)))
    assert path.exists()
    run('compact',extra=('--latency-histogram',str(path),'--hierarchy-latency','0'),ok=False)
print('YCSB-E hierarchy: three policies; generic equivalence; 64 contexts; root/padding; accounting; cleanup OK')
