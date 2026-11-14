from pathlib import Path
import csv,hashlib,itertools,json,sys
p=Path(sys.argv[1]);meta=json.loads((p/'metadata.json').read_text());cfg=meta['config']
rs=list(csv.DictReader((p/'results.csv').open()))
assert len(rs)==162
expected=set(itertools.product(cfg['algorithms'],cfg['policies'],map(float,cfg['thetas'])))
assert len(expected)==162 and {(r['cc'],r['wait_policy'],float(r['theta'])) for r in rs}==expected
for r in rs:
    d=p.parent/r['source_campaign']
    m=json.loads((d/'metadata.json').read_text())
    assert r['source_runner_sha256']==m['runner_sha256']
    raw=next(csv.DictReader((d/(r['run']+'.stdout')).open()))
    for k,v in raw.items():assert r[k]==v,(r['run'],k)
    command=json.loads((d/(r['run']+'.json')).read_text())['command']
    for arg,value in [('keys',2097152),('threads',32),('ops',10),('range',32),('warmup',10),('duration',10),('max-attempts',1000),('seed',8)]:
        assert command[command.index('--'+arg)+1]==str(value)
    assert command[command.index('--retry-backoff')+1]=='adaptive'
    assert r['data_backend']=='local'
    for f in ['retry_exhausted','lock_errors','index_errors','lock_release_failures','failed_transactions','lock_capacity_aborts']:assert int(r[f])==0
    assert int(r['transactions'])==int(r['committed'])+int(r['aborted'])
    assert int(r['committed'])==int(r['committed_read_only'])+int(r['committed_with_writes'])
    assert int(r['logical_started'])==int(r['committed'])+int(r['logical_unfinished'])
    assert int(r['transactions'])==int(r['logical_started'])+int(r['retries'])
    assert int(r['elapsed_ns'])>=10_000_000_000
    assert int(r['max_retries'])<1000
    assert int(r['worker_min_committed'])>0 and int(r['active_workers'])==32
for selection in meta['selection']:
    directory=p.parent/selection['campaign']
    for name,fingerprint in selection['source_sha256'].items():
        assert hashlib.sha256((directory/'sources'/name).read_bytes()).hexdigest()==fingerprint,(directory,name)
lines=[
'Final audit: PASS',
'162 unique configurations; exactly one selected measurement per configuration.',
'All selected rows match their original stdout; all commands match requested workload/window parameters.',
'Every source snapshot matches its captured SHA256.',
'Execution errors, capacity errors and retry-exhausted logical transactions: 0.',
f"Window-end unfinished logical transactions: {min(int(r['logical_unfinished']) for r in rs)}..{max(int(r['logical_unfinished']) for r in rs)} per trial.",
f"Maximum observed retries for one logical transaction: {max(int(r['max_retries']) for r in rs)}.",
'All 32 contexts submitted committed transactions in every trial.',
'No starvation-freedom guarantee follows from these finite, single-trial measurements.',
]
(p/'audit.txt').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))

