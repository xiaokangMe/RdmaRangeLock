#!/usr/bin/env python3
"""Actual-leaf correctness and directed runtime checks. Never a formal matrix."""
import argparse,datetime,hashlib,json,os,shlex,shutil,subprocess
from pathlib import Path
from shared_rdma_services import SharedServices
from run_shared_matrices import validate_row

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--build',default='build-shared');p.add_argument('--cross-host',action='store_true')
 p.add_argument('--contracts-only',action='store_true');p.add_argument('--endurance',type=int,default=0)
 p.add_argument('--only',choices=('local','cross','all'),default='all')
 a=p.parse_args()
 if not 0<=a.endurance<=86400:raise ValueError('duration bounds')
 if a.only=='cross' and not a.cross_host:raise ValueError('cross requires --cross-host')
 root=Path(__file__).resolve().parents[1];os.chdir(root)
 logs=root/'experiments/logs'/('leaf-check-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));logs.mkdir(parents=True)
 build=(root/a.build).resolve();pinned=logs/'binaries';pinned.mkdir()
 names=('rrl_workload_runner','rrl_rdma_data_server','rrl_rdma_lock_server','rrl_rdma_txn_server','rrl_leaf_native_test','rrl_leaf_object_test')
 hashes={}
 for name in names:
  digest=hashlib.sha256((build/name).read_bytes()).hexdigest();shutil.copy2(build/name,pinned/name)
  if digest!=hashlib.sha256((pinned/name).read_bytes()).hexdigest() or digest!=hashlib.sha256((build/name).read_bytes()).hexdigest():raise RuntimeError('binary changed while pinning')
  hashes[name]=digest
 build=pinned
 state=dict(status='RUNNING',formal=False,pid=os.getpid(),source_commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),binary_sha256=hashes,cases=[])
 def save():
  temp=logs/'state.tmp';temp.write_text(json.dumps(state,indent=2));temp.replace(logs/'state.json')
 save();print(logs,flush=True)
 env={k:v for k,v in os.environ.items() if not k.startswith('RRL_')};env['RRL_RDMA_GID_INDEX']='3'
 ssh=['ssh','-o','BatchMode=yes','-o','IdentitiesOnly=yes','-i',str(Path.home()/'.ssh/rdmarangelock_deploy'),'dell@10.102.35.67']
 remote=None
 if a.cross_host:
  remote=subprocess.check_output(ssh+['mktemp -d /tmp/rrl-leaf-check.XXXXXX'],text=True).strip()
  if not remote.startswith('/tmp/rrl-leaf-check.'):raise RuntimeError('unsafe remote directory')
  subprocess.run(['scp']+ssh[1:-1]+[str(build/n) for n in ('rrl_workload_runner','rrl_leaf_native_test')]+[ssh[-1]+':'+remote+'/'],check=True)
  for name in ('rrl_workload_runner','rrl_leaf_native_test'):
   if subprocess.check_output(ssh+['sha256sum '+shlex.quote(remote+'/'+name)],text=True).split()[0]!=hashes[name]:raise RuntimeError('remote binary differs')
 base=json.loads((root/'experiments/configs/rdma_shared_smoke.json').read_text())
 base.update(workload='ycsb-a',cc='rdma-hierarchical',hierarchy_layout='leaf',key_count=128,transaction_count=100,operations_per_transaction=4,zipf=0,order=8,threads=2,coroutines=2,warmup_seconds=1)
 def config(label,cfg):
  path=logs/(label+'.json');path.write_text(json.dumps(cfg,indent=2));return path
 def command(binary,flags,e,cross=False,path=None):
  runenv=dict(e)
  if path:
   if cross:
    subprocess.run(['scp']+ssh[1:-1]+[str(path),ssh[-1]+':'+remote+'/config.json'],check=True,stdout=subprocess.DEVNULL)
    runenv['RRL_TEST_CONFIG']=remote+'/config.json'
   else:runenv['RRL_TEST_CONFIG']=str(path)
  cmd=[(remote if cross else str(build))+'/'+binary]+flags
  if cross:cmd=ssh+[shlex.join(['env']+[k+'='+str(v) for k,v in runenv.items() if k.startswith('RRL_')]+cmd)]
  return cmd,runenv
 def launch(cmd,e,label):
  (logs/(label+'.command.json')).write_text(json.dumps(cmd))
  return subprocess.Popen(cmd,env=e,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 def collect(peer,label,timeout=90):
  try:out,err=peer.communicate(timeout=timeout)
  except BaseException:peer.kill();peer.wait();raise
  (logs/(label+'.stdout')).write_text(out);(logs/(label+'.stderr')).write_text(err)
  if peer.returncode:raise RuntimeError((label,peer.returncode,out[-1500:],err[-2000:]))
  return out
 try:
  if not a.endurance:
   for mode in ('clean','running','committing','gate'):
    label='fault-'+mode;path=config(label,base)
    with SharedServices(build,path,logs/label,env,native=True) as svc:
     cmd,e=command('rrl_leaf_native_test',[label],svc.test_env('10.0.0.3' if a.cross_host else '127.0.0.1'),a.cross_host,path)
     collect(launch(cmd,e,label),label)
     if mode=='clean':svc.check()
     elif svc.processes[1].wait(timeout=10)!=2:raise RuntimeError('object service failed open')
     if any(peer.poll() is not None for i,peer in enumerate(svc.processes) if i!=1):raise RuntimeError('unrelated data/state failure')
    state['cases'].append(dict(name=label,status='PASS'));save();print('PASS',label,flush=True)
   for policy in ('no-wait','wait-die','wound-wait'):
    label='exact-'+policy;path=config(label,base)
    with SharedServices(build,path,logs/label,env,native=True) as svc:
     peers=[]
     try:
      for n,cross in enumerate((False,a.cross_host)):
       cmd,e=command('rrl_leaf_native_test',['increment',policy,'50'],svc.test_env('10.0.0.3' if cross else '127.0.0.1'),cross,path)
       peers.append((launch(cmd,e,label+'-'+str(n)),label+'-'+str(n)))
      for peer,name in peers:collect(peer,name)
     finally:
      for peer,_ in peers:
       if peer.poll() is None:peer.kill();peer.wait()
     cmd,e=command('rrl_leaf_native_test',['read','100'],svc.test_env(),False,path)
     collect(launch(cmd,e,label+'-sum'),label+'-sum');svc.check()
    state['cases'].append(dict(name=label,status='PASS'));save();print('PASS',label,flush=True)
  if not a.contracts_only:
   modes=['local']+(['cross'] if a.cross_host else [])
   if a.only!='all':modes=[a.only]
   for mode in modes:
    for workload in (('ycsb-e',) if a.endurance else ('ycsb-a','ycsb-e','tpcc-payment','tpcc-new-order')):
     for policy in (('wound-wait',) if a.endurance else ('no-wait','wait-die','wound-wait')):
      shared=mode=='cross';label=mode+'-'+workload+'-'+policy;seconds=a.endurance or 1
      cfg=dict(base,workload=workload,wait_policy=policy,cc='rdma-hierarchical' if shared else 'hierarchical',duration_seconds=seconds)
      path=config(label,cfg)
      flags=['--config',str(path),'--index','rdma' if shared else 'v2','--format','csv']
      if shared:
       with SharedServices(build,path,logs/label,env,native=True) as svc:
        # Config file is copied once; only the host/ports differ.
        flags[1]=remote+'/config.json'
        flags+=['--rdma-host','10.0.0.3','--rdma-port',str(svc.lock_port),'--data-port',str(svc.data_port),'--txn-state-port',str(svc.txn_state_port)]
        cmd,e=command('rrl_workload_runner',flags,env,True,path)
        out=collect(launch(cmd,e,label),label,seconds+180);svc.check()
      else:
       cmd,e=command('rrl_workload_runner',flags,env);out=collect(launch(cmd,e,label),label,seconds+180)
      row=validate_row(out,seconds,shared,'native-rdma-state' if shared else None,'leaf')
      if int(row['committed'])<=0:raise RuntimeError('no progress in bounded leaf validation')
      state['cases'].append(dict(name=label,status='PASS',committed=row['committed'],retries=row['retries']));save();print('PASS',label,flush=True)
  state['status']='PASS';save();print('PASS;',len(state['cases']),'actual-leaf checks;',logs,flush=True)
 except BaseException as error:
  state.update(status='FAIL',error=repr(error));save();raise
if __name__=='__main__':main()
