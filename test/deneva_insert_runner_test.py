import csv
import io
import json
import subprocess
import sys
import tempfile
from pathlib import Path

runner = sys.argv[1]
out = Path(sys.argv[2]) if len(sys.argv) > 2 else None
results = []
tables = {
    'record-2pl': '--record-lock-table', 'next-key': '--key-lock-table',
    'gap-lock': '--key-lock-table', 'prior-key': '--key-lock-table',
    'hierarchical': '--hierarchy-lock-table', 'exact-range': '--exact-lock-table',
    'precision-lock': '--precision-lock-table',
}

def run(cc, policy, backend, extra=()):
    cmd = [runner, '--workload', 'ycsb-e', '--index', 'v2', '--cc', cc,
           '--keys', '128', '--txns', '128', '--ops', '10', '--range', '16',
           '--zipf', '0', '--seed', '7', '--threads', '1',
           '--wait-policy', policy, '--execution-mode', 'threads',
           '--max-attempts', '1000', '--format', 'csv', tables[cc], backend]
    options = dict(zip(cmd[1::2], cmd[2::2]))
    options.update(zip(extra[::2], extra[1::2]))
    cmd = [runner, *[v for pair in options.items() for v in pair]]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
    assert p.returncode == 0, (cmd, p.stderr)
    rows = list(csv.DictReader(io.StringIO(p.stdout)))
    assert len(rows) == 1, p.stdout
    row = rows[0]
    assert None not in row and None not in row.values(), p.stdout
    assert row['ycsb_insert_behavior'] == 'deneva' and row['trace_key_scale'] == '10000', row
    assert row['trace_operations_per_transaction'] == '10', row
    assert int(row['index_entries_final']) > 128, row
    for k in ('lock_errors', 'index_errors', 'lock_release_failures', 'failed_transactions'):
        assert int(row[k]) == 0, (cc, policy, backend, k, row[k])
    results.append(row)
    return row

for cc in tables:
    for policy in ('no-wait', 'wait-die', 'wound-wait'):
        generic = run(cc, policy, 'generic')
        compact = run(cc, policy, 'compact')
        for k in ('committed', 'inserts', 'range_reads', 'rows_found', 'checksum', 'index_entries_final'):
            assert generic[k] == compact[k], (cc, policy, k, generic[k], compact[k])
        assert int(compact['index_entries_final']) == 128 + int(compact['inserts']), compact
        timed = run(cc, policy, 'compact', ('--threads', '4', '--warmup', '1', '--duration', '1'))
        assert int(timed['warmup_committed']) > 0, timed
        assert int(timed['index_entries_final']) > int(compact['index_entries_final']), timed
        assert int(timed['elapsed_ns']) >= 1_000_000_000, timed
        print(cc, policy, 'generic/compact equal; warmup+timed replay grows physical entries', flush=True)

with tempfile.TemporaryDirectory() as d:
    config = Path(d) / 'config.json'
    config.write_text(json.dumps({'ycsb_insert_behavior': 'legacy-upsert'}))
    p = subprocess.run([runner, '--config', str(config), '--workload', 'ycsb-e',
                        '--keys', '128', '--txns', '128', '--format', 'csv'],
                       capture_output=True, text=True, timeout=20)
    assert p.returncode == 0, p.stderr
    row = next(csv.DictReader(io.StringIO(p.stdout)))
    assert row['ycsb_insert_behavior'] == 'legacy-upsert' and row['trace_key_scale'] == '1', row

if out:
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
print('Deneva Insert runner: 63 runs passed')
