"""Bounded common-driver correctness checks, not a performance matrix."""
import csv,io,json,subprocess,sys,tempfile
from pathlib import Path
runner=Path(sys.argv[1]).resolve()
rolex_enabled=len(sys.argv)>2 and sys.argv[2]=='ON'
errors=('lock_errors','index_errors','lock_release_failures','failed_transactions','rdma_timeouts','rdma_completion_errors')
base=dict(workload='ycsb-e',index='v2',cc='exact-range',key_count=128,transaction_count=48,
          operations_per_transaction=4,range_length=8,threads=1,seed=7,zipf=0.4,order=16,
          execution_mode='threads',coroutines=1,wait_policy='no-wait',max_attempts=1000,wait_timeout_ms=5000)
def run(c):
    with tempfile.TemporaryDirectory(prefix='rrl-citron-rolex-') as tmp:
        config=Path(tmp)/'case.json';config.write_text(json.dumps(c))
        p=subprocess.run([str(runner),'--config',str(config),'--format','csv'],capture_output=True,text=True,timeout=40)
        assert p.returncode==0,(c,p.stdout,p.stderr)
        rows=list(csv.DictReader(io.StringIO(p.stdout)));assert len(rows)==1
        row=rows[0];assert None not in row and all(x is not None for x in row.values()),row
        assert all(int(row[k])==0 for k in errors),(c,row)
        assert int(row['committed'])>0 and row['execution_mode']=='threads'
        assert row['coroutines']=='1' and row['coroutine_switches_total']=='0'
        assert int(row['successful_attempt_count'])==int(row['committed'])
        assert int(row['logical_completion_count'])==int(row['committed'])
        return row
fields=('committed','checksum','rows_found','rows_scanned','reads','updates','inserts','range_reads')
for workload in ('ycsb-a','ycsb-e','ycsb-e-skew','tpcc-payment','tpcc-new-order'):
    reference=run(dict(base,workload=workload))
    citron=run(dict(base,workload=workload,cc='citron'))
    assert all(citron[k]==reference[k] for k in fields),(workload,reference,citron)
    assert citron['citron_lock_semantics']=='exclusive-range-S-as-X'
    assert citron['data_backend']=='local' and citron['citron_rdma_operations']=='0'
    if rolex_enabled:
        for cc in ('none','record-2pl','gap-lock','next-key','prior-key','hierarchical','exact-range','precision-lock','griffin-pl','citron'):
            expected=run(dict(base,workload=workload,cc=cc,index='v2'))
            actual=run(dict(base,workload=workload,cc=cc,index='rolex'))
            assert all(actual[k]==expected[k] for k in fields),(workload,cc,actual,expected)
            assert int(actual['rolex_live_keys'])>0 and int(actual['rolex_build_ns'])>0
print('Five workloads: common-driver single-thread checksum/scan equality PASS')
# Exactly the requested deployment geometry. Only one second + warm-up per
# bounded correctness check, never labels these numbers as formal performance.
for index,cc in [('v2','citron')]+([('rolex','next-key')] if rolex_enabled else []):
    row=run(dict(base,index=index,cc=cc,threads=32,transaction_count=96,warmup_seconds=1,duration_seconds=1))
    assert int(row['warmup_committed'])>0 and int(row['warmup_elapsed_ns'])>=1000000000
    assert int(row['elapsed_ns'])>=1000000000
    assert row['active_workers']=='32'
    if cc=='citron':assert int(row['citron_grants'])<=int(row['lock_acquires'])
print('32 threads, no coroutines, drained warm-up and latency schema PASS')
for args in (['--cc','citron','--execution-mode','threads','--wait-policy','wait-die'],
             ['--cc','citron','--execution-mode','threads','--wait-policy','wound-wait'],
             ['--cc','citron','--execution-mode','coroutines'],
             ['--cc','citron','--execution-mode','threads','--citron-tree-height','11'],
             ['--index','rolex','--cc','hierarchical','--hierarchy-layout','leaf'],
             ['--citron-k','0'],['--rolex-hugepages','2']):
    p=subprocess.run([str(runner),*args],capture_output=True,text=True,timeout=10)
    assert p.returncode==2 and not p.stdout,(args,p)
print('Unsupported policy/backend combinations reject before running PASS')
