#!/usr/bin/env python3
import argparse,csv,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=argparse.ArgumentParser();p.add_argument('main',type=Path);p.add_argument('root',type=Path);p.add_argument('supplement',type=Path,nargs='*');p.add_argument('--layout',type=Path);a=p.parse_args()
main=list(csv.DictReader((a.main/'results.csv').open()))
for directory in a.supplement:main.extend(csv.DictReader((directory/'results.csv').open()))
root=list(csv.DictReader((a.root/'results.csv').open()))
layout=list(csv.DictReader((a.layout/'results.csv').open())) if a.layout else []
for r in main+root+layout:
 assert all(int(r[f])==0 for f in ('index_errors','lock_errors','lock_release_failures','failed_transactions'))
 assert int(r['transactions'])==int(r['committed'])+int(r['aborted'])
 assert int(r['transactions'])==int(r['logical_started'])+int(r['retries'])
 assert int(r['logical_started'])==int(r['committed'])+int(r['logical_unfinished'])
threads=(1,2,4,8,16,32,64);widths=(16,64,256);policies=('no-wait','wait-die','wound-wait')
def select(rs,policy,t,width,backend,root_variant=None):
 return [r for r in rs if r['wait_policy']==policy and int(r['threads'])==t and
  int(r.get('hierarchy_bucket_width',256))==width and r['hierarchy_lock_table']==backend and
  (root_variant is None or r.get('root_variant',str(root_variant))==str(root_variant))]
def median(rs,field):return statistics.median(float(r[field]) for r in rs)
fig,axes=plt.subplots(3,3,figsize=(12,10),sharex=True,layout='constrained')
colors={16:'#00866a',64:'#d97706',256:'#2563eb'}
for i,policy in enumerate(policies):
 for j,width in enumerate(widths):
  ax=axes[i,j]
  for backend,style in [('generic','--'),('compact','-')]:
   samples=[[float(r['throughput_committed_txn_s']) for r in select(main,policy,t,width,backend)] for t in threads]
   assert all(len(s)==3 for s in samples),(policy,width,backend,[len(s) for s in samples])
   ax.plot(threads,[statistics.median(s) for s in samples],style+'o',color=colors[width],
      label=f'{backend} (width {width})' if j==0 else backend,linewidth=1.7,markersize=4)
   ax.fill_between(threads,[min(s) for s in samples],[max(s) for s in samples],color=colors[width],alpha=.12)
  ax.set_yscale('log');ax.set_xticks(threads);ax.grid(True,which='both',alpha=.2)
  ax.set_title(f'{policy} / bucket {width}',fontsize=10)
  if j==0:ax.set_ylabel('Committed transactions / second')
  if i==2:ax.set_xlabel('OS worker threads')
axes[0,0].legend(fontsize=8);fig.suptitle('YCSB-E hierarchical locking, measured compact root strategy\n100k keys; 10 ops/txn; range 32; Zipf 0.99; 3-trial median; 1 s warmup + 2 s measurement',fontsize=12)
fig.savefig(a.main/'throughput.png',dpi=180);fig.savefig(a.main/'throughput.svg');plt.close(fig)
lines=['# 固定桶层级锁 CAS 性能结果','',
 '机器：10.102.35.66，双路 Xeon Gold 5218，32 物理核 / 64 逻辑 CPU。分支 `codex/record2pl-compact-20261112`。',
 f'',f'主实验 {len(main)} 次，root CAS 对照；root 专项 {len(root)} 次。每个配置 3 轮，结果取中位数，保留 min/max。',
 '', '主实验为项目 YCSB-E：100,000 keys，5,000 条有限 trace，每事务 10 操作、扫描宽 32、Zipf 0.99、seed 7；1/2/4/8/16/32/64 OS threads；bucket 16/64/256；no-wait/wait-die/wound-wait；warmup 1 秒，duration 2 秒。Generic 与 compact 运行相同参数，后端交错运行。此持续 trace 会重复访问 key，不能把每个 Insert 当作新物理插入。',
 '', '主实验显式固定 compact 为 root CAS + dense node word；root 消除和 padding 另作专项对照。此持续 trace 会重复访问 key，不能把每个 Insert 当作新物理插入。',
 '', '![吞吐曲线](throughput.png)', '',
 '## 宽度 256 的吞吐', '', '| 策略 | 线程 | generic txn/s | compact txn/s | compact root模式 | 倍率 |', '|---|---:|---:|---:|---|---:|']
for policy in policies:
 for t in threads:
  g=median(select(main,policy,t,256,'generic'),'throughput_committed_txn_s')
  c=median(select(main,policy,t,256,'compact'),'throughput_committed_txn_s')
  lines.append(f'| {policy} | {t} | {g:,.1f} | {c:,.1f} | CAS | {c/g:.2f}× |')
lines+=['','## 64 线程重试情况', '',
 '| 宽度 | 策略 | 后端 | 中止/提交 | 重试/提交 | 等待 ms/提交 | 未完成 | 重试耗尽 | deadline 中止 | CAS 重试/提交 |',
 '|---:|---|---|---:|---:|---:|---:|---:|---:|---:|']
