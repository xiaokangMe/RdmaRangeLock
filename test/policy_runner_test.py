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
              ['--latency', '2'], ['--cc', 'none', '--wait-policy', 'wait-die'],
              ['--cc', 'rdma-hierarchical', '--wait-policy', 'wait-die']):
    command = base[:]
    if '--cc' in flags:
        i = command.index('--cc')
        del command[i:i+2]
    run = subprocess.run(command + flags, capture_output=True, text=True, timeout=10)
    assert run.returncode != 0, flags
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    for policy in ('no-wait', 'wait-die', 'wound-wait'):
        cfg = path / 'config.json'
        hist = path / 'histogram.json'
        cfg.write_text(json.dumps({'wait_policy': policy, 'max_attempts': 1000,
                                  'wait_timeout_ms': 1000, 'latency_histogram': str(hist)}))
        run = subprocess.run(base + ['--config', str(cfg)], capture_output=True, text=True, timeout=15, check=True)
        assert not run.stderr, run.stderr
        row = validate(run.stdout)
        assert row['wait_policy'] == policy
        data = json.loads(hist.read_text())
        assert data['schema'] == 1 and data['wait_policy'] == policy
        for name, h in data['histograms'].items():
            assert h['count'] == sum(b['count'] for b in h['buckets']) == int(row[name + '_count'])
            assert h['max_ns'] == int(row[name + '_max_ns'])
            assert h['total_ns'] == int(row[name + '_total_ns'])
        assert int(row['logical_started']) == 40
print('Policy JSON/CLI validation, histogram export and count conservation: OK')
