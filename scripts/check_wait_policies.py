#!/usr/bin/env python3
"""Correctness and bounded smoke checks; never launches a formal matrix."""
import argparse
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import select
import shlex
import socket
import subprocess


def validate(text):
    reader = csv.DictReader(io.StringIO(text))
    rows = list(reader)
    assert len(rows) == 1 and len(set(reader.fieldnames)) == len(reader.fieldnames)
    r = rows[0]
    assert None not in r and None not in r.values()
    for f in ('lock_errors', 'index_errors', 'lock_release_failures', 'failed_transactions',
              'rdma_timeouts', 'rdma_completion_errors', 'rdma_local_errors', 'rdma_count_errors',
              'lock_capacity_aborts', 'lock_gate_aborts', 'rdma_gate_transport_errors'):
        assert int(r[f]) == 0, (f, r[f])
    n = lambda f: int(r[f])
    assert n('metrics_schema') == 2 and n('committed') > 0
    assert n('transactions') == n('committed') + n('aborted') + n('failed_transactions')
    assert n('logical_started') == n('committed') + n('logical_unfinished') + n('logical_failed')
    assert n('transactions') == n('logical_started') + n('retries')
    assert n('retries') == n('completed_retries') + n('unfinished_retries')
    for name in ('successful_attempt', 'logical_completion'):
        assert n(name + '_count') == n('committed')
    for name in ('successful_attempt', 'logical_completion', 'lock_wait', 'lock_call', 'release'):
        values = [n(name + '_' + q + '_ns') for q in ('p50', 'p95', 'p99', 'p999', 'max')]
        assert values == sorted(values), (name, values)
    if r['wait_policy'] == 'no-wait':
        assert n('lock_wait_count') == 0 and n('lock_wait_total_ns') == 0
    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build-wait-policy')
    parser.add_argument('--cross-host', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = (root / args.build).resolve()
    logs = root / 'experiments/logs' / ('wait-policy-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    logs.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RRL_')}
    env['RRL_RDMA_GID_INDEX'] = '3'
    def port():
        with socket.socket() as sock:
            sock.bind(('0.0.0.0', 0))
            return sock.getsockname()[1]
    def run(argv, name, **kw):
        (logs / (name + '.command.json')).write_text(json.dumps(argv))
        r = subprocess.run(argv, capture_output=True, text=True, timeout=180, **kw)
        (logs / (name + '.stdout')).write_text(r.stdout)
        (logs / (name + '.stderr')).write_text(r.stderr)
        if r.returncode:
            raise RuntimeError((name, r.returncode, r.stderr, r.stdout[-1500:]))
        return r
    processes = []
    cases = []
    info = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            'kind': 'correctness-and-directed-smoke-not-formal', 'log_dir': str(logs)}
    try:
        rdma_port, control_port, data_port = port(), port(), port()
        for binary, listen_port in [('rrl_rdma_lock_server', rdma_port), ('rrl_wait_coordinator', control_port), ('rrl_rdma_data_server',data_port)]:
            argv = [str(build / binary), str(listen_port)]
            if binary == "rrl_rdma_data_server": argv = [str(build / binary), "--config", str(root / "experiments/configs/rdma_shared_smoke.json"), "--port", str(listen_port)]
            proc = subprocess.Popen(argv, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            processes.append(proc)
            if not select.select([proc.stdout], [], [], 15)[0]:
                raise RuntimeError(binary + ' readiness timeout')
            line = proc.stdout.readline()
            (logs / (binary + '.start')).write_text(line)
            if 'listening' not in line:
                raise RuntimeError(binary + ': ' + line)
        testenv = dict(env, RRL_TEST_RDMA_HOST='127.0.0.1', RRL_TEST_RDMA_PORT=str(rdma_port),
                       RRL_TEST_COORDINATOR_PORT=str(control_port), RRL_TEST_DATA_PORT=str(data_port))
        r = run(['ctest', '--test-dir', str(build), '--output-on-failure'], 'ctest', env=testenv)
        assert 'Skipped' not in r.stdout and '100% tests passed' in r.stdout
        info['ctest'] = 'all passed, no skips'
        for policy in ('no-wait', 'wait-die', 'wound-wait'):
            for cc in ('record-2pl', 'gap-lock', 'next-key', 'hierarchical', 'predicate', 'exact-range'):
                for workload in ('ycsb-a', 'ycsb-e'):
                    name = '-'.join(('local', cc, workload, policy))
                    cmd = [str(build / 'rrl_workload_runner'), '--cc', cc, '--workload', workload,
                           '--keys', '128', '--txns', '100', '--ops', '4', '--threads', '4', '--range', '8',
                           '--bucket', '64', '--wait-policy', policy, '--format', 'csv']
                    row = validate(run(cmd, name, env=env).stdout)
                    cases.append({'name': name, 'committed': row['committed'], 'retries': row['retries']})
        if args.cross_host:
            ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-i',
                   str(Path.home() / '.ssh/rdmarangelock_deploy'), 'dell@10.102.35.67']
            remote = subprocess.check_output(ssh + ['mktemp -d /tmp/rrl-wait-verify.XXXXXX'], text=True).strip()
            assert remote.startswith('/tmp/rrl-wait-verify.')
            files = [build / 'rrl_workload_runner', build / 'rrl_rdma_wait_policy_test']
            scp = ['scp'] + ssh[1:-1]
            subprocess.run(scp + [str(f) for f in files] + ['dell@10.102.35.67:' + remote + '/'], check=True)
            hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
            remote_hashes = {line.split()[1].split('/')[-1]: line.split()[0] for line in
                             subprocess.check_output(ssh + ['sha256sum ' + remote + '/*'], text=True).splitlines()}
            assert hashes == remote_hashes
            info.update(remote_dir=remote, binary_sha256=hashes)
            run(ssh + [shlex.join(['env', 'RRL_RDMA_GID_INDEX=3', 'RRL_TEST_RDMA_HOST=10.0.0.3',
                'RRL_TEST_RDMA_PORT=' + str(rdma_port), 'RRL_TEST_COORDINATOR_PORT=' + str(control_port),
                remote + '/rrl_rdma_wait_policy_test'])], 'cross-contract')
            for policy in ('no-wait', 'wait-die', 'wound-wait'):
                for cc in ('rdma-next-key', 'rdma-hierarchical', 'rdma-predicate', 'rdma-exact-range'):
                    for workload in ('ycsb-a', 'ycsb-e'):
                        name = '-'.join(('cross', cc, workload, policy))
                        cmd = [remote + '/rrl_workload_runner', '--rdma-lock-only', '1', '--cc', cc, '--workload', workload,
                               '--keys', '128', '--txns', '200', '--ops', '4', '--threads', '4', '--range', '8',
                               '--bucket', '64', '--wait-policy', policy, '--format', 'csv', '--duration', '1',
                               '--rdma-host', '10.0.0.3', '--rdma-port', str(rdma_port),
                               '--coordinator-port', str(control_port)]
                        row = validate(run(ssh + [shlex.join(['env', 'RRL_RDMA_GID_INDEX=3'] + cmd)], name).stdout)
                        cases.append({'name': name, 'committed': row['committed'], 'retries': row['retries']})
        info.update(status='PASS', cases=cases)
        (logs / 'validated.json').write_text(json.dumps(info, indent=2))
        print(json.dumps(info, indent=2), flush=True)
    except BaseException as e:
        info.update(status='FAIL', error=repr(e), cases=cases)
        (logs / 'validated.json').write_text(json.dumps(info, indent=2))
        print('retained logs:', logs, flush=True)
        raise
    finally:
        for proc in reversed(processes):
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        print('own verification servers stopped; logs:', logs, flush=True)


if __name__ == '__main__':
    main()
