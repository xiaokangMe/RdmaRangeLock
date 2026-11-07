#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from check_wait_policies import validate

runner = sys.argv[1]
base = [runner, '--cc', 'hierarchical', '--keys', '16', '--txns', '40', '--ops', '4',
        '--threads', '2', '--format', 'csv']
for flags in (['--wait-policy', 'typo'], ['--max-attempts', '0'], ['--wait-timeout-ms', '0'],
              ['--wait-timeout-ms', '600001'], ['--coordinator-port', '65536'], ['--lock-budget', '0'],
              ['--execution-mode','typo'], ['--execution-mode','threads','--coroutines','2'],
              ['--latency', '2'], ['--coroutines','0'], ['--coroutines','4097'],
              ['--coroutine-stack-bytes','1'], ['--warmup','600001'], ['--txn-state-port','65536'],
              ['--txn-state-port','12345'], ['--coordinator-port','12345','--txn-state-port','12346'], ['--index', 'rdma'], ['--data-port', '12345'],
              ['--rdma-lock-only', '2'], ['--rdma-lock-only', '1'], ['--cc', 'none', '--wait-policy', 'wait-die'],
              ['--cc', 'rdma-hierarchical', '--wait-policy', 'wait-die']):
    command = base[:]
    if '--cc' in flags:
        i = command.index('--cc')
        del command[i:i+2]
    run = subprocess.run(command + flags, capture_output=True, text=True, timeout=10)
    assert run.returncode != 0, flags
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    for execution in ("threads","coroutines"):
        for policy in ('no-wait', 'wait-die', 'wound-wait'):
            cfg = path / 'config.json'
            hist = path / 'histogram.json'
            cfg.write_text(json.dumps({'execution_mode': execution, 'wait_policy': policy, 'max_attempts': 1000,
                                      'wait_timeout_ms': 1000, 'latency_histogram': str(hist)}))
            run = subprocess.run(base + ['--config', str(cfg)], capture_output=True, text=True, timeout=15, check=True)
            assert not run.stderr, run.stderr
            row = validate(run.stdout)
            assert row['wait_policy'] == policy and row['execution_mode']==execution
            assert (int(row['coroutine_switches_total'])>0)==(execution=="coroutines")
            assert int(row['active_contexts'])==2
            data = json.loads(hist.read_text())
            assert data['schema'] == 1 and data['wait_policy'] == policy
            for name, h in data['histograms'].items():
                assert h['count'] == sum(b['count'] for b in h['buckets']) == int(row[name + '_count'])
                assert h['max_ns'] == int(row[name + '_max_ns'])
                assert h['total_ns'] == int(row[name + '_total_ns'])
            assert int(row['logical_started']) == 40
            override=subprocess.run(base+['--config',str(cfg),'--execution-mode',
                'coroutines' if execution=='threads' else 'threads'],capture_output=True,text=True,timeout=15,check=True)
            other=validate(override.stdout)
            assert other['execution_mode']!=execution
print('Policy JSON/CLI validation, histogram export and count conservation: OK')

# The new protocol must be reachable through CLI and config, not only C++ tests.
for policy in ("no-wait","wait-die","wound-wait"):
    for workload in ("ycsb-a","ycsb-e"):
        cmd=[runner,"--cc","prior-key","--keys","32","--txns","12","--ops","2",
             "--threads","2","--range","4","--workload",workload,"--wait-policy",policy,"--format","csv"]
        row=validate(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=15).stdout)
        assert row["cc"]=="prior-key" and int(row["committed"])>0

for protocol in ("gap-lock","next-key","prior-key"):
    cmd=[runner,"--cc",protocol,"--keys","16","--txns","12","--ops","2",
         "--threads","2","--workload","ycsb-e","--range","4","--format","csv"]
    import csv,io
    row=next(csv.DictReader(io.StringIO(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=15).stdout)))
    assert int(row["transactions"])==12 and int(row["committed"])+int(row["aborted"])==12

for flags in (["--cc","next-key","--hierarchy-escalation-threshold","2"],
              ["--hierarchy-layout","leaf","--hierarchy-deescalate-after","4"],
              ["--hierarchy-escalation-threshold","1048577"]):
    cmd=[runner,"--keys","32","--txns","4","--ops","4","--cc","hierarchical"]
    if "--cc" in flags:cmd=cmd[:-2]
    assert subprocess.run(cmd+flags,capture_output=True,text=True,timeout=10).returncode!=0
