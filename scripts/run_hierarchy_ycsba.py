#!/usr/bin/env python3
"""Run only RDMA hierarchy/YCSB-A on .67, controlled by .66; no full matrix."""
import argparse
import csv
import datetime
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import select
import shlex
import socket
import subprocess
import traceback
from run_verified_matrices import validate

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='experiments/configs/rdma_hierarchy_ycsba_20261103.json')
    args = parser.parse_args()
    os.chdir(ROOT)
    if subprocess.check_output(['git','status','--porcelain'],text=True).strip():
        raise RuntimeError('commit source/config before measurement')
    commit = subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
    cfgpath = ROOT / args.config
    c = json.loads(cfgpath.read_text(encoding='utf-8-sig'))
    assert c['cc'] == 'rdma-hierarchical' and c['workload'] == 'ycsb-a'
    assert c['duration_seconds'] >= 10 and len(c['seeds']) >= 3
    # Root + at most ops buckets + at most ops records per active transaction.
    # Even if all locks land in one shard, there is no capacity overflow.
    bound = max(c['threads']) * (1 + 2*c['operations'])
    assert bound == c['conservative_all_locks_bound'] < c['capacity_slots_per_shard']
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    logs = ROOT/'experiments/logs'/('hierarchy-ycsba-'+stamp)
    logs.mkdir()
    resultpath = ROOT/'experiments/results'/('hierarchy-ycsba-'+stamp+'.csv')
    manifest = resultpath.with_suffix('.json')
    state = dict(source_commit=commit, config=c, config_sha256=hashlib.sha256(cfgpath.read_bytes()).hexdigest(),
                 log_dir=str(logs), result_csv=str(resultpath), started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 completed_cases=0, expected_cases=c['expected_cases'], pid=os.getpid())
    def publish(status):
        state['status']=status
        state['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        manifest.write_text(json.dumps(state,indent=2))
        Path('/tmp/rrl-hierarchy-ycsba-state.json').write_text(json.dumps(state,indent=2))
        print(status, state['completed_cases'], '/', c['expected_cases'], flush=True)
    def checked(argv, **kw):
        return subprocess.run(argv,check=True,**kw)
    env={k:v for k,v in os.environ.items() if not k.startswith('RRL_')}
    env.update(RRL_RDMA_GID_INDEX=str(c['gid_index']),RRL_RDMA_PROFILE='0',RRL_SLOT_LOOKUP=c['slot_lookup'],
               RRL_LOCAL_GATE=str(c['local_gate']),RRL_GATE_RETRY=str(c['gate_retry']))
    state['environment']={k:v for k,v in env.items() if k.startswith('RRL_')}
    ssh=['ssh','-o','BatchMode=yes','-o','IdentitiesOnly=yes','-i',str(Path.home()/'.ssh/rdmarangelock_deploy'),'dell@10.102.35.67']
    scp=['scp','-o','BatchMode=yes','-o','IdentitiesOnly=yes','-i',str(Path.home()/'.ssh/rdmarangelock_deploy')]
    build=ROOT/'build-hierarchy-fix'
    server=None
    try:
        publish('building')
        for name,cmd in [('local-hardware',['bash','-c','uname -a; lscpu; ibv_devinfo']),
                         ('remote-hardware',ssh+['uname -a; lscpu; ibv_devinfo'])]:
            with (logs/(name+'.txt')).open('w') as handle: checked(cmd,stdout=handle,stderr=handle)
        with (logs/'build.log').open('w') as handle:
            checked(['cmake','-S','.', '-B',str(build),'-DCMAKE_BUILD_TYPE=Release'],stdout=handle,stderr=handle)
            checked(['cmake','--build',str(build),'-j4'],stdout=handle,stderr=handle)
        remote=subprocess.check_output(ssh+['mktemp -d /tmp/rrl-hierarchy-ycsba.XXXXXX'],text=True).strip()
        assert remote.startswith('/tmp/rrl-hierarchy-ycsba.')
        files=[build/'rrl_workload_runner',build/'rrl_rdma_hierarchy_test',cfgpath]
        checked(scp+[str(f) for f in files]+['dell@10.102.35.67:'+remote+'/'])
        expected={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
        actual={line.split()[1].split('/')[-1]:line.split()[0] for line in subprocess.check_output(ssh+['sha256sum '+shlex.quote(remote)+'/*'],text=True).splitlines()}
        assert actual==expected,'deployment hash mismatch'
        state.update(remote_root=remote,artifact_sha256=expected)
        with socket.socket() as sock:
            sock.bind(('0.0.0.0',0)); port=sock.getsockname()[1]
        server=subprocess.Popen([str(build/'rrl_rdma_lock_server'),str(port)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
        if not select.select([server.stdout],[],[],15)[0]: raise RuntimeError('server readiness timeout')
        first=server.stdout.readline()
        if 'listening' not in first: raise RuntimeError('server failed: '+first)
        (logs/'server-start.log').write_text(first)
        state['port']=port
        publish('correctness')
        testenv=env.copy();testenv.update(RRL_TEST_RDMA_HOST='127.0.0.1',RRL_TEST_RDMA_PORT=str(port))
        tests=checked(['ctest','--test-dir',str(build),'--output-on-failure'],env=testenv,capture_output=True,text=True,timeout=180)
        (logs/'ctest.log').write_text(tests.stdout+tests.stderr)
        assert 'Skipped' not in tests.stdout and '100% tests passed' in tests.stdout
        r=checked(ssh+[shlex.join(['env','RRL_RDMA_GID_INDEX='+str(c['gid_index']),
                  'RRL_TEST_RDMA_HOST='+c['server_roce'],'RRL_TEST_RDMA_PORT='+str(port),remote+'/rrl_rdma_hierarchy_test'])],capture_output=True,text=True,timeout=90)
        (logs/'cross-contract.log').write_text(r.stdout+r.stderr)
        state['correctness']='27/27 no skips; cross-host hierarchy contract passed'
        # Three worst-concurrency/bucket checks are also their first valid repeats.
        points=list(itertools.product(c['buckets'],c['threads'],c['seeds']))
        firstpoints=[(b,max(c['threads']),c['seeds'][0]) for b in c['buckets']]
        points=firstpoints+[p for p in points if p not in firstpoints]
        assert len(points)==c['expected_cases'] and len(set(points))==len(points)
        writer=None
        with resultpath.open('x',newline='') as output:
            for b,t,seed in points:
                assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==commit
                assert server.poll() is None,'server stopped unexpectedly'
                state['current_case']=dict(bucket=b,threads=t,seed=seed)
                publish('capacity-preflight' if state['completed_cases']<len(firstpoints) else 'measuring')
                argv=[remote+'/rrl_workload_runner','--index',c['index'],'--cc',c['cc'],'--workload',c['workload'],
                      '--keys',str(c['keys']),'--txns',str(c['transactions']),'--ops',str(c['operations']),
                      '--range',str(c['range']),'--threads',str(t),'--seed',str(seed),'--zipf',str(c['zipf']),
                      '--items',str(c['tpcc_items']),'--duration',str(c['duration_seconds']),'--bucket',str(b),
                      '--hierarchy-diagnostics','0','--rdma-host',c['server_roce'],'--rdma-port',str(port),'--format','csv']
                remoteenv=['env']+[k+'='+v for k,v in state['environment'].items()]
                run=checked(ssh+[shlex.join(['timeout','120']+remoteenv+argv)],capture_output=True,text=True,timeout=140)
                case='b%d-t%d-s%d'%(b,t,seed)
                (logs/(case+'.stdout')).write_text(run.stdout)
                (logs/(case+'.stderr')).write_text(run.stderr)
                reader=csv.DictReader(io.StringIO(run.stdout)); rows=list(reader)
                assert len(reader.fieldnames)==len(set(reader.fieldnames)) and all(None not in r and None not in r.values() for r in rows)
                validate(rows,c['duration_seconds'],1)
                row=rows[0]
                assert row['cc']==c['cc'] and row['workload']==c['workload'] and int(row['threads'])==t
                assert int(row['hierarchy_bucket_width'])==b and int(row['rdma_connections'])==min(t,2)
                assert not any(int(row[k]) for k in ['lock_capacity_aborts','lock_gate_aborts','rdma_gate_transport_errors'])
                assert int(row['committed'])>0,'zero commit regression'
                record=dict(source_commit=commit,transport='rdma-cross-host',server_host=c['server_roce'],client_host=c['client_roce'],
                            run=c['seeds'].index(seed)+1,seed=seed,range=c['range'],bucket=b,zipf=c['zipf'],**row)
                if writer is None:
                    writer=csv.DictWriter(output,fieldnames=list(record));writer.writeheader()
                writer.writerow(record);output.flush()
                state['completed_cases']+=1
                state['last_committed']=int(row['committed'])
                publish('case-complete')
        assert state['completed_cases']==c['expected_cases']
        state['csv_sha256']=hashlib.sha256(resultpath.read_bytes()).hexdigest()
        state['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        publish('complete')
    except BaseException as error:
        state['error']=repr(error)
        (logs/'failure.txt').write_text(traceback.format_exc())
        publish('failed')
        raise
    finally:
        if server:
            server.terminate()
            try:server.wait(timeout=15)
            except subprocess.TimeoutExpired:server.kill();server.wait()
            (logs/'server-rest.log').write_text(server.stdout.read())
        state['own_server_stopped']=True
        manifest.write_text(json.dumps(state,indent=2))
        Path('/tmp/rrl-hierarchy-ycsba-state.json').write_text(json.dumps(state,indent=2))

if __name__=='__main__': main()
