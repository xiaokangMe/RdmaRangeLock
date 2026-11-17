"""Short correctness smoke under contention; values are not publication results."""
import csv
import io
from pathlib import Path
import subprocess
import sys

runner, output = sys.argv[1:3]
tables = {'record-2pl': 'record', 'next-key': 'key', 'gap-lock': 'key',
          'prior-key': 'key', 'hierarchical': 'hierarchy',
          'exact-range': 'exact', 'precision-lock': 'precision'}
rows = []
for cc, table in tables.items():
    for policy in ('no-wait', 'wait-die', 'wound-wait'):
        cmd = [runner, '--index', 'v2', '--cc', cc, '--workload', 'ycsb-e',
               '--ycsb-insert-behavior', 'deneva', '--keys', '16384', '--txns', '256',
               '--ops', '10', '--range', '32', '--threads', '32', '--zipf', '1.3',
               '--duration', '1', '--wait-policy', policy, '--execution-mode', 'threads',
               '--wait-timeout-ms', '100', f'--{table}-lock-table', 'compact', '--format', 'csv']
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        assert p.returncode == 0, (cmd, p.stderr)
        row = next(csv.DictReader(io.StringIO(p.stdout)))
        for field in ('index_errors', 'lock_errors', 'lock_release_failures', 'failed_transactions'):
            assert int(row[field]) == 0, (cc, policy, field, row[field])
        assert int(row['committed']) > 0 and int(row['index_entries_final']) > 16384, row
        rows.append(row)
        print(cc, policy, 'commits', row['committed'], 'final entries', row['index_entries_final'],
              'capacity aborts', row['lock_capacity_aborts'], flush=True)
with Path(output).open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
print('32-thread skewed Insert smoke: 21 runs passed')
