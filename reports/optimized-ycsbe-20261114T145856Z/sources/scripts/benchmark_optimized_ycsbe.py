#!/usr/bin/env python3
"""Single-machine final sweep. Edit CONFIG; one measured trial per case."""
import csv, datetime, hashlib, itertools, json, os, random, subprocess, time
from pathlib import Path

CONFIG = {
    'runner': 'build-precision/rrl_workload_runner',
    'algorithms': ['record-2pl', 'next-key', 'gap-lock', 'prior-key', 'hierarchical', 'exact-range'],
    'policies': ['no-wait', 'wait-die', 'wound-wait'],
    'thetas': [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5],
    'keys': 2_097_152, 'transactions': 5_000, 'ops': 10, 'range': 32,
    'threads': 32, 'seed': 8, 'warmup': 10, 'duration': 10,
    'max_attempts': 1000, 'wait_timeout_ms': 100, 'hierarchy_bucket': 64,
    'retry_backoff': 'adaptive',
    # Keep core commit/abort/retry/deadline/backoff counters; omit optional
    # per-call histograms, checksums and row counts from the performance run.
    'latency': False, 'statistics': False,
    'timeout': 180, 'shuffle_seed': 20261114,
}


def main():
    root = Path(__file__).resolve().parents[1]; os.chdir(root)
    config = dict(CONFIG)
    runner = (root / config['runner']).resolve()
    out = root / 'reports' / ('optimized-ycsbe-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir(parents=True, exist_ok=False)
    files = ['CMakeLists.txt', 'benchmarks/workload_runner.cpp', 'include/index/btree.h',
             'include/txn/retry_backoff.h', 'include/txn/policy_workload.h',
             'include/lock/record_lock_table.h', 'include/lock/classic_lock_table.h',
             'include/lock/hierarchy_lock_table.h', 'src/txn/record_workload.cpp',
             'src/txn/key_range_plan.h', 'src/txn/compact_hierarchy_plan.h',
             'src/txn/compact_exact_plan.h', 'src/index/v2_btree_latch.cpp',
             'scripts/benchmark_optimized_ycsbe.py']
    meta = {'config': config, 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'affinity': sorted(os.sched_getaffinity(0)), 'trials_per_case': 1,
            'runner_sha256': hashlib.sha256(runner.read_bytes()).hexdigest(),
            'source_sha256': {f: hashlib.sha256(Path(f).read_bytes()).hexdigest() for f in files}}
    for f in files:
        dst = out / 'sources' / f; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(Path(f).read_bytes())
    for name, cmd in [('cpu', ['lscpu']), ('head', ['git', 'rev-parse', 'HEAD']),
                      ('branch', ['git', 'branch', '--show-current']), ('diff', ['git', 'diff'])]:
        p = subprocess.run(cmd, capture_output=True, text=True); (out / (name + '.txt')).write_text(p.stdout + p.stderr)
    (out / 'metadata.json').write_text(json.dumps(meta, indent=2))
    cases = list(itertools.product(config['algorithms'], config['policies'], config['thetas']))
    random.Random(config['shuffle_seed']).shuffle(cases)
    print('output=' + str(out) + ' cases=' + str(len(cases)), flush=True)
    rows = []; failures = []
    for number, (cc, policy, theta) in enumerate(cases, 1):
        name = f'{number:03d}-{cc}-{policy}-z{theta:g}'
        backend = 'record' if cc == 'record-2pl' else 'key' if cc in ['next-key', 'gap-lock', 'prior-key'] else 'hierarchy' if cc == 'hierarchical' else 'exact'
        cmd = [str(runner), '--index', 'v2', '--cc', cc, f'--{backend}-lock-table', 'compact',
               f'--{backend}-latency', str(int(config['latency'])),
               f'--{backend}-statistics', str(int(config['statistics'])),
               '--workload', 'ycsb-e', '--keys', str(config['keys']), '--txns', str(config['transactions']),
               '--ops', str(config['ops']), '--range', str(config['range']), '--threads', str(config['threads']),
               '--zipf', str(theta), '--seed', str(config['seed']), '--wait-policy', policy,
               '--execution-mode', 'threads', '--max-attempts', str(config['max_attempts']),
               '--wait-timeout-ms', str(config['wait_timeout_ms']), '--warmup', str(config['warmup']),
               '--duration', str(config['duration']), '--retry-backoff', config['retry_backoff'], '--format', 'csv']
        if cc == 'hierarchical':
            cmd += ['--bucket', str(config['hierarchy_bucket']), '--hierarchy-implicit-root', '1', '--hierarchy-padded', '0']
        rec = {'command': cmd, 'name': name}; start = time.monotonic()
        try:
            assert hashlib.sha256(runner.read_bytes()).hexdigest() == meta['runner_sha256'], 'runner changed during campaign'
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=config['timeout'])
            rec.update(returncode=p.returncode, wall_seconds=time.monotonic()-start)
            (out / (name + '.stdout')).write_text(p.stdout); (out / (name + '.stderr')).write_text(p.stderr)
            assert p.returncode == 0, p.stderr
            parsed = list(csv.DictReader(p.stdout.splitlines())); assert len(parsed) == 1
            row = parsed[0]
            assert row['data_backend'] == 'local' and row[f'{backend}_lock_table'] == 'compact'
            assert int(row['threads']) == config['threads'] and float(row['trace_zipf_theta']) == theta
            assert row['retry_backoff'] == config['retry_backoff']
            assert int(row[f'{backend}_latency']) == int(config['latency'])
            assert int(row[f'{backend}_statistics']) == int(config['statistics'])
            for f in ['lock_errors', 'index_errors', 'lock_release_failures', 'failed_transactions', 'lock_capacity_aborts']:
                assert int(row[f]) == 0, (f, row[f])
            assert int(row['transactions']) == int(row['committed']) + int(row['aborted'])
            assert int(row['committed']) == int(row['committed_read_only']) + int(row['committed_with_writes'])
            assert int(row['transactions']) == int(row['logical_started']) + int(row['retries'])
            assert int(row['logical_started']) == int(row['committed']) + int(row['logical_unfinished'])
            assert int(row['elapsed_ns']) >= config['duration'] * 1e9
            row.update(run=name, theta=theta); rows.append(row)
            print(f"{number}/{len(cases)} {cc} {policy} z={theta:g}: TPS={row['throughput_committed_txn_s']} exhausted={row['retry_exhausted']} unfinished={row['logical_unfinished']}", flush=True)
        except Exception as e:
            rec['error'] = str(e); failures.append(rec); print(f'FAIL {number}/{len(cases)} {name}: {e}', flush=True)
            if isinstance(e, subprocess.TimeoutExpired):
                for field in ['stdout', 'stderr']:
                    data = getattr(e, field) or ''; data = data.decode(errors='replace') if isinstance(data, bytes) else data
                    (out / (name + '.' + field)).write_text(data)
        (out / (name + '.json')).write_text(json.dumps(rec, indent=2))
        if rows:
            with (out / 'results.csv').open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
        (out / 'failures.json').write_text(json.dumps(failures, indent=2))
    meta.update(finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), passed=len(rows), failures=len(failures))
    (out / 'metadata.json').write_text(json.dumps(meta, indent=2))
    subprocess.run(['python3', 'scripts/plot_optimized_ycsbe.py', str(out)], check=True)
    print('COMPLETE ' + str(out), flush=True)
    return bool(failures)

if __name__ == '__main__':
    raise SystemExit(main())