for width in widths:
 for policy in policies:
  for backend in ('generic','compact'):
   rs=select(main,policy,64,width,backend)
   ratio=lambda field:statistics.median(float(r[field])/max(1,int(r['committed'])) for r in rs)
   lines.append(f'| {width} | {policy} | {backend} | {ratio("aborted"):.2f} | {ratio("retries"):.2f} | {ratio("lock_wait_total_ns")/1e6:.3f} | {median(rs,"logical_unfinished"):.0f} | {median(rs,"retry_exhausted"):.0f} | {median(rs,"deadline_aborts"):.0f} | {ratio("hierarchy_cas_retries"):.1f} |')
lines+=['','## Root CAS 优化', '',
 'root 专项固定 width=256，对每种策略和 1/8/32/64 threads 将 compact 的 root word CAS 与隐式 IS/IX root 比较，三轮。Generic root 不在此专项重复测试；它在主实验中按原行为执行。', '',
 '| 策略 | 线程 | root CAS txn/s | implicit root txn/s | 提升 | CAS 重试/提交：root | CAS 重试/提交：implicit |',
 '|---|---:|---:|---:|---:|---:|---:|']
for policy in policies:
 for t in (1,8,32,64):
  r0=select(root,policy,t,256,'compact',0);r1=select(root,policy,t,256,'compact',1)
  x=median(r0,'throughput_committed_txn_s');y=median(r1,'throughput_committed_txn_s')
  q0=statistics.median(float(r['hierarchy_cas_retries'])/max(1,int(r['committed'])) for r in r0)
  q1=statistics.median(float(r['hierarchy_cas_retries'])/max(1,int(r['committed'])) for r in r1)
  lines.append(f'| {policy} | {t} | {x:,.1f} | {y:,.1f} | {y/x:.2f}× | {q0:.1f} | {q1:.1f} |')
if layout:
 lines+=['','## Padding 与统计开销','',
  'width=256、root implicit、32/64 threads、三策略，各配置三轮；padding 直接对比 dense，fast 关闭可选 latency/statistics。','',
  '| 策略 | 线程 | dense/full txn/s | padded/full txn/s | padding倍率 | dense/fast txn/s | fast倍率 | node内存倍率 |',
  '|---|---:|---:|---:|---:|---:|---:|---:|']
 for policy in policies:
  for t in (32,64):
   vals={name:[r for r in layout if r['wait_policy']==policy and int(r['threads'])==t and r['layout_variant']==name]
         for name in ('dense-full','padded-full','dense-fast')}
   x=median(vals['dense-full'],'throughput_committed_txn_s')
   y=median(vals['padded-full'],'throughput_committed_txn_s')
   z=median(vals['dense-fast'],'throughput_committed_txn_s')
   mem=median(vals['padded-full'],'hierarchy_lock_bytes')/max(1,median(vals['dense-full'],'hierarchy_lock_bytes'))
   lines.append(f'| {policy} | {t} | {x:,.1f} | {y:,.1f} | {y/x:.2f}× | {z:,.1f} | {z/x:.2f}× | {mem:.1f}× |')
lines+=['','## 范围与限制','',
 '- Compact 默认省去 intention-only root 的共享 CAS；root 专项仍保留显式 root CAS 对照。该特化只适用于计划中 root 始终为 IS/IX 的固定桶配置。',
 '- 五模式 node authority 是 8 bytes；无竞争的一次获取需要一次成功 CAS。有竞争时 CAS 可重试。wait-die/wound-wait 的冲突路径使用预分配 lease bytes 找到 holder，no-wait 不分配 lease。',
 '- 桶 S 保护范围内的空洞，防止 phantom；桶内 point write 仍为 IX + record X。',
 '- bucket width 会改变冲突粒度；表格按宽度分开报告，不跨宽度宣称锁管理器速度。',
 '- 保留 B+ 树 latch、数据写回、transaction age ticket 和冲突时 lease 扫描。CAS 路径没有消除真实热点上的数据竞争。',
 '- 64-context 所有共享模式通过状态字精确表示，没有把 slot 63 的 S 转为 X。',
 '- 操作、行和 checksum 统计包含已中止尝试；中止/重试/未完成同时列示。generic 的 compact 内存字段为 0 表示未采样。',
 '- 主实验中有一次 generic wound-wait run 触发 lock_errors；原运行保留在逐次日志中。原命令立即重跑成功，summary 用成功重跑值替代失败样本，详见 `retries.json`。',
 '- 原始 CSV、每个 run 的命令和 stdout/stderr、环境与源码快照保存在对应报告目录；ROOT 专项结果在 `../hierarchy-root-ycsbe-20261113`。',
 '- 实现限制和正确性路径见 `../../docs/notes/hierarchy-cas-implementation-2026-11-13.md`。','']
(a.main/'REPORT.md').write_text('\n'.join(lines),encoding='utf-8')
print(a.main/'REPORT.md')
