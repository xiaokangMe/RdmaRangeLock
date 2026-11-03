#!/usr/bin/env python3
"""Independent one-hour correctness endurance; no performance comparison."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime
import hashlib
import json
import os
from pathlib import Path
import select
import shlex
import socket
import subprocess
import time
from check_wait_policies import validate


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds',type=int,default=3600)
    p.add_argument('--build',default='build-wait-policy')
    args=p.parse_args()
    if not 1<=args.seconds<=86400:
        p.error('seconds must be 1..86400')
    root=Path(__file__).resolve().parent.parent
    build=(root/args.build).resolve()
    logs=root/'experiments/logs'/('wait-endurance-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    logs.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith('RRL_')}
    env['RRL_RDMA_GID_INDEX']='3'
    ssh=['ssh','-o','BatchMode=yes','-o','IdentitiesOnly=yes','-i',str(Path.home()/'.ssh/rdmarangelock_deploy'),'dell@10.102.35.67']
    scp=['scp']+ssh[1:-1]
    servers=[]
    jobs=[]
    cases=[]
    state={'kind':'correctness-endurance-not-formal','seconds':args.seconds,'status':'STARTING','cases':cases,
           'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
           'source_dirty':bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root)),
           'binary_sha256':hashlib.sha256((build/'rrl_workload_runner').read_bytes()).hexdigest()}
    def save():
        tmp=logs/'state.tmp'
        tmp.write_text(json.dumps(state,indent=2))
        tmp.replace(logs/'state.json')
    def port():
        with socket.socket() as s:
            s.bind(('0.0.0.0',0))
            return s.getsockname()[1]
    def launch(argv,name):
        (logs/(name+'.command.json')).write_text(json.dumps(argv))
        out=open(logs/(name+'.stdout'),'w')
        err=open(logs/(name+'.stderr'),'w')
        try:
            proc=subprocess.Popen(argv,stdout=out,stderr=err,env=env)
        finally:
            out.close();err.close()
        jobs.append(proc)
        return proc
    try:
        save()
        remote=subprocess.check_output(ssh+['mktemp -d /tmp/rrl-wait-endurance.XXXXXX'],text=True,timeout=15).strip()
        assert remote.startswith('/tmp/rrl-wait-endurance.')
        state['remote_dir']=remote
        config=root/'experiments/configs/wait_policy_endurance.json'
        subprocess.run(scp+[str(build/'rrl_workload_runner'),str(config),'dell@10.102.35.67:'+remote+'/'],check=True,timeout=30)
        remote_hash=subprocess.check_output(ssh+['sha256sum '+remote+'/rrl_workload_runner'],text=True,timeout=15).split()[0]
        assert remote_hash==state['binary_sha256']
        state['config']=json.loads(config.read_text())
        tasks=[]
        for policy in ('wait-die','wound-wait'):
            for cc in ('hierarchical','predicate'):
                name='local-'+cc+'-'+policy
                argv=[str(build/'rrl_workload_runner'),'--config',str(config),'--cc',cc,'--wait-policy',policy,
                      '--duration',str(args.seconds),'--latency-histogram',str(logs/(name+'.hist.json')),'--format','csv']
                tasks.append((name,launch(argv,name),None))
            rdma,control=port(),port()
            for binary,listen in [('rrl_rdma_lock_server',rdma),('rrl_wait_coordinator',control)]:
                proc=subprocess.Popen([str(build/binary),str(listen)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
                servers.append(proc)
                assert select.select([proc.stdout],[],[],15)[0],binary+' readiness timeout'
                line=proc.stdout.readline()
                (logs/(policy+'-'+binary+'.start')).write_text(line)
                assert 'listening' in line,line
            name='cross-rdma-next-key-'+policy
            hist=remote+'/'+name+'.hist.json'
            argv=['timeout',str(args.seconds+30),'env','RRL_RDMA_GID_INDEX=3',remote+'/rrl_workload_runner','--rdma-lock-only','1',
                  '--config',remote+'/wait_policy_endurance.json','--cc','rdma-next-key','--wait-policy',policy,
                  '--duration',str(args.seconds),'--latency-histogram',hist,'--format','csv',
                  '--rdma-host','10.0.0.3','--rdma-port',str(rdma),'--coordinator-port',str(control)]
            tasks.append((name,launch(ssh+[shlex.join(argv)],name),(hist,rdma,control)))
        state.update(status='RUNNING',started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                     pids=[j.pid for j in jobs])
        save()
        print('RUNNING '+str(logs),flush=True)
        def collect(task):
            name,proc,remote_info=task
            rc=proc.wait(timeout=args.seconds+60)
            assert rc==0,(name,rc,(logs/(name+'.stderr')).read_text())
            row=validate((logs/(name+'.stdout')).read_text())
            assert int(row['elapsed_ns'])>=args.seconds*1_000_000_000,(name,'early stop')
            if remote_info:
                hist,rdma,control=remote_info
                subprocess.run(scp+['dell@10.102.35.67:'+hist,str(logs/(name+'.hist.json'))],check=True,timeout=30)
                # A fresh independent-session contract checks the physical table
                # is reusable and empty after the full endurance run.
                testenv=dict(env,RRL_TEST_RDMA_HOST='127.0.0.1',RRL_TEST_RDMA_PORT=str(rdma),RRL_TEST_COORDINATOR_PORT=str(control))
                run=subprocess.run([str(build/'rrl_rdma_wait_policy_test')],env=testenv,capture_output=True,text=True,timeout=30)
                (logs/(name+'.post-contract.stdout')).write_text(run.stdout)
                (logs/(name+'.post-contract.stderr')).write_text(run.stderr)
                assert run.returncode==0,(name,'post-contract',run.stderr)
            data=json.loads((logs/(name+'.hist.json')).read_text())
            for k,h in data['histograms'].items():
                assert h['count']==sum(b['count'] for b in h['buckets'])==int(row[k+'_count'])
            return {'name':name,'status':'PASS','elapsed_ns':row['elapsed_ns'],'committed':row['committed'],
                    'logical_unfinished':row['logical_unfinished'],'retries':row['retries']}
        with ThreadPoolExecutor(max_workers=len(tasks)) as pool:
            futures=[pool.submit(collect,t) for t in tasks]
            for f in as_completed(futures):
                cases.append(f.result());save()
                print('PASS '+cases[-1]['name'],flush=True)
        state.update(status='PASS',finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        save()
        print('PASS '+str(logs),flush=True)
    except BaseException as e:
        state.update(status='FAIL',error=repr(e));save();raise
    finally:
        for proc in jobs+servers:
            if proc.poll() is None:
                proc.terminate()
        for proc in jobs+servers:
            try:proc.wait(timeout=10)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        # Remote runs are additionally bounded by their own timeout, including
        # when the SSH transport dies. No broad kill/process-name matching.
        print('own verification services stopped; logs: '+str(logs),flush=True)


if __name__=='__main__':
    main()
