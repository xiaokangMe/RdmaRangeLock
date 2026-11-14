"""Retry scheduling must preserve data, counters, limits and coroutine progress."""
import csv,json,subprocess,sys,tempfile
from pathlib import Path
runner=sys.argv[1]
algorithms={'record-2pl':'record','next-key':'key','gap-lock':'key','prior-key':'key',
            'hierarchical':'hierarchy','exact-range':'exact','precision-lock':'precision'}

def run(cc,policy,mode,extra=(),ok=True):
    args={'--index':'v2','--cc':cc,f'--{algorithms[cc]}-lock-table':'compact',
          '--workload':'ycsb-e','--keys':'128','--txns':'96','--ops':'10','--range':'16',
          '--threads':'1','--seed':'8','--zipf':'1.5','--wait-policy':policy,
          '--execution-mode':'threads','--retry-backoff':mode,'--max-attempts':'1000','--format':'csv'}
    args.update(zip(extra[::2],extra[1::2]))
    result=subprocess.run([runner,*[v for pair in args.items() for v in pair]],capture_output=True,text=True,timeout=45)
    if not ok:
        assert result.returncode!=0;return
    assert result.returncode==0,(args,result.stderr)
    lines=list(csv.reader(result.stdout.splitlines()));assert len(lines)==2 and len(lines[0])==len(lines[1])
    row=dict(zip(*lines));assert row['retry_backoff']==mode
    for field in ['lock_errors','index_errors','lock_release_failures','failed_transactions']:assert int(row[field])==0,(args,field,row[field])
    assert int(row['committed'])==int(row['committed_read_only'])+int(row['committed_with_writes'])
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
    assert int(row['max_retries'])<int(args['--max-attempts'])
    if mode=='off':assert int(row['retry_backoff_count'])==0 and int(row['retry_backoff_total_ns'])==0
    return row

for cc in algorithms:
    for policy in ['no-wait','wait-die','wound-wait']:
        off=run(cc,policy,'off');adaptive=run(cc,policy,'adaptive')
        for field in ['committed','checksum','rows_found','inserts','range_reads','committed_with_writes']:
            assert off[field]==adaptive[field],(cc,policy,field)
        for mode in ['off','adaptive']:
            run(cc,policy,mode,('--threads','4'))
    for mode in ['off','adaptive']:
        row=run(cc,'no-wait',mode,('--threads','8','--max-attempts','1'))
        assert int(row['retries'])==0 and int(row['retry_backoff_count'])==0
    run(cc,'no-wait','adaptive',('--threads','2','--execution-mode','coroutines','--coroutines','2'))

for mode in ['off','adaptive']:
    row=run('precision-lock','wound-wait',mode,('--threads','4','--warmup','1','--duration','1'))
    assert int(row['warmup_committed'])>0 and int(row['elapsed_ns'])>=1_000_000_000
run('precision-lock','no-wait','invalid',ok=False)
with tempfile.TemporaryDirectory() as folder:
    cfg=Path(folder)/'config.json';cfg.write_text(json.dumps(dict(index='v2',cc='precision-lock',workload='ycsb-e',
        keys=128,transactions=96,operations=10,range_length=16,threads=4,execution_mode='threads',retry_backoff='adaptive')))
    result=subprocess.run([runner,'--config',str(cfg),'--format','csv'],capture_output=True,text=True,check=True)
    assert next(csv.DictReader(result.stdout.splitlines()))['retry_backoff']=='adaptive'
print('YCSB-E: seven algorithms, policies, retry limits, own data equivalence, coroutine progress and window counters OK')
